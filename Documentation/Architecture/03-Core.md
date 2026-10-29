# 03 · Core

`Source/Runtime/Core`：所有模块都依赖它。它不依赖 std，只依赖平台层。

## 1. 头文件政策

只允许 `<initializer_list>`、`<new>`、`<cstddef>`、`<cstdint>`、`<compare>`。
类型萃取用 clang 内建函数自己封装（`__is_trivially_copyable`、`__remove_cvref`、`__builtin_is_constant_evaluated`……）。
靠代码审查保证，Program 专属模块不受限制。

编译器内建函数只在各自的封装里直接调用，其他代码一律走封装：原子操作走 `Atomic<T>` / `AtomicRef<T>`，
内存拷贝走 `MemCopy` / `MemSet`，位操作走 `Templates/Bit.h`（`BitCast`、`PopCount`、`CountLeadingZeros`……），
陷阱走 `MARLIN_TRAP` / `MARLIN_DEBUG_BREAK`，SIMD 走 `Math/Simd.h`。只为 constexpr 求值写的分支例外。

## 2. 内存

### 2.1 通用堆

**决策**：自研 **MarlinHeap**，按大小分三档：

| 档 | 大小 | 算法 |
| --- | --- | --- |
| Small | ≤ 1 KiB（32 个 size class） | 每线程堆 + 64 KiB slab 页；直查表 `Direct[tag][(size+15)>>4]` 一次定位页；本地 free list + 原子跨线程 free list（类 mimalloc） |
| Medium | 1 KiB – 1 MiB | 每线程 TLSF（15×16 级），跨线程释放先挂到所属线程的延迟队列 |
| Large | > 1 MiB | 整段 segment span |

- 启动时 reserve 一整块 arena，切成 2 MiB segment，用位图管理；空闲 segment 保留已提交内存，超过 `RetainedBytes` 的部分延迟 `PurgeDelayMilliseconds` 后归还 OS。
- 这些参数按平台层配置：`HAL/PlatformHeapConfig.h`（Generic：16 GiB arena / 32 MiB 保留 / 100 ms；Windows：256 GiB / 64 MiB / 250 ms）。GPU 内存不归它管。
- `operator new/delete`：toolkit 为每个链接 Core 的二进制生成 `MemoryOperators.cpp`（包含 `Memory/MemoryOperators.inl`）并加入单独的 ObjectList，DLL 和单体构建都转发到 MarlinHeap。
- 调试支持：guard page 模式（`-MallocGuard`）、分配点跟踪（`-MallocTrack`，非 Shipping）、释放后填充 `0xDD`。
  **偏差**：填充只在 Debug 和 Development 开启，Profile 不填，以免性能数据失真。
- AddressSanitizer 构建（`MARLIN_ASAN`）里 `Memory::Allocate` 改用 C 运行时堆（`PlatformMemory::SystemAllocate`），由 ASan 监视；
  MemoryTag 统计和 `-MallocTrack` 照常工作，`-MallocGuard` 不起作用。Arena / FrameArena / PoolAllocator 把还没分配出去的内存
  标记为不可访问（poison），fiber 切换会通知 ASan（见 §7）。
- 性能（Shipping，相对 mimalloc 的耗时比）：小对象 0.57–0.88，1–64 KiB 1.16，64–512 KiB 0.79，8 线程 1.01–1.14，跨线程 0.98。

### 2.2 MemoryTag 与预算

```cpp
MARLIN_MEMORY_SCOPE(MemoryTag::Physics);        // 栈式推入 TLS
void* p = Memory::Allocate(size, alignment);     // 从 TLS 取当前 tag，计入统计
```

- 非 Shipping：每个 Tag 统计当前值和峰值；Tag 预算在 `Config/Memory.mtext` 中配置，可以按平台层覆盖。超出预算时告警，并在 DebugUI 里显示。
- Tag 是开放注册的：`MemoryTag` 由名字的 constexpr rapidhash 标识，不用枚举。统计按线程批量累计，每 256 KiB 或有预算的 tag 更早刷新。
- Tag 栈放在 `ExecutionContext` 里，随 fiber 切换；job 继承提交者当前的 tag。
- 可以通过 Remote 层 `Memory.Snapshot` 导出快照（每个调用栈的分配汇总），供外部工具对比。
- Shipping：tag 参数在编译期消失，零开销。

