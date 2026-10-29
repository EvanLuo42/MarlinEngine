# 07 · 渲染

## 1. RHI

**决策**：只支持 D3D12、Vulkan、Metal 加主机原生 API，全部 bindless。

- 资源绑定方式：全 bindless。缓冲区用 GPU 地址访问（shader 里是 `Ptr` / `Span`，MarlinLanguage §4.5；D3D12 上由编译器降低为原始视图索引加偏移）；
  纹理、采样器、加速结构放在全局描述符堆里，shader 里是句柄值（堆索引，MarlinLanguage §4.4）。一个 Pipeline Layout 打天下：直接参数区（Vulkan push data、D3D12 root constants、
  Metal 参数缓冲区，128 字节，装入口点的 `[Uniform]` 参数块）加资源堆和采样器堆。
- RHI 很薄：负责资源（Buffer/Texture/AccelerationStructure）、PSO、CommandList、Queue（Graphics/Compute/Copy）、Fence、Swapchain，
  以及 Barrier 原语（enhanced barriers 风格）。资源状态跟踪全部由 RenderGraph 负责。
- 最低硬件要求：支持 Mesh Shader、Ray Query、RT 管线、SM 6.6 的 GPU（大约 RTX 20 / RDNA2 / Apple M2 起）。不支持旧硬件，这样可以避免维护两条管线。
- 当前进度：只实现 `RHIVulkan`，第一版在 Windows 上验收（NVIDIA 和 AMD 都要测）；Linux 等 `Application` 有 Linux 层以后再做。
  `RHI` 和 `RHIVulkan` 已实现：Game 能在 RTX 50 上建出设备，Api 和 Sync validation 下没有报错。
  RHI 的 GPU 测试（`RHI/Tests`，`MarlinTests --tags=Gpu RHI*`，`-Rhi=` 选后端）覆盖拷贝、清除、跨队列 fence、timestamp、sparse、AS、
  上传、native 互操作和 GPU profiler（每个队列，包括 Copy）；交换链和 compute PSO 由 Renderer 和 `UIPainterRenderer` 在 Game 里跑到（§5）。
  `RHIValidation`（`-RHIValidation`）和 `GpuCrashAftermath` 已实现；Aftermath 的档位是 `Engine.GpuCrash.Aftermath.Level` / `-Aftermath=`
  （Off / Minimal / Standard / Full，默认 Standard，注入了 capture 工具时降到 Minimal），dump 写到 `/User/Crashes` 并解码进崩溃报告，
  shader 调试信息只留在内存里（§1.7）。GPU watchdog 见 §1.5。
  多命令的 indirect 签名和 `IndirectPipelineSet` 走 `VK_EXT_device_generated_commands`。
  `GpuCaptureNsight`（`-GpuCapture=Nsight`，Windows，Shipping 之外）已实现，`-GpuCaptureAt=<帧>` 或控制台命令 `r.GpuCapture <帧数>`
  录整帧，文件写到 `/User/Captures`。还没做：RenderDoc 的 capture 模块。
- 其余已实现的部分：
  - `RhiUploadQueue`：Copy 队列上的上传服务，带票据和队列所有权转移。
  - `RhiPipelineLibrary`：按 key 管理 PSO，记录每次运行用到和错过的 PSO，清单可保存、合并、预热。key 和 payload 的内容由声明 pass 和 shader 的那一层定（RenderGraph、材质系统）。
  - `IRhiPresentHook`：低延迟 SDK 和帧生成的接入点；交换链和 present 统一经过 `RhiSubsystem`。HDR 元数据用 `RhiDevice::SetHdrMetadata`。
  - `RhiGpuProfiler` 和 `ProfilerHooks::GpuZone`：按队列计时，换算到 CPU 时钟交给 Tracy 等后端；显存统计每帧作为 plot 输出，Remote 有 `Render.GpuInfo` / `Render.GpuMemory` / `Render.Capture`。
- **SDK 接入（DLSS、FSR、XeSS、Streamline 等）**：超分是 RenderGraph 里的一个 pass，接口归 Renderer；RHI 提供它们需要的 native 互操作：
  `GetNativeTexture` / `GetNativeBuffer`，以及 `BeginNativeCommands` / `EndNativeCommands`（结束后重新绑定描述符堆，PSO 和直接参数区要重新设置）。
  帧生成这类拦截 API 的 SDK，在 `IRhiPresentHook::BeforeBackendLoad` 里替换 Vulkan 加载库、请求 instance 扩展，在 `Initialize` 里请求设备扩展和额外队列（`VulkanBackend`）。

### 1.1 模块与后端选择

**决策**：RHI 是虚接口，用哪个后端由链接决定，不用宏。

| 模块 | 内容 | 进 Shipping |
| --- | --- | --- |
| `Runtime/RHI` | 接口、句柄池、显存分配器算法、上传分配器、breadcrumb、各类钩子（§1.7） | 是 |
| `Runtime/RHIVulkan` | Vulkan 后端，放在 `Source/` 下，Windows 和 Linux 共用 | 是 |
| `Platforms/Microsoft/.../RHID3D12`、`Platforms/Apple/.../RHIMetal`、`Platforms/<P>/.../RHI<P>` | 其他后端（Metal 3，argument buffer tier 2） | 是 |
| `Developer/RHIValidation` | Marlin 自己的 validation（§1.6） | 否 |
| `Developer/GpuCaptureRenderDoc`、`GpuCapturePix`、`GpuCaptureNsight` | capture 工具（§1.7） | 否 |
| `Runtime/GpuCrashAftermath`、`GpuCrashDred` | GPU 崩溃 dump（§1.7） | 是 |

- Windows 同时链接 `RHIVulkan` 和 `RHID3D12`，启动时按 Config 或命令行（`-Vulkan`、`-D3D12`）选一个；运行中不切换后端。
- 主机只链接一个后端，后端类标 `final`，由 LTO 去掉虚调用。
- 后端以 `EngineSubsystem` 的形式创建设备（01 §3），Server 不链接任何后端。

### 1.2 线程与录制

- **决策**：CommandList 直接录制 native 命令（`vkCmd*`），没有引擎的中间命令流。GPU-driven 管线的 draw 数很少，翻译一遍得不偿失。
- 创建和销毁资源可以在任意线程进行；CommandList 可以在任意 job 里录制，但同一个 list 只属于一个线程；RenderGraph 的 pass 按 job 并行录制（§2）。
- **一次只有一个提交者**：`Submit`、`Present`、`AcquireNextImage`、`ProcessCompletions`（以及 `WaitForPresent`、`UpdateTileMappings`）
  属于同一个提交者，比如这一帧的渲染 job；可以在任意线程上调用，但不能和别的线程的这类调用重叠。没有专门的 RHI 线程。
  （原来规定只在固定的渲染提交线程上调用；渲染 job 跑在哪个 worker 上不固定，所以改成按提交者约束。）
  - 每帧开头的队列工作（结束和开始整帧 capture、释放队列已经用完的对象）在 `RhiSubsystem::BeginSubmitFrame(frame)` 里，由提交者调用。
    Renderer 的渲染 job 用 `RhiSubsystem::ClaimFrameSubmission()` 认领它，然后在提交的每一帧开头调用（§5）；没人认领时由 `RhiSubsystem::BeginFrame` 在主线程上做。
  - `RHIValidation` 把这类调用重叠当作错误报告（§1.6）。
- 渲染 pass：动态渲染（`BeginRendering` 时传入 attachment 和显式的 load/store），没有 RenderPass 对象和 subpass。
  纹理可以标记为 `Transient`（memoryless），在 TBDR（Apple、Switch2）上不分配内存。

### 1.3 资源、描述符与内存

- **句柄**：所有 RHI 对象都用带代数的句柄（index + generation），对象池由后端管理，没有引用计数。
  调用方显式 `Destroy`；RHI 记录销毁时各队列的 fence 值，等 GPU 用完再真正释放。过期句柄和重复释放由 validation 拦截。
- **View**：资源本身不带 bindless 索引。显式创建 TextureView / BufferView（format、mip/slice 范围、SRV 或 UAV）时，才从全局资源堆分配一个槽位；
  槽位由 RHI 的 free list 管理，释放后同样等 fence 再复用。
- **Sampler**：同样走 bindless 的采样器堆。创建时按描述去重，返回索引；常用的 sampler 在启动时预先注册，索引固定，shader 可以把它当常量用。
- **描述符步长**：资源堆里所有种类的描述符都用同一个步长（`DeviceLimits::ResourceDescriptorStride`，在 Vulkan 上取 image 和 buffer 描述符大小的较大值，并按对齐取整），
  所以 view 的索引和描述符种类无关。采样器堆有自己的步长（`SamplerDescriptorStride`）。
  两个步长都由设备决定：RHI 建 pipeline 时把它们作为 specialization constant 传给每个 stage
  （`RhiResourceStrideSpecializationId = 0x7FFF0000`、`RhiSamplerStrideSpecializationId = 0x7FFF0001`，shader 自己的 specialization constant 用更小的 id）。
  SPIR-V 后端按这两个常量计算堆里的偏移，而不是按每种类型的 `OpConstantSizeOfEXT`（MarlinLanguage §11.3）。
