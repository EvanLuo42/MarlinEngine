# 01 · Target、生命周期与主循环

## 1. Target 类型

`marlin/build/model.py` 中已有 `TargetType { Game, Editor, Server, Program }`。构建模型里没有 Plugin 这一层：扩展是普通模块，
由 target 规则的 `extra_modules` 决定链接进哪些 Target（12 §3）。

| Target | 可执行文件 | 链接的模块 | 说明 |
| --- | --- | --- | --- |
| **Editor** | `Editor` | Runtime + Developer + Editor 模块 | 只有 Windows/Linux/macOS |
| **Game** | `Game` | Runtime 模块 | 一个引擎 fork 只编出一个 Game，所有项目共用 |
| **Server** | `Server` | Runtime 中除 RHI 后端、UIPainter GPU 后端、音频输出、`Application`（窗口与输入，12 §1.3）以外的部分 | headless，主要跑在 Linux 上 |
| **Program** | `MarlinXxx` | Core 加按需选的模块 | 命令行工具和守护进程 |

裁剪靠的是**模块依赖图**，不是宏。比如 Server 的 launch module 是 `ServerLaunch`，它不依赖 `RHID3D12`，
渲染相关的 Subsystem 也就不会被注册（Subsystem 的发现机制见 §3）。

没有被任何 Target 链接的模块（例如还没有 Game / Editor Target 时的 `Application`），toolkit 也会在每个已生成的构建组合的共享环境里
单独解析它：它有自己的 VS 工程和 compile_commands 条目，可以用 `marlin build <Module>-<Platform>-<Arch>-<Config>-Shared` 单独编译（已实现）。

### 1.1 Program 列表（初版）

| Program | 职责 |
| --- | --- |
| `MarlinHeaderTool` | 扫描反射标记，生成 `.gen.h/.gen.cpp`（04） |
| `MarlinAssetIndexer` | 每台机器、每个项目一个常驻守护进程，负责资产索引与文件监控（06） |
| `MarlinConverter` | 资产转换 worker，由 Indexer 或 Editor 调度（06） |
| `MarlinShaderCompiler` / `MarlinScriptCompiler` | 语言编译器的命令行入口（08） |
| `MarlinLanguageServer` / `MarlinDebugAdapter` | LSP / DAP，背后连接 Remote 层（10） |
| `MarlinMcp` | MCP 服务器，把 Remote 层的方法翻译成 MCP tool（10） |
| `MarlinCrashReporter` | 进程外写 minidump 并上报 |
| `MarlinFileServer` | 主机 devkit 通过网络读取 PC 上的源文件和 Config（02）；转换结果直接从 GlobalStore 读（06 §4.2） |
| `MarlinTests` | 合并的测试 runner（11） |
| `MarlinPackager` | cook 加 `.mpak` 打包（06） |

### 1.2 Program 的语言规则

**决策**：引擎的 Runtime / Developer 模块被所有 Target 共享，始终是 no-exceptions、no-RTTI、无 std。
Program **专属**的模块可以在 `*.module.py` 里声明 `allow_exceptions = True`、`allow_rtti = True`、`allow_std = True`，
用来接入 libclang、压缩库这类第三方代码。和 Runtime 交界的地方必须 `noexcept`，不允许异常穿过去。

## 2. 进程启动流程

所有 Target 共用 `Launch` 模块里的 `LaunchMain`，差别只在于它跑到哪一步为止。

```
0  进程入口           平台层的 main/WinMain → LaunchMain(argc, argv)
                      macOS：进程主线程留给 AppKit（窗口线程），LaunchMain 另起引擎主线程跑后面的步骤（12 §1.5）
1  Core 早期初始化     堆分配器、MemoryTag、TLS、高精度时钟、CommandLine 解析
2  定位引擎            EngineRoot = 可执行文件所在目录向上 4 级 (Binaries/<Platform>/<Arch>/<Config>/)
                      有 Build/InstalledBuild → Installed，否则 Source；推出 EngineId（02 §1）
3  基础挂载            /Engine  /Cache  /Temp（02）
4  日志与崩溃          结构化日志开始写；连接或拉起 MarlinCrashReporter
5  反射注册            调用生成的模块表 MarlinModuleTable[]，按依赖拓扑序执行每个模块的
                      Register_<Module>()，只写元数据，不执行任何业务逻辑（04）
6  解析项目            Editor/Game(dev)：-Project=；Editor 没有时进入项目浏览器（§5.1）；Shipping：exe 同目录的 Product.mtext
                      校验 .mproject 的 Engine 绑定
7  项目挂载            /Project  /User（02）
8  Config 层叠         按 Engine → 平台层栈 → Project → 项目平台层 → User → 命令行 的顺序合并（06 §1.4）
9  Engine Subsystem   按依赖拓扑启动（§3）
10 Target 专属         Editor：Editor Subsystem、恢复布局、连接 Indexer
                      Game：加载 StartupLevel
                      Server：开始监听
11 主循环              §4
12 关闭                逆序 Deinitialize → 刷新日志 → 卸载 → 退出
```