### 2.3 局部分配器

容器用模板参数指定分配器：`List<T, TAllocator = HeapAllocator>`。

- `InlineAllocator<N, TFallback = HeapAllocator>`：前 N 个元素放在对象内部。
- `FixedAllocator<N>`：只用内联存储，超出就断言。
- `ArenaAllocator`：线性分配，一次性释放；**FrameArena** 每帧重置（双缓冲，用于 render 提取）。
- `PoolAllocator<T>`：固定大小块。
- 需要类型擦除的地方，用 `PolymorphicAllocator` 适配一个 `IAllocator&`（基本只在工具代码里用）。
- 函数之间传参用 `Span<T>` / `StringView`，这样形参不受分配器类型影响。

## 3. 容器与基础类型

| 类型 | 说明 |
| --- | --- |
| `List<T, A>` | 动态数组 |
| `Array<T, N>` | 定长 |
| `Span<T>` | 非拥有视图 |
| `HashMap<K, V, A>` / `HashSet<K, A>` | 开放寻址，SwissTable 风格的 SIMD 探测 |
| `SortedMap` / `BitArray` / `RingBuffer` / `SparseSet` | |
| `UniquePtr<T>` / `SharedPtr<T>`（侵入式与非侵入式两种） / `WeakPtr<T>` | |
| `Optional<T>` / `Variant<Ts...>` / `Result<T, E>` | 没有异常，用 `Result` 返回错误 |
| `Function<Sig, InlineSize>` / `FunctionRef<Sig>` | 回调 |
| `Guid` / `Hash128`（XXH3） / `DateTime` / `TimeSpan` | `DateTime` 是自 0001-01-01 起的 100 ns tick（同 .NET） |
| `CommandLine` / `Random` | `-Name=Value` / `-Name:Value` 开关；`Random` 是 xoshiro256**，`ThreadRandom()` 用 OS 随机源播种 |

- 哈希：`RapidHash`（短键默认）、`Xxh3Hash64/128`，都是 constexpr 且与参考实现逐位一致；运行期 XXH3 长输入用 4×64 位向量累加（16 MiB 约 66 GB/s）。
- `HashMap` 用 16 字节控制组，一次向量比较加 movemask 探测；C 字符串按内容哈希，所以字符串键的表可以直接用 `const char*` 查找。
- 分配器策略：`TAllocator::ForType<T>` 提供 `Allocate` / `Free` / `IsInline` / `InlineCapacity`；`IsTriviallyRelocatable<T>` 让容器用 memmove 搬移元素。
- 相对 EASTL（Shipping 耗时比）：HashMap 插入 0.54、查找命中 0.67、未命中 0.15，List push 约 1.2，排序 1.06。

## 4. 字符串

**决策**：全线使用 UTF-8。

- `String`（拥有内存，带 SSO）、`StringView`、`StringBuilder<InlineSize>`。
- `String` 是 24 字节，最多 23 字节内联；`StringBuilder<N>` 继承非模板的 `StringWriter`，格式化只针对 `StringWriter` 实现一次。
- `Name`：驻留字符串，内部是 32 位索引，指向全局分段表，比较是 O(1)；用于类型名、组件名、Config 键。
  区分大小写，永不释放；读取无锁，添加按哈希分 16 个分片加锁。
- 只在 Win32 API 边界转换成 UTF-16（Microsoft 层 `Internal/HAL/MicrosoftHeaders.h` 的 `Microsoft::Utf8ToWide` / `WidePath`，12 §1.3）。
- 格式化：`Format("{0} {1:F2}", a, b)`，C# 风格的占位符 `{index[,alignment][:spec]}`，编译期检查每个占位符都有参数、每个参数都被用到。
  运行期才知道的格式串（本地化文本）用 `RuntimeFormat` 包装。自定义类型实现 `FormatTo(StringWriter&, StringView spec)` 或特化 `Formatter<T>`。