- **Root constants**：全局 layout 里固定 128 字节，等于 Vulkan 规范保证的 push constant 下限，在所有平台上一致。放不下的数据写进上传 buffer，再把地址传进去。
- **显存分配器**：**决策** 自研，所有后端共用同一套算法（TLSF 风格的子分配器 + 线性分配器），不用 VMA / D3D12MA。
  块大小、专用分配的阈值、heap 类型划分等参数和策略按平台配置（同 03 §2.1 的 `PlatformHeapConfig` 做法）。
  对外暴露 Heap 和 placed resource，供 RenderGraph 做 transient 资源的 aliasing；统计数据接入引擎的内存统计和 Remote（10）。
- **上传**：RHI 提供每帧的环形上传分配器（持久映射）和 Copy 队列上的上传服务。有 ReBAR 或 UMA（主机、Apple）时直接写显存，省掉一次拷贝。

### 1.4 队列、同步、PSO 与 Present

- **队列**：Graphics、Compute、Copy 各一个。唯一的同步原语是 64 位 timeline fence（VK timeline semaphore / D3D12 fence）；
  跨队列等待在 Submit 时显式声明。没有 binary semaphore，交换链需要的由后端内部处理。
- **队列所有权转移**（`RhiQueueTransfer`）：源队列录 release，目标队列在等源队列的提交之后录 acquire。
  两个队列属于同一个 queue family 时（例如设备没有独立的 transfer family）不换所有者：release 做完整个 barrier，acquire 什么都不录，顺序由提交的 signal/wait 保证。
- **PSO**：在 job 线程上异步创建。磁盘上有驱动级缓存（`VkPipelineCache` 等），并打包一份在 playtest 中收集的 PSO 清单，在加载界面预热。
  PSO 还没就绪时跳过该次 draw 并记录下来，供补充清单使用。
  - 要放进 `RhiIndirectPipelineSet`（间接命令切换 PSO）的 pipeline 创建时必须标 `IndirectBindable`；Vulkan 的 GPU-assisted validation 不能插桩这种 pipeline。
  - 全局 shader（§4.1）的 pipeline 也走这条路：`ShaderLibrary` 经 `RhiPipelineLibrary` 在 job 上创建，shader 没加载或 pipeline 没就绪时 `Dispatch` 什么也不录，返回 false。
  - 每个 pipeline 创建时，后端把它的 shader 交给 crash 工具（`IGpuCrashTool::OnPipelineCreated`，§1.7）。
- **Swapchain**：由 RHI 管理，支持多窗口（编辑器）、HDR 色彩空间、VRR，以及可等待的帧延迟对象。
  低延迟 SDK（Reflex、Anti-Lag 2、XeLL）和帧生成（DLSS FG、FSR FG）做成独立模块，接到 RHI 的 present 钩子上，不放进后端。
  - `WaitForPresent` 让排队的帧不超过 `MaxFrameLatency`。Vulkan 有 `VK_KHR_present_wait` 时用它，并且最多等 100 ms：
    合成器不显示的 present（例如窗口正在缩放）不会卡住帧。

### 1.5 特性范围与能力查询

- 第一版接口包含：
  - 光追：BLAS/TLAS 的构建、refit、compaction 和序列化（Converter 预构建 BLAS 要用），Ray Query，以及 RT 管线（raygen/miss/hit 组和 SBT）。
  - Sparse/Tiled 资源：页级 commit 和 decommit，在队列上执行映射更新（虚拟纹理、虚拟阴影图）。
  - Device Generated Commands：在 MultiDrawIndirectCount 基线之上，支持切换 PSO 和直接参数的间接命令（`VK_EXT_device_generated_commands`）。
  - Variable Rate Shading：由属性图驱动。
- 第一版不包含：Work Graphs。
- **能力查询**：可选特性用开放注册的特性名，和语言里的可选特性（MarlinLanguage §9.7）是同一套名字。后端声明自己支持哪些，RHI 据此选择 ShaderProfile。
  数值上限（最大纹理尺寸、对齐、wave 大小等）放在一个固定的 `DeviceLimits` 结构里。
- **Vulkan 基线**：Vulkan 1.4，要求 `shaderBufferInt64Atomics`（VisBuffer，§5.3），并强制要求 `VK_EXT_descriptor_heap`、`VK_EXT_mesh_shader`、`VK_KHR_ray_query`、`VK_KHR_ray_tracing_pipeline` 等扩展（完整列表见 MarlinLanguage §11.3）。
  `VK_KHR_shader_untyped_pointers`（`shaderUntypedPointers`）也是必需的：SPIR-V 后端访问描述符堆时总是用 untyped pointer。
- **错误处理**（没有异常）：
  - 编程错误（参数非法、句柄过期）交给 assert 和 validation。
  - 可以恢复的失败（显存不足、PSO 编译失败）返回带错误码的 Result。
  - device lost 走设备级回调：写崩溃报告（breadcrumb、Aftermath/DRED dump）然后退出，不做设备重建。
  - **GPU watchdog**：`RhiSubsystem` 起一个 `GPU Watchdog` 线程，每秒看一次各队列的提交值和完成值。
    某个队列有没完成的提交，却在 `Engine.Rhi.GpuHangTimeout` 秒内（默认 10；0 关掉 watchdog）一个都没完成，就算 GPU 挂起，按 device lost 处理：
    `HandleDeviceLost(hung = true)` 在报告里写明是挂起，然后退出。挂着调试器时不计时。两条路径同时报告时只有第一个写报告。
- **GPU 计时**：RHI 提供 timestamp 和 pipeline statistics 的 QueryPool，以及 CPU/GPU 校准时间戳（`VK_KHR_calibrated_timestamps`），
  对接 Tracy 和 Remote；RenderGraph 自动打点（03 §7）。厂商硬件计数器（Nsight Perf SDK、AMD GPA）以后做成独立模块。
  - 有的队列的 command list 不能 `ResolveQueries`（Vulkan 的 transfer-only 队列，看 `DeviceLimits::ResolvesQueries`），
    就等那帧的工作完成后用 `RhiDevice::ReadQueries` 在 CPU 上读结果（读完会重置这些 query）。
    所以 `RhiGpuProfiler` 也给 Copy 队列计时；不能写 timestamp 的队列（`TimestampFrequency` 为 0）不计时。

### 1.6 Validation

两套 validation 各自独立开关：

- **GAPI validation**（VK validation layers / D3D12 debug layer + GBV）：级别分为 `Off` / `Api` / `GpuAssisted` / `Sync`，可以组合，由 Config 或命令行控制。
  Debug 配置默认开 `Api`，其他配置默认全关。消息转发到引擎 Log，带资源调试名和当前 marker 栈，严重错误会中断到调试器。
- **RHI validation**：**决策** `RHIValidation` 是装饰器模块，包装所有 RHI 接口，用 `-RHIValidation` 开启。
  检查过期句柄、重复释放、跨线程使用 CommandList、barrier 和 view 的参数、fence 的使用顺序等 Marlin 自己的契约。后端代码里没有任何校验分支，Shipping 里也没有这个模块。
- RenderGraph 层面的读写冲突检查归 RenderGraph（§2），不在 RHI 里做。
- 当前进度：`RHIValidation` 还检查提交者契约（§1.2）：`Submit`、`Present`、`AcquireNextImage`、`WaitForPresent`、`ProcessCompletions`、
  `UpdateTileMappings` 在一个线程上执行时，另一个线程又调用其中之一，就报「submissions must not overlap」；设备内建的 plain 命令签名（`GetCommandSignature`）登记为已知签名，`ExecuteIndirect` 照常接受，销毁它们则报错；`ReadQueries` 检查 pool、query 范围和输出大小。

### 1.7 Capture 工具、GPU 崩溃与调试名

**决策**：capture 和 crash 工具不放进后端，而是做成实现开放注册接口（`IGpuCaptureTool` 等）的独立模块。RHI 只提供钩子：

- 设备创建前的回调（工具在这里注入）；
- native 设备、队列和 command buffer 的 handle；
- 帧边界，以及在两次 Submit 之间开始或结束 capture 的点；
- marker 和资源调试名。

Capture：

- RenderDoc、PIX、Nsight 都必须在创建设备之前注入，而且互相排斥。所以工具在**启动时**选定（编辑器偏好或命令行，默认不注入），切换要重启；
  编辑器提供「以 RenderDoc 重启」之类的按钮。
- 编辑器里的录制按钮通过 Subsystem 触发，支持这些范围：
  - 整帧，或连续 N 帧（包含所有窗口和视口）；
  - 单个视口 / SceneView；
  - RenderGraph 可视化面板里选中的一段 pass；
  - 远程进程或 devkit：通过 Remote（10）让 Game 进程录制，再把文件拉回来。
- 后三种范围要求 RenderGraph 在 capture 的起止点切开提交，所以开始和结束 capture 的钩子必须能在一帧中间、两次 Submit 之间调用。

GPU 崩溃：

