# 14 · Net：传输层

引擎里所有走 socket 和本机管道的代码共用的一层：Remote（10）、GlobalStore（06 §3.3）、`MarlinFileServer`（02）、网络复制（05 §7）。
它只负责把字节可靠、高效地搬进搬出，协议（`Content-Length` 帧、WebSocket、GlobalStore 二进制头、复制快照）都写在上层。

## 1. 模块与范围

**决策**：模块 `Source/Runtime/Net`，依赖 Core，和 Engine（只为 `NetSubsystem`）。Game、Server、Editor、Program 都可以链接；Shipping 的 Game 也链接它（复制要用）。

- 上层不再自己开线程、调用 `PlatformSocket::Poll` 或阻塞的 `Receive`：连接、监听、收发都通过 Net。
- 每个协议自己的帧解析是一个纯函数式的状态机：喂进一段字节，吐出完整的帧和用掉的字节数。这样协议代码不关心字节从哪来，也容易单独测试。

## 2. 平台层：完成端口

**决策**：Net 建在**完成式**（completion-based）的 IO 上，而不是就绪式（readiness，poll / select）。Core 的 HAL 提供
`HAL/PlatformIoPort.h`（12 §1.3）：

- `IoPort`：一个完成队列。把 socket 或管道关联到它之后，在上面投递异步的接收、发送、接受连接、发起连接、等待管道客户端，
  每个操作带一个调用方拥有的 `IoOperation`（内含平台的重叠结构），完成时从 `Wait` 里取回，带上传输的字节数和结果。
- Windows 用 IOCP（`WSARecv` / `WSASend`、`AcceptEx`、`ConnectEx`、重叠的 `ReadFile` / `WriteFile` / `ConnectNamedPipe`）；
  Linux 用 io_uring；主机用各自平台的等价物，写在平台层。
- 选完成式的理由：socket 和命名管道在 Windows 上只有完成式能统一；数据直接收进调用方的缓冲区，不需要「就绪后再拷一次」；
  一个端口加几个线程就能服务几千个连接。

## 3. NetLoop

**决策**：`NetLoop` 拥有一个 `IoPort` 和若干**专用 IO 线程**（03 §5.2：网络 socket 属于专用线程），线程都在同一个端口上等待完成。

- 所有回调都在 IO 线程上执行，必须很短：解析帧、入队、发起下一次操作。耗时的工作交给 Job 系统（`Jobs::Run`）或者主线程。
- 同一个连接的回调不会并发：一个连接同时最多只有一个接收在途，接收完成的回调返回后才投递下一个。发送完成只做簿记。
- 所有权：引擎的 `NetSubsystem`（EngineSubsystem）拥有进程共用的那个 loop，线程数是 `Engine.Net.Threads`（默认 2）；
  不创建 Subsystem 的 Program 自己构造一个 `NetLoop`。用到网络的类都显式接收 `NetLoop &`，不藏全局状态。
- `NetLoop::Post(Function)` 把一个函数排到 IO 线程上执行，用于从其他线程安全地关闭连接等。

## 4. 流式连接

**决策**：TCP 和本机管道（Windows 命名管道，其他平台 Unix domain socket）都是 `NetStream`，上层看不出区别。

- `NetStream` 是引用计数对象。`Start(handler)` 之后，收到的字节追加到连接自己的接收缓冲区，然后调用 `OnReceive(bytes)`；
  回调返回用掉了多少字节，剩下的留到下一次（半个帧）。连接关闭（对端关闭、出错或本端 `Close`）时调用一次 `OnClose`。
- `Send(bytes)` 可以从任意线程调用：字节拷进发送队列，没有发送在途时立刻投递；一次投递把队列里的多个缓冲区一起发出（gather）。
- **决策**：`Send` 也接受**引用的缓冲区**（引用计数的字节，不拷贝）：发送队列持有引用直到发送完成，数据直接从调用方的内存交给网卡。
  GlobalStore 服务器用它零拷贝地应答（06 §3.3）。`Send(owner, bytes)`；另有 `SendFile(owner, file, offset, size)`（只限 TCP），
  内核用 `TransmitFile` 从文件缓存直接发出一段文件，紧挨在它前面的一个缓冲区作为 head 一起发。
  `GetQueuedBytes` 给上层做背压。`Close` 先把队列发完，TCP 上再 `shutdown(SD_SEND)` 后关闭（否则 `TransmitFile` 刚完成就 `closesocket` 会截断对端还没收到的数据）；`Abort` 立刻关闭。
- `NetListener`：TCP 监听预先投递若干个 `AcceptEx`；管道监听预先创建若干个实例并投递 `ConnectNamedPipe`，接受一个就补一个。
  每接受一个连接调用 `OnAccept(NetStream)`。
- 连接：`NetStream::ConnectTcp(loop, host, port, timeout)` 和 `ConnectPipe(loop, name, timeout)`，在调用线程上等到连上或超时。

## 5. 数据报

**决策**：`NetDatagramSocket` 是 UDP 端点，同时有多个接收在途，每个数据报带来源地址交给回调；`SendTo` 可以从任意线程调用。
它是 05 §7 可靠 UDP（通道、分片、加密）的底层；可靠 UDP 本身是 Net 之上的一层，和复制一起做。

### 5.1 传输扩展点

**决策**：复制、语音等面向会话的流量不直接用 socket，而是经过**传输**（transport）：一个 Provider 扩展点（13 §3）
`MCLASS(Abstract, ExtensionPoint) class NetTransport`，按名字选。引擎自研的可靠 UDP（名字 `Udp`）、其他模块提供的 KCP、
Steam Networking Sockets、EOS P2P、主机平台的中继都是它的实现，地位相同，复制层的代码里没有按传输写的分支。

