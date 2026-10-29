# 10 · Remote：对外运行时接口层

让外部程序（MCP、LSP、DAP、Profiler 前端、自动化测试、编辑器 ↔ Game/Server/devkit）读取运行时信息、操纵引擎。

## 1. 模块与范围

- `Source/Runtime/Remote`：Server 端，以 `RemoteSubsystem`（EngineSubsystem）的形式存在。
- **决策**：在所有非 Shipping 构建的 Editor / Game / Server / Program 中都启用；Shipping 构建**不链接**这个模块，用的是模块依赖，而不是宏。
  做法是 `Launch` 依赖一个空的接口模块，Shipping 的 launch module 不依赖 Remote。
- 默认只监听本机；远程连接（主机 devkit、其他机器）需要在 Config 中启用，并携带 token。

## 2. 传输与协议

**决策**：JSON-RPC 2.0，加一个可选的二进制附件通道。

- 传输层：本机用命名管道或 Unix domain socket（`\\.\pipe\Marlin.<Pid>` 以及一个按项目命名的发现管道）；
  远程用 TCP 和 WebSocket（浏览器工具）。
- 消息帧：`Content-Length` 头加 JSON（与 LSP 相同），这样 LSP、DAP 的转发层可以非常薄。
- 二进制附件：大数据（内存快照、帧捕获、截图、纹理回读）在 JSON 中返回 `{"attachment": id, "size": n}`，数据从同一连接的二进制帧或共享内存中读取。
- 进程发现：每个进程在 `/Temp/../Marlin/Processes/<Pid>.mtext` 写入自己的身份（Target、项目、端口、World 列表），退出时删除。`MarlinMcp` 通过它枚举可以连接的进程。

## 3. 方法注册

```cpp
MFUNCTION(Remote, Category="World")
static List<EntityInfo> QueryEntities(WorldId world, StringView componentFilter, int32 limit);
```

- 方法由 HeaderTool 从 `MFUNCTION(Remote)` 生成：方法名是 `World.QueryEntities`，参数和返回值通过反射完成 JSON 编解码；同时生成 JSON Schema（给 MCP tool 描述用）。
- 线程模型：默认在主 fiber 的帧边界执行（安全）；标了 `[Remote(AnyThread)]` 的方法可以并发执行。
- 推送：订阅 `Log.Stream`、`Profiler.Frames`、`Asset.Changed`、`Script.Breakpoint` 等事件流。

## 4. 内建方法分组

| 分组 | 示例 |
| --- | --- |
| `Engine` | Info、Targets、Quit、`ExecConsole`、CVar 读写 |
| `World` | 列出 World、实体查询、读写组件（反射 JSON）、生成和销毁、暂停和单步 |
| `Asset` | 通过 Indexer 查询、加载状态、强制重载 |
| `Editor` | 执行 Command（09 §4.3）、打开资产、选择、截图视口 |
| `Script` | 编译、热重载、调试（DAP 后端） |
| `Render` | RenderGraph 导出、纹理回读、GPU 统计 |
| `Memory` / `Profiler` / `Log` | 快照、帧数据、日志流 |
| `Test` | 运行测试、收集结果（11） |

## 5. 适配器程序

| Program | 作用 |
| --- | --- |
| `MarlinMcp` | MCP 服务器（stdio），把 Remote 方法加上 JSON Schema 映射成 MCP tools 和 resources，可以同时连接多个引擎进程 |
| `MarlinLanguageServer` | LSP：前端在进程内运行，符号和类型查询依赖 Indexer 和运行中的编辑器（反射声明） |
| `MarlinDebugAdapter` | DAP：转发到目标进程的 `Script.*` 方法 |

Asset Indexer 守护进程也使用同一套协议和传输层，所以客户端只需要一套实现。

## 6. 实现现状

模块 `Runtime/Remote`（依赖 `Runtime/Json`）。`Launch` 在非 Shipping 配置下才依赖它，所以 Shipping 的 Game 里没有 Remote。

- **传输**：每个进程默认只开本机命名管道 `Marlin.<Pid>`。`Engine.Remote` 段的 `Port` 不为零时再监听 TCP，默认只绑定回环地址；
  `AllowRemote = true` 才绑定所有网卡，并且必须设置 `Token`，否则不开 TCP。同一个端口既接受 `Content-Length` 流，也接受 WebSocket：
  连接的头 4 个字节是 `GET ` 就做 WebSocket 握手（RFC 6455）。连接、监听和收发都走 Net（14）：帧在 IO 线程上解析，没有每个连接一个的线程。