- **breadcrumb**（RHI 自研）：每个 marker 开始和结束时把计数写进一个 buffer（`VK_AMD_buffer_marker` / NV checkpoints / D3D12 `WriteBufferImmediate`），
  device lost 时写进崩溃报告，能看出挂在哪个 pass。
- **Aftermath / DRED**：作为 `GpuCrash*` 模块，用来收集 GPU dump。
- **crash 工具和 capture 工具可以同时运行，但 crash 工具要降档**：Nsight Graphics 支持 `VK_NV_device_diagnostic_checkpoints` 和 `VK_NV_device_diagnostics_config`，
  会录下 Aftermath 的调用，replay 时也可以关掉。但 call stack capturing、shader error reporting 这类重档位会改变 GPU 时序，也会影响 replay。
  所以 crash 工具初始化时会拿到已注入的 capture 工具，并据此降到不干扰它的档位（只保留 crash dump 和 marker）。
  D3D12 上 PIX 这类 API 拦截工具和 Aftermath 的 marker、resource tracking 冲突，到时由 Aftermath 模块在 D3D12 上处理。
- **crash 工具能看到每个 pipeline 的 shader**：后端创建 pipeline 时（在创建它的线程上）经 `RhiSubsystem::HandlePipelineCreated` 调用各 crash 工具的
  `IGpuCrashTool::OnPipelineCreated(shaders, debugName)`，dump 里就能用 pipeline 名字标出出错的 shader，pipeline 销毁之后也能解码。
- 当前进度（`GpuCrashAftermath`）：
  - 按 Aftermath 的 SPIR-V hash 记下每个 pipeline 的 shader：Minimal 只记名字，Standard 和 Full 还留 SPIR-V 给解码器查找。
  - shader 调试信息（Standard 起）只留在内存里供解码，不写盘也不上传：我们的 SPIR-V 调试信息内嵌了完整的 shader 源码。
  - device lost 时等 Aftermath 写完 dump（等它开始最多 3 s，总共最多 10 s），dump 写到 `/User/Crashes/GpuCrash-<时间>.nv-gpudmp`，
    然后解码进崩溃报告：设备状态、page fault 和对应的资源、正在运行的 shader（用 pipeline 名字标出）和出错的 warp（最多 64 个）。
    解码器输出的完整 JSON 作为附件放在 dump 旁边。
  - 已用真实崩溃验证过（场景流送的 GPU 挂起和 page fault）：设备状态、page fault 地址、breadcrumb 和出错 warp 的 `.mshader` 源码行
    （如 `SceneLod.mshader:72`）都能解出来。源码行要求 shader 带调试信息：`MarlinConverter` 给开发构建编译时打开（08 §7）。
  - **驱动线程先崩**：驱动可能在自己的线程里访问冲突（如 `nvoglv64.dll`），进程直接走 CPU 崩溃，没有 device lost 回调。
    `RhiSubsystem` 因此注册一个崩溃前回调（`CrashReport::AddCrashHook`，03 §7.1）：出错地址落在 `Engine.Rhi.DriverModules`
    列出的驱动模块（Windows 层配置 NVIDIA、AMD、Intel 的 Vulkan 和 D3D12 驱动）里时，照 device lost 的做法把 breadcrumb 和 crash 工具的
    dump 写进日志，标题是 `The GPU driver crashed in <模块>.`；另一个线程正在写 device lost 报告时等它写完（最多 15 s）。
  - 还缺：page fault 地址为 0 或指错时说不出是哪个 shader 参数。资源追踪只认 RHI 对象，`Span` 的基址不是。
- **shader 里的检查**不靠 Aftermath：开发构建的 shader 把 `Debug.Print`、`Debug.Assert` 和 `CheckSpans` 的越界、空 `Span` 报告写进
  调试缓冲区，`ShaderLibrary` 在帧完成后读出并写进日志（MarlinLanguage.md §9.11）。它报告的是 GPU 继续跑下去的错误；
  让 GPU 当场出错的访问要等帧完成才读得到，那时设备已经丢失，只能靠上面的崩溃报告。
- 帧内命令用到的内存（`ShaderLibrary` 的间接参数块、调试缓冲区）在提交帧开始时退役（`RhiSubsystem::AddSubmitFrameCallback`），
  而不是主线程的 `BeginFrame`：那时上一帧的 render job 可能还在录制，提前退役的参数块会被重用，shader 读到别的参数（表现为指针错乱的 page fault）。

第三方 SDK：

- Aftermath SDK vendor 在 `Source/ThirdParty/NsightAftermath`（目前只有 Windows x64；Linux 版本是 `.tgz`，以后再加）。
- Nsight Graphics SDK（NGFX 0.9.2，Apache-2.0）只 vendor 头文件，放在 `Source/ThirdParty/NsightGraphicsSdk`；
  DLL 不入库，运行时从本机的 Nsight Graphics 安装加载。`Engine.GpuCapture.Nsight.Installation` 选安装（路径包含该字符串的第一个），
  为空时用找到的第一个；编辑器以后扫描安装列表让用户选。
- RenderDoc 目前只有抽象（`IGpuCaptureTool`），还没有实现模块。
- PIX 和 DRED 只支持 D3D12，等 `RHID3D12` 做的时候再实现。

进 Shipping 的调试设施：

- breadcrumb、Aftermath/DRED 进 Shipping。
- breadcrumb 和 Aftermath event marker 记的是 pass 名字在静态字符串表里的 ID，崩溃报告里再还原成名字。
  所以驱动层的调试名（`vkSetDebugUtilsObjectName` 等）不进 Shipping：既不用每帧调用驱动，也不会泄露内部名字。

### 1.8 待定

- 纹理格式表，以及 sparse 资源的页大小。
- 多 GPU：建议不支持。
- device lost 后的设备重建与恢复（目前直接退出）。
- DirectStorage 的衔接：D3D12 接入 DirectStorage（12 §1.3）；Vulkan 没有对等的 API，先走 Copy 队列。

## 2. RenderGraph

- 每帧声明 Pass 及其资源读写，自动推导 barrier、资源别名（transient 资源复用堆内存）、async compute 调度，并剔除无用 Pass。
- Pass 的录制按 job 并行，最后按队列提交。
- 调试：可以通过 Remote 导出图（10）；编辑器里有 RenderGraph 可视化面板，可以查看任意中间纹理。
- 当前进度：`Runtime/RenderGraph` 和 `Runtime/RenderGraphRemote` 已实现。
  - `RenderGraphBuilder` 每帧一个，在渲染提交线程上 `Execute` 一次。资源：`CreateTexture` / `CreateBuffer`（transient，usage 由 pass 的访问推出）、
    `ImportTexture` / `ImportBuffer`（外部资源，带起始和最终 layout，`Execute` 成功后写回状态；`Concurrent` 的 buffer 跨队列不做所有权转移）。
  - `AddPass(name, queue, setup, execute)`：setup 用 `RenderGraphPassBuilder` 声明访问（`ReadTexture`、`WriteTexture`、`OverwriteTexture`、`Buffer` 等，
    SRV 和 UAV 访问自动得到 bindless view）、render target、depth target、shading rate 和 `NeverCull`；execute 在 job 上用 `RenderGraphContext` 录制。
    `OverwriteTexture` 表示整段写、不读旧内容，之前的内容直接丢弃；读了还没人写过的内容会警告「Pass … reads … before anything writes it」。
  - 编译：剔除结果没人用的 pass（`r.RenderGraph.Culling`）；纹理按子资源跟踪 layout，同一队列上的 buffer 用全局 barrier；
    exclusive 资源跨队列时插 release/acquire 和等待（§1.4）。Compute / Copy pass 在各自的队列上运行，`r.RenderGraph.AsyncCompute` / `r.RenderGraph.AsyncCopy` 关掉时并到 Graphics。
    每个用到的队列有 `RenderGraph.Prologue.<Queue>` 和 `RenderGraph.Epilogue.<Queue>` 两个 pass；每个队列先等上一个图的 Graphics epilogue，所以图和图之间有序。
  - transient 资源按首次使用的顺序放进 `RenderGraphTransientPool` 的 heap（`Engine.RenderGraph.TransientHeapSize`，更大的资源单独一个 heap），
    生命周期不重叠的资源共用内存（`r.RenderGraph.Aliasing`，带 alias barrier）；放好的资源按（描述、heap、偏移）缓存复用，
    `RetainGraphs` 个图没用到就释放。memoryless 纹理不分配内存。
  - 执行：每个 job 录 `r.RenderGraph.PassesPerList` 个 pass 到自己的 command list；提交按需要等待或 signal 的位置切段，按队列提交。
    每个队列一个 `RhiGpuProfiler`，给每个 pass 计时，`RenderGraphSubsystem::GetLastGpuFrame(queue)` 返回 GPU 最近完成的那个图的 pass 时间。
  - 工具：`RequestExport` 把下一个执行的图编译后的 `RenderGraphExport`（pass、访问、barrier、等待、资源放置、提交）交给回调；
    `RequestPassCapture(first, last)` 用注入的 capture 工具截下一个图里从 first 到 last 的 pass，提交在两端切开（§1.7）。
    `RenderGraphRemote` 把它们作为 Remote 的 `Render.Graph` 和 `Render.CapturePasses` 提供（10）。
  - 默认值：`PassesPerList = 8`、`TransientHeapSize = 64 MiB`、`RetainGraphs = 3`，四个开关默认都开。
  - 光栅 pass：有 render target、depth target，或者只用 `SetRenderArea` 给了渲染区域（无附件光栅，比如用 atomic 写结果的 VisBuffer）的 pass，
    在 `BeginRendering` / `EndRendering` 之间执行；执行器把 viewport 和 scissor 设成整个渲染区域，pass 可以再改。
    `OverwriteBuffer` 是 `OverwriteTexture` 的缓冲区版本：UAV 写、不读旧内容，可以只写一部分（比如读者按计数读的追加列表），
    所以 transient 缓冲不需要更早的写者，也不会报「reads … before anything writes it」。
    每次 `BeginRendering` 都把 shading rate 设回 1x1（有 shading rate 附件时用附件的）：开了 VRS 时所有图形 pipeline 都把它当动态状态。
  - 尚未实现：编辑器的 RenderGraph 可视化面板和中间纹理查看、Remote 的纹理回读。