- **传输是工厂**：`NetTransport` 本身无状态，`GetName` 给出选择用的名字；`Listen(loop, desc, onAccept)` 返回一个 `NetEndpoint`，
  `Connect(loop, host, port, desc, timeout)` 返回一个 `NetSession`。会话和端点才有状态，归调用方所有。
- **会话是消息，不是字节流**：`NetSession::Send(channel, message)` 交出一条完整的消息，对端的 `OnMessage(channel, message)` 收到的也是完整的一条；
  分片、确认、重传、拥塞控制、加密都在传输内部。消息不能超过 `GetMaxMessageSize()`。
- **通道由调用方声明**：`NetSessionDesc::Channels` 是一组 `NetChannelDesc { Delivery }`，通道号就是下标；两端必须声明相同的通道。
  `NetDelivery` 是投递保证：`Unreliable`、`UnreliableSequenced`（只交付比已交付的更新的消息，适合快照）、`Reliable`、`ReliableOrdered`
  （适合 RPC）。这是一组固定的语义，不是可扩展的概念：传输模块扩展的是**怎样实现**这些保证，比如 KCP 传输用 KCP 实现可靠通道，
  不可靠通道直接发数据报。传输不支持某种投递时，`Listen` / `Connect` 失败，而不是悄悄降级。
- **线程**：和 `NetStream` 相同（§3、§4）：回调在 IO 线程上，同一会话的 `OnMessage` 不并发，`OnClose` 只调用一次且最后调用；`Send` 可以从任意线程调用。
- **统计**：`NetSession::GetStats()` 给出往返时间、丢包率、收发字节数和排队字节数，复制层据此做优先级和带宽分配（05 §7）。
- **选择**：`NetTransportSet` 持有 `ExtensionSet<NetTransport>` 并按名字建表；同名且同 Priority 的两个实现在启动时报错。
  `NetSubsystem` 持有引擎的那一份，`Engine.Net.Transport` 指定默认传输（默认 `Udp`），使用方也可以按名字要别的传输；
  不创建 Subsystem 的 Program 自己构造一个 `NetTransportSet`。`Engine.Extensions.Disabled` 和 `Replaces` 照常生效（13 §3.4），
  所以模块可以包装引擎的传输（例如加一层统计或模拟丢包）。
- **会话的公共部分**：`NetSession` 基类保存处理函数并负责「只关闭一次」的簿记，实现只需调用 `Deliver(channel, message)` 和 `Finish()`。

## 6. 上层的用法

| 上层 | 用法 |
| --- | --- |
| Remote（10） | 监听本机管道和 TCP；每个连接的帧解析器在 `Content-Length` 流和 WebSocket 之间按前 4 个字节选择 |
| GlobalStore（06 §3.3） | 服务器：一个 TCP 监听，请求在 IO 线程上解析和应答；内存里的值用引用的缓冲区零拷贝发出，段文件里的值异步读（同一个完成端口）后发出；客户端：请求按 id 和回调匹配，同步调用等待事件 |
| `MarlinFileServer`（02） | 同 GlobalStore 的模式 |
| 复制（05 §7） | `NetTransport` 创建的 `NetSession`：快照走 `UnreliableSequenced` 通道，RPC 走 `ReliableOrdered` 通道 |

## 7. 实现现状

- **HAL**：`HAL/PlatformIoPort.h`，Windows 实现在 Windows 层（`WindowsIoPort.cpp`，因为命名管道在那一层）；`AcceptEx`、`ConnectEx` 和
  `GetAcceptExSockaddrs`、`TransmitFile` 运行时通过 `WSAIoctl` 取得。`PlatformSocket::Create` 创建的 socket 都是 overlapped 的，阻塞调用照常可用。
  `FileFlags::Async` 打开的文件也能关联到端口，按偏移重叠读（`IoPort::Read`）；`IoPort::SendFile` 是 `TransmitFile`。
- **模块**：`Runtime/Net`（依赖 Core 和 Engine，`NetSubsystem` 需要 Engine）：`NetLoop`、`NetStream`、`NetListener`（TCP 预投递 8 个
  `AcceptEx`，管道预建 4 个实例）、`NetDatagramSocket`（8 个接收在途）、`NetSubsystem`（`Engine.Net.Threads`，默认 2）。
- **关闭**：`NetStream` 的 `OnClose` 只调用一次，之后不再有回调，处理函数随即释放，所以处理函数可以持有对象自己的引用。
  `NetListener::Close` 和 `NetDatagramSocket::Close` 等在途操作全部完成才返回，之后不会再有回调；不能在 IO 线程上调用。
- **已迁移**：Remote（10 §6，帧解析是 `IRemoteFraming`：`Content-Length` 流、TCP 接入时的 WebSocket 嗅探和握手、WebSocket 客户端；
  握手 2 秒内没完成的连接在 `ProcessPending` 里关闭）和 GlobalStore（06 §3.3）。Indexer、Converter 等不创建 Subsystem 的程序自己构造 `NetLoop`。
- **传输扩展点**（§5.1）：`NetTransport`、`NetEndpoint`、`NetSession`、`NetTransportSet`，`NetSubsystem` 持有引擎的一份。
  引擎自己的 `Udp` 传输（可靠 UDP）还没有实现；测试里有一个以扩展形式实现的测试传输，验证发现、选择、禁用和会话语义。
- **复制**（05 §7）：`Runtime/Replication` 的 `NetDriver` 通过指定名字或默认的 `NetTransport` 监听和连接；快照走 `UnreliableSequenced`，IO 线程上只把收到的消息排队，`BeginFrame` 再交给复制器。在 `Udp` 传输做好之前，复制只能跑在其他模块提供的传输上。
- 尚未实现：`Udp` 传输、`MarlinFileServer`、Linux 的 io_uring 实现。