- 数字：浮点格式化默认最短可往返，用 Dragonbox（缓存表在 Core 私有头里）；定点、指数和指定精度用大整数精确输出；解析正确舍入；与区域设置无关，日志、Config、存档在任何机器上都一样。
- **i18n 不在 Core**：本地化文本、区域相关的数字/日期格式、复数规则放在独立的 Localization 模块（ProjectSubsystem 管理，01）；Core 只提供 UTF-8 与 `RuntimeFormat`。

## 5. 并发

### 5.1 原子与同步

`Atomic<T>` / `AtomicRef<T>`（`__atomic_*` 内建）、`SpinLock`、`Mutex`（平台层实现，Windows 用 SRWLOCK，posix 用 futex 或 os_unfair_lock）、
`ReadWriteLock`、`Event`、`Semaphore`、无锁有界 `MpmcQueue<T>`。

### 5.2 Fiber Job 系统

**决策**：类似 Naughty Dog 的 fiber job 系统。

- 每个物理核心一个 worker 线程，绑核；主机上为 OS 保留核心（由平台 Config 决定）。
- Fiber 池：160 个 64 KiB 栈的 fiber，外加 32 个 512 KiB 栈的大 fiber。
- 三级优先级队列：High / Normal / Low，无锁 MPMC。
- `JobCounter`：`Jobs::Run(span, &counter)`；`Jobs::Wait(counter, target)` 会切换 fiber，不会阻塞线程。
- 不能在 fiber 之间迁移的代码（持有 OS Mutex、调用某些平台 API）要用 `[[marlin::pinned]]` 标记，并使用 `JobScope::Pin`。
- TLS 在 fiber 切换后不再可靠，所以引擎内部用的是 `FiberLocal<T>`，而 `ThreadLocal<T>` 必须显式声明。
- 专用线程：只有 IO、音频输出回调、OS 消息泵（窗口线程；macOS 上是进程主线程，引擎主循环在另一个线程，12 §1.5）、网络 socket。
- 生命周期：`JobSystem::Start/Stop` 由拥有它的 Subsystem 调用；未启动时 `Jobs::Run` 在调用线程上立即执行。默认 worker 数是物理核心数减一（留给启动它的线程）。
- 等待：`Wait` 先把切换后要做的事（挂起登记、归还 fiber）交给下一个 fiber 执行，所以 fiber 完全离开 CPU 后才可能被另一个线程恢复。
  可切换的 fiber 依次取：就绪 fiber、空闲 fiber；都没有时在栈空间允许的范围内内联执行队列中的 job，再不行就创建溢出 fiber，
  `JobSystem::GetOverflowFiberCount()` 非零说明池子对这类负载偏小（深度递归扇出时常见）。
- `FiberLocal<T>` 存在 `ExecutionContext` 里，语义是“每个 job 一份”，job 开始时清零；`JobScope::Pin()` 让 job 在等待后回到同一个 worker。
- 性能（Shipping，5 worker）：空 job 约 70 ns，Run + Wait 往返约 190 ns，ParallelFor 加速比约 6×（6 个线程）。

## 6. 数学

**决策**：右手系、Y 轴向上、单位是米。

- `Vector2/3/4`、`Quaternion`、`Matrix3x4`（变换）、`Matrix4x4`（投影）、`Transform`（平移 + 旋转 + 缩放）。
- 存储类型是标量；SIMD 计算类型是 `Vector4Simd`（SSE4.2 / AVX2 / NEON 通过 clang 向量扩展 `__attribute__((ext_vector_type(4)))` 实现，同一份源码）。
  能用 clang 内建函数的就用内建函数；只有近似倒数 / 平方根倒数和字节 movemask 用平台指令，放在架构层的 `HAL/PlatformSimd.h`（Generic 层有可移植实现）。
- 矩阵按行存储，向量是列向量（`v' = M * v`），上传到 HLSL 时用 `row_major`。`Matrix3x4` 是仿射变换，`Matrix4x4` 用于投影。
- 投影使用反向深度（近平面 1、远平面 0），视图空间看向 -Z；`Frustum::FromViewProjection` 按此约定提取平面。
- 其他：`Float16`、`LinearColor` / `Color32`（sRGB 转换）、`Box` / `Sphere` / `Plane` / `Frustum` / `Ray`、`IntVector2/3`。
  `Transform::Inverse` 只在均匀缩放时精确。