Program 默认只走到第 5 步。需要更多功能时，调用 `ProgramHost::Initialize(ProgramFeatures::Vfs | ProgramFeatures::Config | ...)`
按需开启。

**实现现状**（`Runtime/Engine`、`Runtime/Launch`）：

- `Launch` 模块只有入口：`main` 填一个 `LaunchDesc`（`TargetName`、`Features`、`RequiresProject`）后调用 `LaunchMain(desc)`。
  `LaunchMain` 和 `ProgramHost::Initialize/Shutdown` 是 `Engine.h` 里的 inline 包装，把当前可执行文件的 `GetModuleTable()` 传给
  `Engine::Startup(desc, modules)`，所以模块表留在各自的可执行文件里。`Engine::Shutdown` 按逆序关闭。
- `ProgramFeatures` 是标志位：`Vfs`（第 3 步，挂载 `/Engine`、`/User/Global`、`/Cache`、`/Temp`）、`Project`（第 6–7 步）、
  `Config`（第 8 步）、`Subsystems`（第 9 步）、`CrashReporter`（第 4 步）；`LaunchMain` 默认是 `All`，`MarlinTests` 只开 `Vfs`。
- 第 2 步不读任何文件：EngineRoot 是可执行文件目录向上 4 级；有 `Build/InstalledBuild` 标记文件就是 Installed，Id 是编进二进制的版本号；
  否则是 Source，Id 由路径计算（02 §3）。exe 旁边有 `Product.mtext` 就是 Packaged。结果放在 `Engine::GetEnvironment()`。
- 第 6 步：没有 `-Project=` 时 Game 照样启动（`/User` 指向 `<EngineRoot>/Saved/User`），`LaunchDesc::RequiresProject` 为 true 时报错退出。
  `.mproject` 的 `Engine` 绑定和当前引擎不一致时只是警告。
- 第 4 步：`Startup` 的第一行就装上崩溃处理器（报告先写到 `%LOCALAPPDATA%/Marlin/Crashes`）；`ProgramFeatures::CrashReporter`（`All` 包含它）时拉起 `MarlinCrashReporter`；没有它的 Program 也装崩溃回调，崩溃时在进程内写 dump（03 §7.1）。项目确定后，报告改写到 `/User/Crashes`，`Engine.CrashReport` 段在 Config 加载后交给崩溃上报。
- Packaged 形态在第 5 步之后挂载 exe 旁 `Paks/*.mpak` 里的每个包（`Engine::GetPackages()`，优先级从高到低）；压缩 codec 在反射注册后建好（`Engine::GetCompressionCodecs()`）。
- 第 10 步：所有 Engine Subsystem 初始化后，按初始化顺序调用 `EngineSubsystem::Start`；关闭时在任何 Subsystem Deinitialize 之前按逆序调用 `Stop`。需要整个引擎的作用域在这里启动，例如 Editor 的 `EditorHostSubsystem`（§3）。Game 和 Server 的启动关卡由 `WorldManagerSubsystem` 打开：`Engine.World` 段的 `LoadStartupLevel` 和 `StartupProfile`（`Config/Engine.mtext` 按 `@if Target` 只给 Game 和 Server 打开），Profile 按名字取自 `Config/WorldProfiles.mtext`（05 §1.1）。
- 第 9–10 步：每个 Subsystem 的 `Initialize` 和 `Start` 之前，Engine 把「Initializing <Subsystem> (k/N)」写进 `StartupProgress`，
  耗时长的同步操作由 Subsystem 自己写状态，供 Splash 显示（09 §4.6）。
- `-ExecCommands="a;b"` 在第一帧结束后执行控制台命令，用于自动化。
- 第 4 步之后，`LaunchDesc::Splash` 不为空时调它的 `Show`（Editor 传 `Splash::GetStartupSplash()`，09 §4.6）；`Shutdown` 开头调 `Hide`。
- 尚未实现：项目浏览器和选好项目后的重启（§5.1）、只索引引擎的 Indexer（06 §2），以及 `ProjectSubsystem` 的创建（基类已经有了）。
  计划顺序：只索引引擎的 Indexer → 项目浏览器和最近项目 → `MarlinLauncher` 的文件关联（02 §3）。Splash 已在 Windows 上实现。