## 3. 主管线

**决策**：Visibility Buffer + GPU Driven。

```
Instance + Cluster Culling (compute，两阶段 HZB 剔除，输出可见 cluster 列表，§5.3)
  → Visibility Buffer Raster (mesh shader → 64 位 atomic：深度|可见 cluster|三角形，§5.3)
  → Material Classification (按 tile 统计用到的材质)
  → Material Resolve (每种材质一个 compute 或全屏 pass，只处理 tile 中属于自己的像素；输出紧凑的 Substrate 风格 BSDF 数据)
  → Lighting (clustered lights + 阴影 + GI) → Translucency (forward) → Post → UI 合成
```

- 几何：网格转换时切分 cluster（≤256 顶点 / 128 三角形，`Mesh::MaxClusterVertices` / `MaxClusterTriangles`）并生成层级 cluster，为以后做虚拟几何（cluster LOD）留出空间。
  128 三角形的上限由 VisBuffer 的 7 位三角形编号决定（§5.3）。
- 光追：BLAS 由 Converter 预先构建，TLAS 每帧更新；Ray Query 用于阴影、反射、AO。之后可以加 Path Tracer 参考路径（§4.3 的自定义 BSDF 要求）。
- 当前进度：管线本身尚未实现。只有几何数据：`Runtime/Mesh` 的静态网格是 cluster DAG，按页流送（06），由 `MeshPipeline` 用 `ClusterLod`（并行的 cluster DAG 构建，06 §3.5）生成。

## 4. 材质

### 4.1 两类 shader

| 类别 | 文件 | 用途 |
| --- | --- | --- |
| **Pass shader** | `Language/Shaders/Passes/*.mshader` | 渲染管线直接使用的 VS/MS/PS/CS/RT shader |
| **Material shader** | 项目或引擎中的 `*.mshader`，带 `[Material]` 的 struct | 描述表面；由引擎插入到各个 Pass 的材质接口中 |

**决策**：不提供节点图，所有材质都用 Marlin Shader 写，这样 AI 和程序员都能直接阅读和编写。

- Pass shader 的入口点就是**全局 shader**（MarlinLanguage §1.6）：引擎或项目的 `Language/Shaders/Passes/` 下的入口点，按限定名
  （例如 `Marlin.Passes.Ui.Raster`）引用。C++ 在 pass 旁边用 `ComputeShader<TArgs>` 声明一次，`ShaderLibrary::Dispatch` 调度（§1.4）；
  `TArgs` 必须和入口点的参数块（`[Uniform]` 参数按声明顺序组成的 struct，MarlinLanguage §9.3）按名字、偏移和大小一致，加载时检查，不一致就拒绝调度。
  参数块超过 128 字节时 `ShaderLibrary` 把它写进每帧的上传内存，只把指针放进直接参数区，C++ 一侧的写法不变。
- 当前进度：全局 shader 已实现（`Runtime/Shader`、`Developer/ShaderPipeline`、`Runtime/GlobalStoreShaderStore`），目前只有 compute 入口点；用到的是 UIPainter 的 7 个 pass（09 §2）。
  - `ShaderLibrary` 启动时选设备的 profile：`Engine.Shaders.Profiles` 里第一个 RHI 匹配、特性都有的。每个全局 shader 按 profile 编译，代码相同的 profile 共用一个变体。
  - 开发时 `GlobalStoreShaderStore`（Shipping 之外）经 indexer 的 `Shader.Locate` 按入口点定位，未命中就当场编译；改了源码会重新加载并重建 pipeline。
    结果 key 包括转换器版本、编译器各模块（LanguageFrontend、LanguageIR、SPIR-V 后端、ShaderPipeline）的源码 hash、入口点、target、profile 和选项，
    再加上次编译读过的每个源文件现在的 hash，所以改编译器或改入口点用到的任何文件（直到核心库）都会未命中。没用到的文件只在新增或改动后
    声明了编译查找过的名字时才未命中（名字记录，06 §3.2）：加一个更近的同名声明会重新编译，加一个无关的材质文件不会。
  - indexer 启动后在后台为本机预热（warm）全局 shader。`MarlinConverter -Warm`（CI 预热 GlobalStore）还不包括全局 shader。
  - 打包时 `MarlinPackager` 把全局 shader 为目标平台编译并打进包里（`GlobalShaders`），运行时从包里按 `GetGlobalShaderGuid(名字)` 加载，和资产一样。
  - 图形入口点：C++ 用 `TaskShader<TArgs>`、`MeshShader<TArgs>`、`VertexShader<TArgs>`、`PixelShader<TArgs>` 声明阶段，
    一个 pipeline 的各阶段共用同一个参数块，不读参数的阶段可以不声明 `[Uniform]` 参数（参数类型 `NoShaderArguments`）。
    pass 用 `ShaderGraphicsPipelineDesc`（阶段加固定状态和目标格式）画：`ShaderLibrary::DrawMesh` / `DrawMeshIndirect` / `Draw`，`GetPipeline(desc)` 取 pipeline。
    每个不同的 desc 一个 pipeline，key 是 desc 加各阶段代码的 hash，所以某个阶段重新加载后会建新的；
    pipeline 清单的 payload 是各阶段入口点名和状态，靠它能重建。所有阶段加载完、pipeline 就绪前，绘制跳过并返回 false。
  - 材质 pass（泛型入口点，MarlinLanguage §9.4）按「入口点 × 材质 × 变体」编译：shader key 写作 `entry|material|switch=value;…`
    （switch 按名字排序），Guid 是 `GetGlobalShaderGuid(key)`，所以 GlobalStore、indexer 的 `Shader.Locate`、热重载都和全局 shader 走同一条路；
    编译时才把 key 拆开（`ParseShaderKey`）。C++ 用 `ShaderLibrary::GetMaterialShader(pass, material, variant)` 拿到这个排列的声明，
    像普通的 `ComputeShader<TArgs>` 一样调度；泛型入口点不算全局 shader。泛型参数也可以是普通 struct（不是 `[Material]`，没有变体），
    比如自定义 BSDF 的光照 pass（§4.3）。
  - Specialization constant：每个变体记下自己的 `[Specialization]` 常量（名字、id、默认值的 32 位）。
    `ShaderLibrary::GetSpecializedShader(shader, values)` 给出一个设了某些常量的声明（按限定名或短名，其余用默认值）：
    它的 key 是 `key#name=bits;…`，加载的仍是不带后缀的那个 shader（`GetShaderKeyAsset`），但有自己的 entry 和管线；
    管线记录的 payload 就是这个 key，所以管线列表能按它重建；管线 key 是代码 hash 加这些值。名字不存在的常量是错误，
    那个声明不调度。

### 4.2 Substrate 风格的 BSDF 组合

```csharp
// Rock.mshader
using Marlin.Shading;

[Material, Id("9a0b")]
public struct Rock<bool UseMoss = true> : ISurfaceMaterial
{
    public MaterialTexture2D<half4> BaseColor = DefaultTextures.White;
    [Range(0, 1)] public half Roughness = 0.5h;

    public Surface Evaluate(in SurfaceInputs input)
    {
        half4 albedo = BaseColor.Sample(input.Uv0);
        Slab rock = Slab.Default with { Albedo = albedo.rgb, Roughness = Roughness };
        Slab moss = Slab.Default with { Albedo = half3(0.2h, 0.4h, 0.1h), Roughness = 0.9h, Fuzz = 0.5h };
        return Surface.VerticalLayer(top: moss, bottom: rock, thickness: input.WorldNormal.y * 0.3h);
    }
}
```

- `Surface` 是一棵编译期可见的值树（`Slab`、`HorizontalMix`、`VerticalLayer`、`Weight`……）。编译器把它展平成闭包列表，
  再由 Visibility Buffer 的 Material Resolve 序列化成紧凑格式（一个像素可以有多个闭包，tile 按复杂度分类：Simple / Single / Complex）。