- 大世界用 `WorldPosition`（double3）。渲染提取时转换成相对相机的 float（05）。
- 平台最低 ISA：X64 是 x86-64-v3（AVX2），Arm64 是 armv8.2-a；`Platform::Initialize` 启动时检查，不满足就提示并退出。

## 7. 诊断

- **日志**：`MARLIN_LOG(LogPhysics, Warning, "…{0}", x)`，带分类、级别、帧号、线程。Core 只提供前端：
  分类用 `MARLIN_DECLARE_LOG_CATEGORY(API, LogPhysics, 默认级别, Shipping 级别)` 在头文件声明、`MARLIN_DEFINE_LOG_CATEGORY` 在一个源文件定义，
  所有模块共享同一个阈值；低于 Shipping 级别的调用在 Shipping 中编译期消失，Fatal 永远保留。
  消息写入无锁 MPSC 二进制环形缓冲，`Log::Flush` 把记录分发给 `ILogSink`（Error 及以上立即刷新）；Core 只内置调试器输出 sink。
  写文件、Remote 推流、崩溃报告附带日志都是其他模块注册的 sink，刷新时机由拥有日志的 Subsystem 决定。
  主循环开始前没有每帧刷新：有 Splash 时由 Splash 在窗口线程上定期刷新（09 §4.6），没有时第一帧结束才一起输出。
  - **运行时过滤**：`MARLIN_DEFINE_LOG_CATEGORY` 同时在模块的 `.mlogc$m` section 里放一个指向分类的指针（Windows 的 COFF 按 `$` 后缀排序，
    生成的模块文件里 `MARLIN_DEFINE_LOG_CATEGORY_MARKERS` 放 `$a`、`$z` 两个空指针作首尾），`Register_<Module>` 调 `MARLIN_REGISTER_LOG_CATEGORIES()`
    把这一段交给 `Log::RegisterCategories`，启动时不执行任何静态构造。单体可执行文件里所有模块共用一个 section，所以注册表按指针去重。
    `Log::FindCategory`、`Log::ForEachCategory`、`Log::SetLevels("LogCore:Debug,LogAssetIndex:Trace")` 查找和设置阈值；
    控制台命令 `Log.Level <分类> <级别>` 和 `Log.List`（`LogSubsystem`），启动参数 `-LogLevel=<分类>:<级别>,…` 在 `LogSubsystem::Initialize` 里最先应用。
    Shipping 编译掉的消息调高阈值也不会出现。其他平台还没有 section 的写法（`MARLIN_LOG_SECTION` 在那里是 `#error`）。
  - **级别**：`Info` 只记生命周期和用户可见的状态变化：启动/关闭阶段、窗口与设备创建、索引与编译完成、配置重载；
    `Debug` 记逐个对象、逐帧的诊断；`Trace` 记高频细节。`Warning` 记可恢复的降级，必须写明原因和替代行为；
    `Error` 记操作失败，必须写明对象、路径或 GUID、错误码和调用方接下来做什么；`Fatal` 只在进程无法继续时用。
    `Info` 不得出现在逐帧或逐资产的路径上。
  - **上下文**：消息以对象名、路径或 GUID 开头，写清操作、关键数值和单位（`ms`、`MiB`、`1280x720`），
    计数写成 `当前/总数`。不要重复分类名：`LogRenderer` 的消息里不再写 `[Renderer]`。
    例：`Texture {uuid} did not load; it samples white`、`Compiled 28 of 28 global shaders in 131 ms, 0 failed`。
  - **频率**：同一对象的同一故障只记一次，持续状态用统计或 `Debug`，不要每帧刷 `Warning`；逐资产的流式驻留变化、
    字体加载明细、材质置换选择只在 `Debug` 输出。成功的批量操作记一条带总数和耗时的摘要，不逐项记 `Info`。
  - **来源**：`[时间][帧][线程] 分类: 级别: 消息` 由 `Log::FormatRecord` 统一生成；子进程转发的记录由
    `RemoteLogStream::Relog` 加 `[进程 pid]` 前缀，消息本身不再带进程名。