## 3. Subsystem：生命周期的唯一单位

**决策**：模块只是**构建单元**，没有 Startup/Shutdown。凡是需要初始化、每帧更新或关闭的东西，都写成 **Subsystem**。

```cpp
MCLASS(Dependencies = "JobSubsystem, TransformSubsystem")
class PhysicsSubsystem final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    MFUNCTION()
    static bool ShouldCreate(const SubsystemContext &context);

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
};
```

| 作用域 | 基类 | 生命周期 | 举例 |
| --- | --- | --- | --- |
| Engine | `EngineSubsystem` | 进程 | Jobs、RHI、Renderer、Audio、ScriptVM、Remote、AssetManager |
| Project | `ProjectSubsystem` | 加载项目到关闭项目 | Localization、项目级 Config |
| World | `WorldSubsystem` | 每个 World 一份 | ECS 调度器、PhysicsScene、RenderScene、ScriptDomain、NetDriver |
| Editor | `EditorSubsystem` | Editor 进程（第 10 步启动，先于 Engine 作用域关闭） | AssetIndexerClient、SourceControl、Undo、EditorUI |
| AssetContext | `AssetContextSubsystem` | 每个打开的资产编辑上下文一份 | 预览 World、选择集 |

- **发现**：反射注册阶段收集所有 Subsystem 子类。哪个类存在，只取决于它所在的模块有没有被链接。
  抽象类和标了 `MCLASS(Abstract)` 的中间基类不会被创建。
- **顺序**：同作用域内按 `Dependencies` 元数据（逗号分隔的类型名）拓扑排序，同层按类型名排序，保证每次启动顺序一致；有环直接报错，
  并打印环路。依赖可以指向外层作用域（Project 依赖 Engine），这时只检查外层已经有它。依赖没有被创建时，依赖方也不创建，并打印警告。
  `OptionalDependencies` 只排顺序：名字对应的类型链接了并且被创建时排在它后面，没链接或没创建都不影响依赖方
  （`InputSubsystem` 要在 `ApplicationSubsystem` 泵完消息之后，但 Input 模块不链接 Application，15 §4）。
- **可选**：反射的 `MFUNCTION() static bool ShouldCreate(const SubsystemContext &)` 可以按 Config 或命令行决定是否实例化
  （比如 `ApplicationSubsystem` 遇到 `-Headless` 就不创建）。签名不对时启动直接报错。
- **Config**：`MCLASS(Config = "Engine.Jobs")` 的静态 `MPROPERTY` 字段在 Config 加载后、Subsystem 创建前就已经填好（06 §1.4）。
- **查找**：`Engine::GetSubsystem<T>()`，或者在 `Initialize` 里用 `context.Collection->Get<T>()`。
- **禁止静态构造器做事**：全局对象只能是常量初始化，可以用 `-Wglobal-constructors` 强制。

Editor 作用域由 `EditorCore` 里的 Engine Subsystem `EditorHostSubsystem` 承载：它在 `Start` 里创建 `EditorSubsystem`、在 `Stop` 里销毁，每个帧钩子都在 Engine 的同名钩子里调用，所以依赖可以指向任何 Engine Subsystem；用 `Editor::GetSubsystem<T>()` 查找。Editor 可执行文件的 launch module 是 `EditorLaunch`；`MarlinEditorTests` 是 Editor 类型的测试 Target，包含 Editor 模块的测试（`MarlinTests` 是 Program，装不下它们）。

已有的 Engine Subsystem：`JobSubsystem`（`Engine.Jobs`：`WorkerCount`、`ReservedCores`）、`LogSubsystem`（写 `/User/Logs/<Target>.log`，
每帧 `EndFrame` 时刷新）、`ApplicationSubsystem`（`Engine.Window`：`Width`、`Height`、`Fullscreen`；泵消息，主窗口关闭时请求退出）。

## 4. 主循环

**决策**：固定步长模拟，渲染帧率可变，渲染时做插值；不设专门的渲染线程，而是「提取 + 流水线」。