- 材质是带 `[Material, Id(…)]` 的 struct，实现 `ISurfaceMaterial.Evaluate`（不再是 `material` 修饰的函数）。
- 材质的 public 字段就是参数，自动导出到编辑器（`[Range]`、`[Color]` 等特性给属性面板用）：材质资产的属性面板就是这些参数，材质实例用 MarlinText 的继承语法覆盖参数。
- 变体：只用材质的泛型参数（例如 `UseMoss`，可以有默认值）和 specialization constant，编译器会统计可能的组合数，超出预算就报错。

### 4.3 自定义 BSDF（例如 Toon）

```csharp
public struct ToonBsdf : IBsdf
{
    public half3 Color; public half Steps;
    public half3 EvaluateDirect(in BsdfContext ctx, in LightSample light)
        => Color * Floor(Saturate(Dot(ctx.N, light.L)) * Steps) / Steps;
    // 未实现 Sample / Pdf：路径追踪路径跳过此材质
}

[Material, Id("…")]
public struct Hero : ISurfaceMaterial
{
    [Color] public half3 Color = half3(1);
    public Surface Evaluate(in SurfaceInputs input) => Surface.Custom(new ToonBsdf { Color = Color, Steps = 3 });
}
```

- `IBsdf` 的必需成员是 `EvaluateDirect`；可选成员是 `Sample`、`Pdf`、`EvaluateIndirect`。编译器通过静态分派检查实际实现了哪些，并生成能力位。
- 自定义 BSDF 在 Material Resolve 中占一个**专属闭包类型槽位**（由编译器分配 ID，带用户数据载荷），Lighting Pass 对这种 tile 实例化对应的 `EvaluateDirect`。
- 路径追踪路径：遇到没有 `Sample` / `Pdf` 的闭包，就不渲染（输出调试色或直接跳过）。
- 所以只用 shader 语言就能加一种新的渲染风格，不需要改 C++。

### 4.4 实现现状

- **着色库** `Language/Shaders/Shading/`（`Marlin.Shading`）：`Slab`（albedo、roughness、metallic、specular、法线、fuzz、自发光）、
  `Closure`、`Surface`（最多 4 个闭包，`FromSlab`、`HorizontalMix`、`VerticalLayer`、`Weight`、`Custom<T>`）、`SurfaceInputs`、
  `ISurfaceMaterial`、`MaterialTexture2D`、`DefaultTextures`、`IBsdf`、`BsdfContext`、`LightSample`、内置的 `SlabBsdf`（Lambert + GGX +
  Charlie fuzz，环境光用 Karis 的解析环境 BRDF）。`VerticalLayer(top, bottom, thickness)` 是真实分层：顶层第一个 Slab 是层的物质，
  `TransmittanceColor` 是光垂直穿过一个单位厚度剩下的比例；底层每个闭包记下上面各层的不透明度（`CoatOpacity`，
  一减 `TransmittanceColor^thickness`，金属层不透光）和表面反射率（`CoatSpecular`，F0 的平均）；光照对被覆盖的闭包乘
  `T^(1/cos l + 1/cos v)` 和两次穿过表面的 `1 − Fresnel`（环境光按法线方向进入，自发光只走出去的一次）。顶层自己的漫反射只散射
  没透过去的那部分（`Albedo × (1 − T)`），所以薄的透明层只加一层反射，厚的层盖住底层。覆盖率由 `HorizontalMix` 负责。
  被覆盖的闭包不进 Slab plane，记在溢出记录的最后一个字里（Unorm8x4）。
  - 形状必须编译期确定：`Surface` 的操作末尾用 `Compiler.Require(count <= 4, …)`，内联、展开、折叠之后条件必须是常量 true，
    否则报错（依赖运行时值时附注说明）。
  - 自定义闭包：`Compiler.TypeId<T>()`（限定名 XXH3 的 16 位，非零）是闭包类型 ID，`Compiler.ToWords/FromWords` 把 BSDF 的标量
    （最多 8 个，32 位以内）存进闭包载荷。
  - 引擎材质：`Marlin.Materials.Default`（`Content/Materials/Default.masset`，没有材质的槽位都用它）和 `Marlin.Materials.Toon`。
- **贴图导数**：Material Resolve 用两方向前向自动微分（MarlinLanguage §9.12）对 `Evaluate` 求导，输入是重心坐标在屏幕 x、y 方向上
  一个像素的差分插值出的属性；`MaterialTexture2D.Sample` 的 `[ForwardDerivative2]` 同时拿到 UV 的两个方向导数，调用 `SampleGrad`。
- **资产**：`Material` 和 `MaterialInstance`（06 §3.5）转换成同一个运行时 `Material`：struct 的限定名、变体值、参数块（材质表一个
  元素的 GPU 布局，默认值来自字段初值）、贴图绑定、可能用到的自定义 BSDF。贴图由 `SceneTextureResidency` 按需流送（见下）。
- **贴图流送**（UE 的 texture streaming，用 GPU 反馈决定 mip）：贴图先只读尾部（`TailMips` = 7 级，64×64 及更小），尾部一直常驻。
  `MaterialTexture2D` 多一个 `Feedback` 指针（第 16 字节，`SceneMaterialRegistry` 写入；材质检查允许它这一个可写字段）：
  Material Resolve 的 `SampleGrad` 按 UV 的两个屏幕导数（16× 各向异性）算出这次采样需要 UV 范围里多少 texel，把 log2 + 1 用
  read-before-atomic 的 `Atomic.Max` 写进每张贴图一个字的反馈 buffer；每帧的 render job 把它拷进 readback 再清零，主线程在 GPU 做完那帧后
  读回，换算成要的 mip（加 mip bias）。更细的 mip 从 bulk 里连续读一段，贴图按新尺寸重建：新读的 mip 上传，已有的在 GPU 上拷过去，
  descriptor 变了，`GetResidentStamp` 增长让材质块重写。要的比常驻的粗、持续 `RelaxFrames`（30）帧，或 `UnseenFrames`（300）帧
  没被采样，就丢掉最细的；超出贴图池时，最久没用的先每帧丢一级（不低于尾部），读新 mip 时比它更久没用的先让位，放不下就少读几级。
  设置：`r.Scene.TextureStreaming`（关掉就整张加载）、`r.Scene.TexturePoolSize`（MiB，默认 1024）、`r.Scene.TextureMipBias`、
  `r.Scene.MaxAnisotropy`（改了重建 sampler，旧的等 `RetireFrames` 再销毁）。
- **渲染**（`SceneMaterialFeature`、`SceneLightingFeature`、`SceneTonemapFeature`）：
  - `SceneMaterialRegistry` 给每个材质一个槽位，同 struct 同变体的材质共用一个排列（一个 material pass、一张参数表）；
    section 表（每个 mesh section 一个材质槽位）由 mesh 的默认材质填写；每帧整张上传槽位表和各排列的参数表。
  - Classification：每个像素的排列，每个 8×8 tile 挂到它含有的每个排列的 tile 列表下。
  - Resolve：每个排列一个 `Resolve<TMaterial>`，只处理自己的像素。闭包格式：每像素一个 header（闭包数、第一个闭包是否在 Slab plane、
    溢出记录起点）、6 个字的 Slab plane（albedo 10:10:10、八面体法线、roughness/metallic/specular/fuzz、fuzz 颜色、自发光 half）、
    12 个字的溢出记录（类型、权重、Slab 或自定义载荷、法线）。tile 标记分出 Simple（只有朴素 Slab）、Single（每像素一个闭包）、
    Complex（多个闭包或自定义 BSDF）。
  - Lighting：每个 tile 类别一个 pass（太阳 + 环境光，没有阴影），再为每个用到的自定义 BSDF 编译一个 `LightCustom<TBsdf>`，
    叠加到 Complex tile 上。灯光来自 `DirectionalLight`（实体的 −Z）和 `SkyLight` 组件，世界里没有时用 `r.Scene.SunIlluminance` /
    `r.Scene.AmbientIlluminance` 的默认值。
  - Post：曝光（`r.Scene.Exposure`，EV100）加 ACES 拟合曲线到 UI 范围。
  - 显示模式：默认 Lit；Buffer 模式新增 Base Color、Shading Normal、Roughness、Metallic、Closures、Tile Class（这些模式也跑材质阶段）。
- 验证：Sandbox 的形状用 Default，圆环用 Toon（自定义 BSDF 的分色带）；`ShaderTests`（两方向导数、方法与接口的可微、Surface 展平与
  形状错误、材质反射）、`ShaderPipelineTests`（各 pass 编译通过 SPIR-V 校验）、`MaterialPipelineTests`（转换、实例覆盖、错误）。
- 其他设置：`r.Scene.MaterialTileClasses` 关掉时所有画到的 tile 都按 Complex 打光（对照、调试分类）；曝光和默认灯光见上。
- 尚未实现：材质参数面板与预览视口（09，缺控件）；路径追踪；编辑器里新建材质时自动写 `[Id]`；
  `RenderMesh` 覆盖的源数据按槽位名书写（现在按 section 下标，§5.2）。

## 5. SceneRenderer 与视口