- **认证**：命名管道上的连接直接可用；TCP 和 WebSocket 的连接在服务器有 token 时，必须先调用 `Remote.Authenticate { Token }`，否则所有调用都返回 `-32001`。
- **帧**：字节流上每帧是 `Content-Length` 头加内容；附件多一个 `Marlin-Attachment: <id>` 头。WebSocket 上消息是文本帧，附件是二进制帧，前 8 字节是 id。
  附件在引用它的响应之前发出；结果里写成 `{"Attachment": id, "Size": n}`（反射字段名，和其他 JSON 一样用 PascalCase）。
- **方法**：服务器启动时收集所有反射类型上的 `MFUNCTION(Remote)`。名字是 `Category.Function`，`Category` 取函数或类型的元数据，没有就用类型名。
  静态函数直接调用；成员函数只允许写在 Subsystem 上，调用时找当前运行的那个实例。参数按名字（对象）或位置（数组）传，
  JSON 里的参数名是 C++ 参数名首字母大写；有默认值的参数可以省略。非 const 引用参数是输出，结果变成 `{"Result": 返回值, "<输出名>": …}`。
  参数里可以有一个 `RemoteCall &`：用它报错（`Fail`）、发附件（`Attach`）或取得连接 id。代码里也可以用 `RemoteServer::AddMethod` 注册原生处理函数。
- **线程**：默认在主线程的帧开始时执行（`RemoteSubsystem::BeginFrame` 调用 `RemoteServer::ProcessPending`）；`MFUNCTION(Remote = AnyThread)` 收到后立即作为 job 执行（`Jobs::Run`，Job 系统没启动时在 IO 线程上内联执行），不占用 IO 线程。
  支持 JSON-RPC 的批量请求和通知（没有 id 的请求不回复）。
- **内建方法**：`Remote.Authenticate`、`Remote.Subscribe { Stream }`、`Remote.Unsubscribe`、`Remote.ListStreams`、
  `Remote.ListMethods`（每个方法带参数和结果的 JSON Schema，类型定义放在 `$defs`），`Engine.Info`、`Engine.Quit`、`Engine.ExecConsole`、`Engine.GetVariable`。
- **推送**：`RemoteServer::Publish(stream, params)` 给订阅了这个流的连接发 JSON-RPC 通知，方法名就是流名。已有 `Log.Stream`（每条日志一个通知；`RemoteLogStream` 是它的 sink，转发别的进程的记录时带 `Source`，`RemoteLogStream::Relog` 把收到的记录记进本进程）。`RemoteClient::NotifyJson` 发已经写成 JSON 文本的参数，不经过 `JsonValue`。
- **发现**：进程启动时写 `<系统临时目录>/Marlin/Processes/<Pid>.mtext`（`RemoteProcessInfo`：Pid、Target、EngineId、Project、Pipe、Port），退出时删除；
  `RemoteDiscovery::List` 会顺便删掉已退出进程留下的文件。按项目命名的发现管道还没有做，目前用这些文件。
- **客户端**：`RemoteClient` 可以连管道、TCP 或 WebSocket（`GetPipeName` 返回最近一次连上的管道名，用别的方式连接后为空）；`Call` 的回调和通知在调用 `ProcessPending` 的线程上执行，`CallSync` 阻塞等待。
  Asset Indexer 守护进程和各个工具都用它。
- **Render 组**：RHI 提供 `Render.GpuInfo`（后端、适配器、特性、device layer、capture 工具）、`Render.GpuMemory`（驱动预算和 RHI 分配器各池的占用）、
  `Render.Capture { Frames }`；模块 `Runtime/RenderGraphRemote` 提供 `Render.Graph`（下一个执行完的图编译后的导出：pass 及其访问、barrier 和等待、资源及其放置、提交；
  图执行完才回复，进程里没有 RenderGraph 时直接报错）和 `Render.CapturePasses { First, Last }`（用 capture 工具截下一个图里从 First 到 Last 的 pass）。纹理回读尚未实现。
- 尚未实现：共享内存附件、`MFUNCTION(Remote)` 的 HeaderTool 期检查（现在启动时检查）、其余模块的内建方法组（World、Asset、Editor、Script、Memory、Profiler、Test）、
  `MarlinMcp` 等适配器程序。