```
Frame N（主 fiber）
 ├─ PumpOsEvents / Input 采样
 ├─ while (accumulator >= FixedDelta)        // 默认 60 Hz，可在 Config 里调
 │     World.FixedUpdate: PrePhysics → Physics → PostPhysics → Gameplay → Net
 ├─ World.Update(variableDelta): Camera → Animation(姿态) → UI → Late
 ├─ Extract: ECS → RenderScene（按 alpha = accumulator / FixedDelta 插值 Transform）
 ├─ 等待 Frame N-1 的渲染任务释放 RenderScene 的双缓冲槽
 └─ Kick Render(N)（job 图，和 Frame N+1 的模拟并行执行）
        RenderGraph 构建 → 所有 SceneView → UIPainter → Present
```

- 每个 Phase 内部的 System 由调度器按读写冲突生成 DAG（05）。
- 每个 World 跑哪些 System 由它的 `WorldProfile` 决定（数据驱动，见 05 §1.1），没有写死的模式枚举。
  例如编辑器关卡用 `EditorEdit`，PIE 用 `Game`，资产预览用 `AssetPreview` 的派生 Profile。
- Server 没有 Extract 和 Render 两步。

**实现现状**：还没有 World，`Engine::RunLoop()` 只驱动 Engine Subsystem 的四个钩子：`BeginFrame(time)` → 按固定步长循环 `FixedUpdate(delta)`
→ `Update(time)` → `EndFrame(time)`。`Engine.Loop` 段：`FixedRate`（默认 60）、`MaxFrameRate`（0 表示不限，否则 `FramePacer` 按绝对截止时间限帧：在平台计时器上睡到截止前的余量，剩下的让出线程；Windows 用高精度可等待计时器）、
`MaxDelta`（单帧时间上限，默认 0.25 s，防止断点后追帧）。`Engine::RequestExit(code)` 结束循环；`-ExitAfterFrames=N` 跑满 N 帧后退出，用于冒烟测试。

## 5. 各 Target 的具体流程

### 5.1 Editor

1. 第 0–5 步，Splash 从这里开始显示（09 §4.6）。
   如果没有给 `-Project`，就进入**项目浏览器**：不挂载 `/Project`，只加载引擎的 Config 层，启动 Engine Subsystem，UI 只显示项目浏览器。
   - 内容：最近打开的项目（记在 `/User/Global`，02 §5.1）、打开一个 `.mproject`、新建项目。
   - UI 资源（UIPainter 的 shader、字体、图标）来自 `/Engine/Content`，由**只索引引擎的 Indexer** 转换（06 §2），结果同样进 GlobalStore，
     所以也给之后打开的所有项目预热了缓存。
   - **决策**：选好项目后，以 `-Project=<.mproject>` 启动一个新的 Editor 进程，当前进程退出（和 UE 的项目浏览器一样）。
     不在同一进程里切到项目：那时 Engine Subsystem 已经按只有引擎的 Config 初始化，项目的 Config（渲染设置、插件、`Engine.Window`……）
     要求重新初始化大量 Subsystem。这样每个编辑器进程从启动起就只有一个项目，第 6–10 步只走一遍；新进程再显示一次 Splash。
2. 有项目时：挂载 → Config → 连接本项目的 `MarlinAssetIndexer`（没有就拉起，用 lockfile 加命名管道互斥）。
3. Editor Subsystem 启动，打开常驻的 **Asset Browser** 面板，恢复上次打开的资产编辑上下文（09）。
4. 每帧：UI 事件 → Signal 传播 → 所有活动 World 更新 → 每个 World 提取一次 → 每个 SceneView 渲染 → UIPainter 合成 → Present（每个 OS 窗口一个 swapchain）。
5. 关闭：保存布局 → 关闭所有 AssetContext（有未保存内容时询问）→ 断开 Indexer（Indexer 自己继续常驻）。

### 5.2 Game（开发版）

`Game -Project=Foo.mproject [-Level=...] [-UserDir=...]`。内容来自 Indexer 加 GlobalStore，缺失的现场转换（02、06）。
也可以加 `-Cooked=<dir>`，直接从 cook 产物运行，用来验证打包前的数据。

### 5.3 Game（Shipping）

1. `<Product>.exe` 读同目录下的 `Product.mtext` → 产品身份、Chunk 清单。
2. 按优先级挂载 `Paks/*.mpak`：Patch > DLC > Language > Base。
3. 没有 Indexer、Converter、Remote、Console。
4. 加载 `Product.StartupLevel`。

### 5.4 Server

和 Game 一样，但 launch module 不同：没有 Extract/Render，`Net` Phase 驱动快照发送，按 `-Port`、`-Map` 启动。
打包时使用 **server cook 变体**：剥掉纹理 mip、音频这类只有客户端需要的数据（Converter 按 `TargetKind=Server` 输出）。