**决策**：`RenderScene`、`SceneView`、`SceneRenderer` 放在 `Runtime/SceneRenderer` 模块里，依赖 `Renderer`、`World`、`Mesh`。
`Renderer` 只管帧、surface 和图像槽，不认识 World；Server 不链接 `SceneRenderer`（同 `UIPainterRenderer` 的做法）。

### 5.1 对象

- **每个 World 一个 `RenderScene`**（WorldSubsystem）：持有 GPU Scene（实例缓冲、几何池的引用、灯光、材质表），只上传有改动的部分（§5.2）。
  标了 `[GpuMirrored]` 的组件列也放在 GPU Scene 里，供 `.mshader` 的 `[ForEachEntity]` compute 使用（08 §5.5）。
- **`SceneView`** = 相机 + 视口尺寸 + 显示模式（§5.4）+ 调试叠加 + 图像槽（§5.5）。
  - **决策**：SceneView 是调用方持有的长期对象。Game 或编辑器视口创建它并注册到 RenderScene，销毁时注销；
    它持有图像槽和跨帧状态（HZB、以后的 TAA 历史、自动曝光），每帧只更新相机和尺寸。
  - **决策**：相机是 SceneView 自带的普通结构（`WorldPosition`、朝向、投影），不引用实体。编辑器视口直接设置；
    Game 由一个 System 把 `Camera` 组件拷进自己的 SceneView。
- **`SceneRenderer`** 是 `SceneViewsFrameStage` 上的 `RenderFrameFeature`：每帧渲染各 RenderScene 注册的 SceneView，
  同一个 RenderScene 的 view 共享剔除前的数据、阴影图集和 GI 缓存；剔除和光照则按 view 分别执行。
- 编辑器中：同一个关卡的多个视口（透视加三视图）= 同一个 World 和 RenderScene，对应多个 SceneView；
  材质预览 = 一个使用 `AssetPreview` 派生 Profile 的独立 World，加一个 SceneView；PIE = 一个使用 `Game` Profile 的独立 World。

### 5.2 GPU Scene 与 Extract

- **可渲染实体**：**决策** 第一种图元是网格：Shared 组件 `RenderMesh`（Mesh 资产和材质列表）加 `WorldTransform`，同网格同材质的实体在同一个 chunk 里。
  **决策**（材质列表）：Mesh 的每个 section 是一个材质槽（部件 × 材质，06 §3.5），默认材质随 Mesh 资产；`RenderMesh` 的材质列表只写覆盖，
  源数据里按槽位名写，转换成按 section 下标的表。Material Resolve 按「实例的 section → 材质」查表，两个槽位用同一个材质在材质阶段不多花成本。
  Mesh 的默认材质已实现（`MeshSection::DefaultMaterial`，§4.4）。`RenderMesh::Materials` 是按 section 下标的表
  （`SoftAssetRef<Material>[8]`，空的用默认材质；entity template 只存纯数据，所以不是 List）；同一个 mesh 和同一张表的实体
  共用一个「材质绑定」（`SceneRendererSubsystem::AcquireMaterialBinding`，引用计数），mesh 加载后为它在 section 表里建一份，
  实例的 `SceneGpuInstance::Materials` 是绑定号，Material Resolve 经绑定表找到它的 section 起点（还没建好时用 mesh 的默认）。
  尚未实现：源数据里按槽位名写、转换时换成下标。
  图元类型开放注册：每种图元（以后的粒子、地形、植被……）注册一个 extract 提供者，自己查询 ECS、往 GPU Scene 写实例，RenderScene 不写死图元种类。
- **实例槽位**：**决策** RenderScene 给每个可渲染实体分配稳定的 instance index（free list，实体销毁或不再可渲染时回收，等 GPU 用完再复用）。
  GPU 剔除、VisBuffer 和以后的 TLAS 都用这个 index 当 InstanceId；实体在 chunk 间移动不改变它。
- **大坐标**：**决策** 实例的位置存成整数 tile 坐标加 float 局部变换（由 `WorldTransform` 的 double 平移拆出来）。
  每个 view 在 shader 里精确地减去自己相机的 tile，得到相机相对的 float。所以多个 view 共用一份实例数据，相机移动不需要重新上传，
  view 之间离得再远也不丢精度（05 §6）。
- **增量 Extract**：**决策** `PresentationSet` 末尾的一个 System 按 chunk 并行，用列版本号（05 §3 的 `Changed` 过滤）找出改过的 chunk，
  把改了的实例槽写进 RenderScene 的双缓冲更新列表；`RenderFrameFeature::Extract` 在主线程上只交换缓冲，渲染 job 录制上传（`RhiUploadQueue` 或每帧环形缓冲）。
  主线程上不遍历 chunk。

### 5.3 几何与 Visibility Buffer

- **几何池**：**决策** 所有 Mesh 共用一个全局几何池（cluster、顶点、索引按页子分配），实例和 cluster 用池里的偏移引用。
  第一版在 Mesh 加载时把所有页上传并常驻，只画最精细的一层；池的布局按 Mesh 的页（06）组织，以后直接接页流送和 cluster LOD。
- **页流送与 cluster LOD**：**决策**（参照 Nanite 和 vk_lod_clusters）
  - cluster 记录跟页一起流送：页里先是 GPU 格式的 cluster 记录，再是顶点和三角形；`Mesh` 块里只有组、层级、页表和页依赖，没有 cluster 表。
    页之间的依赖：页 P 的组简化出的 cluster 所在的页是 P 的父页；父页都在显存时才加载 P，P 的子页都不在显存时才淘汰 P。
  - 显存里常驻每个网格的组、层级节点和页表（页 → 槽位，或不在显存）；页池是一个按 `PageSize` 切成固定槽位的 buffer，
    cluster 记录放进一个按槽位分区的 cluster 池，地址用槽位号，不受 32 位字节偏移的限制。root 页（最粗的几级）一直常驻。
  - 遍历：实例先测视锥，把网格每个深度的根节点放进队列；一个持久线程的 compute kernel 用原子队列走完所有层级（节点误差投影到屏幕上
    大于阈值就往下走），叶子是组：组的页在显存就进组列表，不在就记一条页请求（页表项上原子去重），带上这个组的误差投影到屏幕上的像素数
    （一帧里同一页取第一个请求者的）。之后每组一个 workgroup，
    cluster 自己所在组的误差太大、并且它的 `RefinedGroup` 误差够小或不在显存时画它；再做视锥、背面锥、HZB，接两阶段剔除。
    持久线程每轮只看一次队列项，不在内层自旋，同一 wave 的线程不会互相等。
    误差投影照 Nanite 的 `GetProjectedEdgeScales`：取包围球最近的深度乘以离视轴最远那条切线的余弦（偏离视轴 θ 的长度投影后放大 1/cos²θ），
    而不是只用到球面的距离；只用距离时屏幕角落的误差会到阈值的两倍多，相机转动时物体移到边缘就能看出换 LOD。
  - 流送：请求列表和每个槽位最后一次被用到的帧号复制进 Readback buffer，GPU 完成那一帧后在主线程读回；按依赖补齐父页、
    合并同一网格的相邻页交给 `BulkStreamer`。排队的页按屏幕误差从大到小开始读，让画面改善最大的先到：误差取最近一次读回里请求它的值
    （更早的请求作废），父页的误差至少是子页的，误差相同时父页先读；往父页传播只在误差真的提高时继续，不会沿 DAG 的每条路径走一遍；到达后复制进槽位、更新页表；槽位不够时按最久没用的、没有子页在显存的页淘汰，
    页表先改、槽位等 GPU 用完再复用。
- **VisBuffer 编码**：**决策** 一开始就用 64 位 atomic 写法，为以后软光栅化小三角形做准备：
  高 32 位是深度，低 32 位是 25 位可见 cluster 编号 | 7 位三角形编号。硬件光栅（mesh shader）在像素 shader 里用 64 位 atomic max 写入，不用深度附件。
  VisBuffer 是每个 view 一个 Width × Height 的 `ulong` 数组（按行排列，shader 里是 `Span<ulong, Access.ReadWrite>`），用 64 位 **buffer** atomic：
  `shaderBufferInt64Atomics` 在基线里（RTX 20 / RDNA2 / M2 都有），而 64 位 image atomic（`VK_EXT_shader_image_atomic_int64`，特性 `ImageAtomic64`）是可选的，
  语言里也还没有纹理 atomic。深度是 reversed-Z，近处更大，空像素是 0。
  - 可见 cluster 编号是剔除阶段输出的可见 cluster 列表里的下标（每个 view 最多 2^25 个），列表项里记 instance index 和 cluster 在几何池里的位置。
    所以即使第一版不剔除，也有一个 pass 输出可见 cluster 列表（列出所有实例最精细一层的 cluster）。
  - 7 位三角形编号决定了每个 cluster 最多 128 个三角形（§3）。