- **断言**：`MARLIN_ASSERT`（非 Shipping）、`MARLIN_VERIFY`（表达式总是会执行）、`MARLIN_CHECK_SLOW`（只在 Debug 启用）、`MARLIN_FATAL`；
  带格式化消息的版本在 `Diagnostics/AssertFormat.h`：`MARLIN_ASSERTF` / `MARLIN_VERIFYF` / `MARLIN_FATALF`。失败会以 Fatal 写入日志并刷新后再崩溃。
- **崩溃**：进程启动时与 `MarlinCrashReporter` 握手，交换共享内存和事件句柄；崩溃时由它从进程外写 minidump，附带最近的日志和 Config，再上报到自建服务。详见 §7.1。
- **Profiler**：`MARLIN_PROFILE_SCOPE("Name")` 记录 CPU 区间，GPU 时间戳由 RenderGraph 自动打点；只在 Profile 及以下的配置中开启。
  Core 只调用可安装的 `ProfilerHooks`（区间、帧标记、曲线、消息、fiber 切换），Tracy 等后端作为 ThirdParty 在其他模块里实现这些回调。
- **Sanitizer**：用 LLVM clang 自带的 AddressSanitizer 和 UndefinedBehaviorSanitizer（Windows 上只有这两个）。
  `marlin generate --preset Sanitizers` 在 Debug 和 Development 打开两者；toolkit 负责链接运行库并把 ASan 的 DLL 拷到二进制旁边。
  Core 的标注接口在 `Diagnostics/Sanitizer.h`（poison / unpoison、fiber 切换），不开 ASan 时为空操作。
- **CVar 与控制台命令**：反射化，`MPROPERTY(ConsoleVariable="r.Shadow.Quality", Flags=Cheat|Scalability)`；
  都暴露给 Remote 层，可以从 Config 中设置，也可以热重载。
  - CVar 必须是**静态字段**，控制台命令是**静态** `MFUNCTION(ConsoleCommand="r.Recompile")`；模块注册时自动登记，名字不区分大小写，帮助文字取自 `///` 注释（Tooltip）。
  - Core 只有注册表和执行器（`Console/Console.h`：`Console::Execute("r.Shadow.Quality 2")`），没有 UI；主机上由 Remote 层或 DebugUI 的手柄菜单输入。
  - 值的来源有优先级：Default < Config < CommandLine < Code < Console，低优先级不能覆盖高优先级；`Reset` 回到注册时的值。
  - `Flags=ReadOnly` 拒绝控制台修改；Shipping 中 `Flags=Cheat` 的 CVar 和控制台命令拒绝控制台调用，CVar 仍可由 Config 设置，
    但只能是引擎和项目的层，玩家可写的 `/User/Config` 与 `-Config:` 设置不了（06 §1.4）。
  - 只在游戏线程的帧间修改；其他线程读到新值的时机不保证。

### 7.1 崩溃报告

**决策**：dump 尽量在进程外写；进程内只做最少的事，进程外的 `MarlinCrashReporter` 负责写 dump、收集附件和上传，
服务端 `MarlinCrashServer` 负责符号化和归类。模块分三层，靠链接关系组合：

| 模块 | 内容 | 谁链接 |
| --- | --- | --- |
| `Runtime/CrashReport` | 进程内：崩溃回调、与 Reporter 共享的状态（`CrashProtocol.h`）、`Report.mtext`（`CrashReportInfo`）、隐私过滤（`CrashPrivacy`）、`Debug.Crash` 命令 | `Engine`，即所有 Target |
| `Runtime/CrashAnalysis` | 读 minidump、用镜像的 unwind 数据走栈（x64）、符号化、计算签名 | `MarlinCrashReporter`、`CrashServer` |
| `Developer/CrashServer` | 接收上传、分析、按签名分桶 | `MarlinCrashServer` |