- **软光栅**：**决策** 屏幕上小的 cluster 用 compute 光栅化，大的仍走 mesh shader：三角形只有一两个像素时，硬件按 2×2 像素块执行像素 shader，
  加上每个三角形的建立开销，大部分工作是浪费的（完整 Zorah 在 720p、0.5 像素误差下约 108 万个 cluster，硬件光栅化占 GPU 帧时间约 90%）。
  - 分流：两个剔除阶段把可见 cluster 追加进同一个列表（编号照旧用于 VisBuffer），同时把编号记进硬件列表或软件列表。
    和 Nanite 的 `r.Nanite.MinPixelsPerEdgeHW` 一样按三角形边长分：转换时给每个 cluster 记下最长边（`MeshPageCluster::MaxEdgeLength`，
    网格空间），剔除时按包围球最近处的距离投影到屏幕上；包围球完全在近平面之前、最长边短于 `Engine.SceneRenderer.SoftwareRasterPixels`
    （`r.Scene.SoftwareRasterPixels`，默认 32）的进软件列表，0 表示全部走硬件。阈值衡量的是三角形本身的大小，所以基本不随场景变，
    主要看 GPU 上两条路径的相对速度。（最初按包围球投影直径分，最佳值随 cluster 的三角形密度变化，在 Zorah 上要 128 左右才划算。）
    计数器里两个列表各有总数和第一阶段的数量，每个阶段各画自己新增的那段。
  - 软光栅（`RasterizeSoftware`）：每个 cluster 一个 128 线程的 workgroup，先把顶点变换到像素坐标（和硬件相同的翻转视口映射，
    吸附到 1/256 像素网格）放进 groupshared，再每个线程一个三角形：单面 cluster 的背面三角形直接丢掉（双面的两面都画，06 §3），
    在包围盒内的像素中心上求边函数，压在边上的像素按 top-left 规则归属（共享一条边的两个三角形恰好一个覆盖它），
    深度在屏幕空间线性插值，64 位 atomic max 写进 VisBuffer。软光栅不做近平面裁剪，所以与近平面相交的 cluster 一律走硬件。
  - 硬件光栅的 mesh shader 用齐次坐标 (x, y, w) 的行列式判断朝向（相机两侧的顶点都对），单面 cluster 的背面三角形输出成退化三角形。
    镜像实例（奇数个负缩放）的朝向反过来。两条光栅写 VisBuffer 前先普通读一次，像素里已经有更近的值就不做 atomic
    （值只会变大，读到旧值最多多做一次 atomic，不会丢掉更近的片元）。
  - Zorah 默认视角的测量（GPU pass 计时，720p）：第一阶段约 38 万个软光栅 cluster、1.3 万个硬件 cluster，软光栅约占 GPU 帧的四分之三。
    去掉像素循环后仍有一大半，只启动 workgroup 就要约 1.3 ms，所以软光栅每个 cluster 一个 64 线程的 workgroup（每线程最多两个三角形）：
    同样 1.67 px 的阈值下软光栅从 128 线程的约 5.0 ms 降到约 3.5 ms（32 线程反而约 4.0 ms）。位置量化（06 §3）解码多几条指令，
    在 128 线程时让软光栅慢了约 0.5 ms，但同样的画面页池需求从 82% 降到 57%，页池过载降质量后的质量从 0.60 升到 0.94
    （阈值约 1.06 px，接近 Nanite 的 1 px），GPU 帧约 7.9 ms。把反量化、实例变换和投影合成每个 cluster 一组行向量的尝试更慢
    （每个线程只处理一两个顶点，每线程各算一次反而更贵），没有保留。

### 5.4 View 阶段与显示模式

- **View 阶段**：**决策** 照搬帧阶段的做法，`MSTRUCT(SceneViewStage, After = A, Before = B)` 定义 view 内的阶段
  （Culling、VisBuffer、Material、Lighting、Translucency、Post……）。`SceneViewFeature` 是无状态的提供者，用 `MCLASS(Stage = …)` 挂在阶段上，
  对每个 view 往图里加 pass；阴影、GI、超分、编辑器的选中描边都这样接入，SceneRenderer 不写死主流程。
  跨帧状态放在 SceneView 上，feature 按自己的类型取。
- **显示模式**：**决策** `ViewMode` 开放注册，不用枚举。每种模式是一个反射类型，决定打开哪些 SceneViewFeature、最后输出哪张纹理。
  Buffer 可视化（instance / cluster / 三角形着色、深度、法线……）由写那张纹理的 feature 自己注册；编辑器的显示模式菜单从注册表生成。

### 5.5 视口合成

- SceneView 输出的纹理作为图片交给 UIPainter 合成（09 §2）；HDR 显示器上视口以 HDR 输出，UI 以 SDR 白点合成。
- **决策**：UI 合成一切。SceneView 只渲染到图里的纹理，窗口的 swapchain 只由 UIPainter 写；Game 用全屏 `DrawImage` 画自己的视图，HUD 画在上面，编辑器视口同样是 `DrawImage`。
  HDR 编码（PQ）和 UI 白点缩放只在 UIPainter 的最后一个 pass 里做一次。
- **帧图像槽**：SceneView 持有 `RendererSubsystem::AllocateImageSlot` 分配的槽，UI 在 UI 阶段录制 `DrawImage(UIImage::FromFrameSlot(slot), rect)`，rect 同一帧决定视图大小；
  构建帧图时 SceneRenderer 的 pass 先执行，用 `RenderFrame::BindImage` 把槽绑到自己的输出纹理（带值域：UI 白点或显示 nits），UIPainter 把它声明为读取。

### 5.6 进度与计划

- 当前进度：`Runtime/Renderer` 已实现。`RendererSubsystem` 在每帧末等上一帧的渲染 job，在主线程调用各 `RenderFrameFeature::Extract`，再启动大栈 job：
  获取各 surface 的图像、按帧阶段顺序让各 feature 往一个 RenderGraph 里加 pass、执行、present。帧阶段是 `MSTRUCT(RenderFrameStage, After, Before)` 的反射类型，
  锚点是 `SceneViewsFrameStage` 和 `UiFrameStage`；feature 用 `MCLASS(Stage = …)` 挂在阶段上。`RenderSurface` 每个 OS 窗口一个（`CreateSurface(window)`），
  图像格式 `RGB10A2Unorm`，带 UnorderedAccess（UIPainter 用 compute 写整个窗口），每帧导入图里、最后转到 Present layout。
  - 渲染 job 是这一帧唯一的提交者（§1.2）：`RendererSubsystem` 初始化时 `ClaimFrameSubmission()`，job 开头调用 `BeginSubmitFrame(frame)`，
    每个 surface 先 `WaitForFrame`（present 钩子或 `WaitForPresent` 限帧）再 `AcquireNextImage`。同一时间最多一个渲染 job 在跑，第 N 帧渲染时第 N+1 帧在模拟。
  - surface 在每帧开头跟随窗口大小；最小化或尺寸为 0 时这一帧跳过它；acquire 或 present 报 surface lost 时下一帧重建 swapchain。
  - HDR：显示器处于 HDR、`Engine.Renderer.Hdr` 和 surface 的 `AllowHdr` 都允许时用 HDR10，否则 sRGB。每 60 帧查一次显示器的 HDR 状态和 SDR 白点，
    HDR 状态变了就重建 swapchain。`RenderSurface::GetUiWhiteNits()` 取 `r.UiWhiteNits`，为 0 时跟随显示器的 SDR 白点。
  - 帧图像槽的分配和释放（`AllocateImageSlot` / `FreeImageSlot`）可以在任意线程上调用。
  - 帧 feature 有两个：`SceneViewsFrameStage` 上的 `SceneRendererFrameFeature`（下面）和 `UiFrameStage` 上的 `UIPainterFrameFeature`（09 §2）。
- 前置工作已完成：图形入口点的全局 shader（§4.1）、图形 PSO、RenderGraph 的光栅 pass（§2），GPU 测试
  `RenderGraph.RastersMeshShadersIntoTargetsAndAtomics` 覆盖 mesh → 像素写颜色目标，以及无附件 pass 里的 64 位 buffer atomic。
- SceneRenderer 第一版已实现（`Runtime/SceneRenderer`）：
  - `RenderMesh`（Shared，目前只有 `Mesh`；材质列表等材质资产）、`RenderInstance`（槽位，系统维护，不手写）、`Camera` 组件，
    放在单独的 `Runtime/RenderComponents` 模块：转换关卡和 Prefab 的 `MarlinConverter` 要认识它们，但不链接渲染器。
  - 相机分三层：`Camera` 组件和把它拷进 SceneView 的 `GameCameraSystem`（C++）；**rig** 是组件加系统，在 `CameraPhase` 里写相机的 `LocalTransform`，
    每种一个组件、不用模式枚举，常用的（跟随、轨道、碰撞回缩）由 C++ 内置；用什么 rig、参数怎么随输入和玩法变由脚本决定，脚本也可以写自己的 rig。
    第一个 rig 是 `FlyCamera`（调试用自由飞行）：`GameViewport` 里的 `FlyCameraSystem`（`CameraPhase`，`GameplaySet`）读 Shared 组件
    `FlyCameraInput` 引用的输入动作（15），并在有 FlyCamera 实体时保持它的 `InputContext` 处于活动状态；引擎自带的绑定在
    `/Engine/Content/Input/FlyCamera`：按住右键转向并用 WASD / QE / Shift / 滚轮移动和调速，摇杆和扳机同理，`Engage` 动作按下时锁定指针。
    它同时写 `WorldTransform`，这样视图在输入到达的这一帧就跟上，不必等下一个 Fixed 步的传播，所以只作用于没有 `Parent` 的实体。
  - `RenderMeshExtractSystem`（`LatePhase`，`PresentationSet`）：先串行回收实体已经没了或不再渲染的槽位（每帧扫一遍槽位表），
    再串行给新实体分配槽位（`CommandBuffer` 加 `RenderInstance`），最后按 chunk 并行处理 `WorldTransform` 或 `RenderInstance` 列改过的 chunk。
    `RenderInstance` 被复制到别的实体上（槽位表里登记的不是它）时给副本重新分配槽位。
  - `RenderScene`（WorldSubsystem，`Sets = (PresentationSet)`）：CPU 镜像加更新列表；Extract 交给渲染 job，渲染 job 用 scatter compute 写进常驻实例缓冲。
    实例缓冲按 2 倍增长（至少 1024），重建时整份上传。scatter shader 还没加载好时更新留到下一帧，否则不再变化的场景永远上传不了。实例 64 字节：tile（int3）、mesh、旋转四元数、tile 内偏移、缩放；tile 边长 1024 m。
  - `SceneRendererSubsystem`（EngineSubsystem）：几何池（`Engine.SceneRenderer.GeometryPoolSize` 默认 256 MiB、`MaxClusters` 默认 2^20、
    `MaxMeshes` 默认 2^16，各自首次适配的区间分配）。Mesh 第一次被引用时等资产加载完，用 `BulkStreamer` 一次读出所有页，页原样放进几何缓冲
    （每页后留 16 字节，三角形索引按字读不越界），cluster 表记录每个 cluster 的顶点和索引的字节偏移；没有引用的 Mesh 退役 3 帧后区间才复用。
    还负责 view 阶段排序、`SceneViewFeature` 和 `SceneViewMode` 的注册表，以及每帧的上传（自己的 `RhiUploadAllocator`，图里的复制 pass）。
  - View 阶段锚点：`SceneCullingStage`、`SceneVisBufferStage`、`SceneMaterialStage`、`SceneLightingStage`、`SceneTranslucencyStage`、`ScenePostStage`。
    feature：`SceneCullingFeature`（Culling）、`SceneVisBufferFeature`（VisBuffer）、
    `SceneVisualizeFeature`（Post：从 VisBuffer 写 RGBA16F 的 scene color，按 UI 值域绑到图像槽）。
  - View 模式：`SceneUnlitViewMode`（默认，`Engine.SceneRenderer.DefaultViewMode`；按三角形法线、相机处的光源着色）和 `Buffer/Instances`、
    `Buffer/Clusters`、`Buffer/Triangles`、`Buffer/Depth`、`Buffer/Normals`，以及仿 Nanite 可视化的
    `Buffer/LOD`（遍历选中的 DAG 层级，按 vk_lod_clusters 的色带：白色是原始三角形，到红色是最粗一级；`SceneGpuGroup` 为此带上 `Depth`，
    cluster 范围压进一个字）、`Buffer/Raster`（绿色软光栅、红色硬件光栅，用剔除时同一个判断）、`Buffer/Groups`、`Buffer/Pages`（流送页，
    即页池槽位）、`Buffer/Overdraw`（每像素光栅化的片元数，被遮住的也算：蓝色 1 到红色 32 以上；只有这个模式下两条光栅化路径才对
    每像素计数缓冲做原子加），都派生自 `SceneVisBufferViewMode`。
  - 显示模式不只在编辑器里：Game 的视图没设模式时用默认模式，控制台命令 `r.Scene.ViewMode <名字>`（显示名如 `Buffer/LOD` 或类型名，
    不带参数时列出所有模式）在运行时改默认模式，Shipping 下什么都不做。编辑器的显示模式菜单（从注册表生成）尚未实现。
  - 投影：reversed-Z、无限远平面；view 数据是相机的右、上、前三个方向（w 放投影缩放和近平面）加 tile 拆分的位置，不用矩阵。
  - `Runtime/GameViewport`（只有 Game 链接）：`GameViewportSubsystem` 给第一个有 RenderScene 的 World 建一个 SceneView，
    每帧开头在主窗口全屏 `DrawImage`（游戏 UI 画在上面），`GameCameraSystem` 把第一个 `Camera` 实体拷进 view；没有相机时看向原点。
  - `Samples/Sandbox` 有一个 `Content/Scene/Shapes.gltf`（地面、球、环、方块，导入成四个 MeshSource）和启动关卡 `Content/Levels/Sandbox.masset`。
  - **GPU 剔除**（两阶段，Nanite 的做法）：
    - 第一阶段 `Scene Cull Clusters`：每个实例一个 group，先用 Mesh 的包围球（`SceneGpuMesh::Sphere`）测视锥；再逐个测最精细一层的 cluster：
      视锥、背面锥（meshoptimizer 的锥加包围球测试；镜像或非均匀缩放的实例不做）、上一帧的 HZB（用上一帧的 view 投影，`SceneViewHistory`）。
      过了的进可见列表，只被 HZB 挡住的进候选列表。计数缓冲是 4 个 uint：可见数、候选数、第一阶段的可见数。
    - 第一次光栅（`Phase = 0`）后用 VisBuffer 建这一帧的 HZB，`Scene Cull Candidates` 用它重测候选（间接 dispatch），没被挡住的接在可见列表后面，
      第二次光栅（`Phase = 1`）只画这部分。可见 cluster 的编号在两个阶段里都是列表下标，VisBuffer 的编码不变。最后再从完整的 VisBuffer 建一次 HZB，留给下一帧。
    - HZB：每个 view 一张 R32F（`SceneViewState` 里，view 改尺寸就重建），mip 0 是 view 一半大小向上取到 2 的幂，每个 texel 存下面 2×2 的最远深度
      （reversed-Z 取最小），view 外算空（深度 0，挡不住任何东西）；每级 mip 一个 compute pass。
      遮挡测试：包围球的视空间包围盒投影成保守的屏幕矩形，选能用 2×2 texel 盖住它的 mip，球最近点的深度比这些 texel 的最远深度还远就算被挡住。
      第一帧或 view 刚改尺寸时 HZB 无效，第一阶段不做 HZB 测试，也就没有第二阶段。
    - 验证：Sandbox 里 837 个 cluster，背面锥和视锥剔掉约 13%（剩 732）；临时在相机前加一面墙时，墙后 625 个 cluster 被上一帧的 HZB 挡住，
      第二阶段一个也没放回来。
    - 有 LOD 遍历以后，两阶段也覆盖实例和遍历（Nanite 的做法）：`Scene Cull Instances` 和第一阶段的 `Scene Traverse` 用上一帧的 HZB 测实例的
      Mesh 包围球和节点的包围球（节点球是子树几何的合并包围球，保守），被挡住的不往下走、不请求页，记进 `Scene Occluded Nodes`；
      第一次光栅建好这一帧的 HZB 后，`Scene Traverse (Second Phase)` 从这些节点接着遍历（还挡着的丢掉），列出的组由
      `Scene Cull Groups (Second Phase)` 用同一张 HZB 剔除后直接接进可见列表，和候选一起进第二次光栅。列表满了就当作可见在第一阶段走完，不会出洞。
    - 软光栅三角形包围盒宽于 4 像素时逐行只扫三条边之间的区间（Nanite 的扫描线路径），区间外扩一像素，区间内照常逐像素精确测试。
  - 默认值和 Nanite 对齐：`LodPixelError` 1 px（`r.Nanite.MaxPixelsPerEdge`）、`MaxVisibleClusters` 4M、`StreamingPoolSize` 512 MiB 加
    `RootPages` 2048 个常驻根页槽、每帧最多 128 个加载和 128 个提交。页池过载自动降质量（`r.Nanite.Streaming.QualityScale.*`）：
    一帧用到和请求的流送页超过池的 85% 连续 2 帧，LOD 阈值每帧放大 3%（质量最低 0.3）；低于 70% 连续 30 帧，每帧恢复 1%。
    Zorah 默认视角下质量稳定在约 0.6（池需求约 80%），约 7 ms/帧，不闪。
  - 测试：`SceneRenderer.PassShadersCompileAndMatchTheirArguments`（9 个入口点编译成合法 SPIR-V，根常量和 C++ 参数一致）、
    `PassPipelinesCompileOnTheDevice`、`ScatterWritesWholeInstances`（GPU 上核对 scatter 写出的字节）、tile 拆分、区间分配。
- 尚未实现：cluster LOD 和页流送（几何现在整份常驻，只画最精细一层）、灯光和阴影、
  多 view 共享剔除前数据、实例缓冲按需缩小、HZB 的单 pass 下采样。

## 6. Scalability

`Config/Scalability.mtext` 定义各档（Low…Cinematic）对应的 CVar 组合；平台层可以覆盖。主机通常固定一档，再加上动态分辨率。

当前进度：尚未实现（还没有 `Config/Scalability.mtext`）。