- **启动**：`Engine::Startup` 在定位引擎后立刻 `CrashReport::Install`，此后的崩溃都能接住（还没有 Reporter 时在进程内写 dump，
  落到 `<EngineRoot>/Saved/Crashes`，没有引擎根目录时落到 `%LOCALAPPDATA%/Marlin/Crashes`）；挂载后 `CrashReport::Start` 创建共享内存
  `Marlin.Crash.<pid>` 和两个命名事件（`.Crashed`、`.Written`），带 `ProgramFeatures::CrashReporter` 时拉起 `MarlinCrashReporter -Pid=<pid>`
  并等它挂上（`-NoCrashReporter` 关闭）；`/User` 挂载后报告目录换成 `/User/Crashes`；Config 加载后把 `Engine.CrashReport.*`
  （`Enabled`、`Server.Host/Port`、`AskUser`、`HangSeconds`、`Privacy.Scrub/KeptArguments`、`MaxLocalReports`、`RetentionDays`）交给 Reporter，
  并把这些设置写成 `/Temp/CrashConfig.mtext` 作为附件。主循环每帧 `CrashReport::Heartbeat`，顺带把被驱动或覆盖层替换掉的异常回调装回来。
- **崩溃时**：崩溃线程把异常码、地址、`EXCEPTION_POINTERS` 的地址和断言 / Fatal 记下的失败信息写进共享内存，触发 `.Crashed`，
  等 Reporter 写完（`.Written`）再退出；Reporter 不在或超时则退回进程内写 dump。日志由一个 sink 镜像到共享内存的环形缓冲，
  Reporter 写成 `Log.txt`；`CrashReport::AddAttachment` 登记的文件（如 Aftermath 的 GPU dump）复制进报告目录。
- **崩溃前回调**（`CrashReport::AddCrashHook`，最多 8 个）：崩溃线程在交给 Reporter 之前依次调用，用来把只有进程内才看得到的状态写进日志。
  这是「进程内只做最少的事」的例外，目前只有 `RhiSubsystem` 用：GPU 驱动在自己的线程里崩溃时写 breadcrumb、等 Aftermath dump（07 §1.7）。
  回调里再崩溃时，报告的是第一次崩溃，其余回调不再调用。
- **Reporter 判断的四种情况**（`CrashKind`）：`Exception`、`Failure`（断言、Fatal、CRT 失败）、`Hang`（`HangSeconds` 内没有心跳且没挂调试器，
  写进程快照）、`AbnormalExit`（进程以 NTSTATUS 错误码退出、崩溃回调没跑到，没有 dump）。
- **报告目录**：`Report.mtext`、`Minidump.dmp`、`Log.txt`、`Stack.txt`（本机符号化的栈）和附件；问过用户（`AskUser`）后上传。
  用户同意（或没开 `AskUser`）先写 `Consented`，成功后写 `Uploaded`，拒绝写 `Declined`；下次启动的 Reporter 只补传有 `Consented`
  而没有 `Uploaded` 的报告，所以弹窗时 Reporter 被杀掉或关了机、问题没得到回答的报告永远不会发出。本地按数量和天数清理。
- **默认询问**：`AskUser` 在打包形态下默认为 true（玩家的报告离开机器前先问），源码和安装版引擎下默认为 false；产品可以在 Config 里改。
- **传输**：上传是明文 TCP，没有 TLS。`MarlinCrashServer` 必须放在可信网络后面（内网、VPN 或终止 TLS 的反向代理）。TLS 客户端尚未实现。
- **隐私**：`Privacy.Scrub` 打开时，文本文件里的用户名、计算机名和主目录替换成占位符，命令行只保留 `KeptArguments` 列出的开关的值。
  minidump 本身无法清洗。
- **服务端**：`MarlinCrashServer`（`Engine.CrashServer.Port/DataDirectory/SymbolStore`）把每个报告放进 `Reports/<编号>`，
  用 `marlin symbols` 写出的符号库（`<文件>/<时间戳><大小>/<文件>`、`<pdb>/<GUID><Age>/<pdb>`）分析，写 `Analysis.txt`；
  签名是焦点线程去掉崩溃处理帧之后的前 5 帧的哈希，相同签名进同一个 `Buckets/<签名>.mtext`。
- 发行版也保留 PDB（不随包发布），可执行文件只记录 PDB 的文件名，dump 里不会带出构建机的路径。
- 主机平台见 12 §1.4。
