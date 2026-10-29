# 12 · 平台层与 ThirdParty

## 1. 平台层栈

每个目标平台是一组有序的层，每层对应 `Platforms/` 下的一个目录：

| 目标平台 | 层栈（优先级从高到低） |
| --- | --- |
| Windows | Windows → Microsoft → X64 → Generic |
| Xbox | Xbox → Microsoft → X64 → Generic |
| PS5 | PS5 → X64 → Generic |
| Switch2 | Switch2 → Arm64 → Generic |
| Linux | Linux → Posix → X64 → Generic |
| macOS | macOS → Apple → Posix → Arm64 → Generic |

**决策**：架构层跟随构建的 arch，而不是平台：层栈 = 平台自己的层 + 当前 arch 的层 + Generic。
所以 Windows Arm64 的层栈是 Windows → Microsoft → Arm64 → Generic。层名与 toolkit 的 `Platform` 枚举一致（`macOS`），
层栈表在 `marlin/build/platforms.py`（已实现）。

### 1.1 每层的内容

```
Platforms/<Layer>/
  Source/<Category>/<Module>/      叠加进同名模块的源文件（Category = Runtime | Developer | Editor | Programs）
  Source/<Category>/<NewModule>/   只在该平台存在的模块（例如 RHID3D12 在 Microsoft 层，RHIMetal 在 Apple 层）
  Source/ThirdParty/<Lib>/         平台专属的预编译库，或已有库的平台二进制
  Config/*.mtext                   按层栈合并进 /Engine/Config（Generic 层优先级最低）
  Language/{Common,Shaders}/       平台 intrinsic、wave 大小、线程组参数（.mcommon / .mshader）
  Build/                           工具链描述（toolchain.py）、SDK 探测、打包器、devkit 部署和运行脚本
  Content/                         平台专属内容（手柄按键图标等）
```

层目录本身就是层的标识，不需要描述文件；`Platforms/` 下的目录名必须是层栈表里出现过的层名，否则 `marlin generate` 报错。
运行时真正需要层的元数据时，再加 `Layer.mtext`。

### 1.2 源码叠加规则

以下规则已在 toolkit 中实现：

- 模块 `Core` 的源文件 = `Source/Runtime/Core/**` 加上层栈中每一层的 `Platforms/<L>/Source/Runtime/Core/**`（overlay）。
  `Source/` 的优先级低于所有层；定义在某层里的模块（如 Microsoft 层的 `RHID3D12`）只接受比它更具体的层的 overlay。
- overlay 的目录结构和模块相同，但没有 rules 文件；overlay 里的 `Tests/` 叠加进测试模块，被测模块自己必须有 `Tests/`。
- 头文件选择：每层的 `Public/`、`Internal/`、`Private/` 按层栈优先级**从高到低**加入 include 路径，所以 `#include "HAL/PlatformArch.h"`
  会先命中最具体那一层的实现（例如 X64 层的 `HAL/PlatformArch.h`），Generic 层提供兜底实现或者用 `#error` 报错。
  带引号的 include 会先在当前文件所在目录查找，所以头文件之间要写从 include 根开始的完整路径，不写同目录的短路径。
- 同一个 `.cpp` 相对路径在多层中都存在时，只编译优先级最高的那一份（覆盖），以免出现重复定义。
- 主机层的目录可以不在主仓库里：作为 Lore 的独立子仓库，或者只发给有授权的成员；某个平台缺少层栈中的任何一层时，toolkit 把它标记为不可用。
  定义在某层的模块被不使用该层的平台依赖时，对应的构建组合被排除。
- 现有的「模块内按平台名称排除目录」规则继续保留，用于少量不值得单独拆层的代码。
- `Public/`、`Internal/`、`Private/` 区分的是头文件的可见性，不是文件类型：实现某个公开头文件的 `.cpp` 和头文件放在一起，
  `Private/` 只放私有头文件和实现它们的 `.cpp`。层里的 `.cpp` 也一样，跟随它实现的那个头文件所在的可见性目录。
- **决策**：可见性目录下面再分一层，include 路径就是真实目录，不做自动加前缀（header map、转发头、junction 都会让编辑器和构建看到不同的路径）。
  小模块用模块名作为这一层：`Vfs/Public/Vfs/Vfs.h` → `#include "Vfs/Vfs.h"`；Core 这样的大模块按类别分目录：`Containers/List.h`、`HAL/PlatformFile.h`。
  平台层的 overlay 用同样的子目录（`Platforms/Windows/Source/Runtime/Application/Public/Application/WindowsWindow.cpp`）。
  这样不同模块的同名头文件不会冲突，从 include 也能看出头文件属于哪个模块。

### 1.3 平台 HAL

**决策**：平台抽象分在两个模块里，靠链接关系裁剪，不用宏。

| 模块 | 内容 | 谁链接 |
| --- | --- | --- |
| `Core` 的 `HAL/Platform*.h` | 虚拟内存、时钟和 UTC 时间、线程和 TLS、Mutex / ReadWriteLock / Event / Semaphore、fiber、CPU 拓扑和 ISA 检查、文件和目录、内存映射、异步 IO、目录监控、子进程、管道、共享内存、动态库、socket、完成端口（`PlatformIoPort`，14 §2）、调用栈与符号、崩溃回调和 minidump、已知目录、环境变量、系统信息、OS 安全随机数 | 所有 Target |
| 架构层的 `HAL/PlatformArch.h` / `HAL/PlatformSimd.h` | 自旋提示、周期计数器；clang 内建函数覆盖不到的 SIMD 指令（近似倒数、字节 movemask），Generic 层给出可移植实现 | 所有 Target |
| 平台层的 `HAL/PlatformHeapConfig.h` | MarlinHeap 的 arena 大小、segment / 页大小、保留量与归还延迟（03 §2.1） | 所有 Target |
| `Application` | 窗口、消息泵与事件队列、键盘（按物理键位）、文本输入和 IME、鼠标和 raw 相对移动、手柄、DPI 和多显示器、光标、剪贴板、文件拖放、消息框 | Game、Editor；Server 和 Program 不链接 |

- 接口头文件放在模块的 `Source/` 里，只有声明；实现放在各层的 overlay 中。
  Windows 上的分工：Microsoft 层放 Windows 和 Xbox 共用的 Win32 部分（内存、时间、线程、同步、fiber、文件、动态库、socket、调试输出），
  Windows 层放桌面专属部分（进程和 IPC、异步 IO、目录监控、崩溃与符号、系统信息、CPU 拓扑、窗口和输入），X64 层放 cpuid、`pause` 等指令。
- 字符串一律 UTF-8；返回的路径用 `/` 分隔，传入时两种分隔符都接受；超过 MAX_PATH 的路径自动加 `\\?\` 前缀。
  平台层还没有分配器，填充字符串的函数由调用方提供缓冲区：返回完整长度，放得下才写入。
- Win32 头文件只通过 Microsoft 层 `Internal/HAL/MicrosoftHeaders.h` 引入：它定义 `UNICODE`、`WIN32_LEAN_AND_MEAN`、`NOMINMAX`，
  `#undef` 和引擎函数重名的宏（`CreateDirectory`、`GetEnvironmentVariable`、`Yield` 等），并提供 UTF-8 / UTF-16 转换。
  放在 `Internal/` 是为了让其他模块（如 `Application`）的 Windows overlay 也能用，而项目看不到。
- 异步 IO：Windows 11 22H2 及以上用 IoRing，更早的系统回退到 overlapped IO + IOCP；DirectStorage 需要 D3D12 设备，做 RHI 时接入。
- Windows 上的 fiber 用 OS fiber（`CreateFiberEx`），保证调试器、SEH 和栈探测正常。
- `Platform::Initialize` 在启动最早期检查 CPU 是否满足最低 ISA（03 §6），不满足时提示并退出，而不是在第一条 AVX2 指令处崩溃。
- 窗口和消息泵在**窗口线程**上，不在引擎主线程上（03 §5.2），否则拖动和缩放窗口时 Windows 的模态循环会卡住主循环；见 §1.5。
- 崩溃回调同时接管 SEH 异常和 CRT 层面的失败（`abort`、纯虚调用、非法参数），后者以 `PlatformCrash::FailureExceptionCode` 抛出。

### 1.4 主机上的崩溃报告

**决策**：主机上没有 `MarlinCrashReporter`。主机不允许游戏再拉起一个进程，系统自己会在崩溃时抓 core dump 并走平台方的上报渠道，
所以 03 §7.1 的进程外部分换成平台服务，进程内部分照旧：

- `CrashReport` 在主机上仍然安装崩溃回调、记录失败信息、镜像日志；`Start` 不拉起 Reporter，也不创建共享内存。
  崩溃回调把失败信息、日志尾部和附件路径交给平台层，由平台层写进系统 dump 的用户数据区（各平台 SDK 都提供这类附加数据接口），
  然后让系统接管崩溃，不在进程内写 minidump。
- dump 的收集、去重和下载走平台方的后台（PS5、Xbox、Switch 2 各自的崩溃报告服务），不上传到 `MarlinCrashServer`；
  `CrashAnalysis` 只认 Windows x64 的 minidump，主机 dump 用平台工具分析。
- Hang 检测由平台的看门狗负责；心跳仍然每帧调用，平台层可以用它补充信息。
- 这些实现放在各平台层的 `Platforms/<Console>/Source/Runtime/CrashReport/` overlay 里，随平台 SDK 一起，不进主仓库。

### 1.5 桌面平台的差异（Editor 跨平台）

**决策**：Editor 支持 Windows、macOS、Linux。差异只出现在两处：`Application`（和 Core HAL）的平台 overlay，以及按 `@if Platform` 分层的 Config；
Editor 模块里不写 `MARLIN_PLATFORM_*` 分支，平台不同的行为都通过 `Application` 的查询接口拿到。目前只实现了 Windows。

- **线程**：引擎主线程（主循环）和窗口线程（OS 窗口、消息泵、Splash 窗口）是两个概念，引擎代码不能假设主循环跑在进程的主线程上。
  - Windows、Linux：窗口线程是 `Application` 启动的专用线程（Windows 已实现，`WindowsApplication.cpp`）；进程主线程就是引擎主线程。
  - macOS：AppKit 要求窗口和 runloop 在进程主线程上，所以反过来：macOS 层的 `LaunchMain` 在第 0 步就另起一个线程跑引擎（第 1 步起的全部流程），
    进程主线程进入 `NSApplication` 的 runloop，当窗口线程。
  - `PlatformWindow`、`PlatformSplash`、剪贴板、消息框都是任何线程可调用，平台层转到窗口线程执行（现在的 `PlatformWindow` 已经这样约定）。
  - 窗口线程在第一次用到时启动（`PlatformSplash::Show` 或 `PlatformApplication::Initialize`，谁先谁启动），这样 Splash 在第 4 步就能显示，
    不用等 `ApplicationSubsystem`（09 §4.6）。macOS 上它从进程启动起就在。
- **窗口按钮**（`CustomFrame` 窗口）：`PlatformWindow::GetFrameControls(window)` 返回 `WindowFrameControls`：按钮是否由系统画（`SystemDrawn`）、
  在左还是右（`Side`）、有哪些按钮和顺序、要给它们留出的区域（客户区像素）。Editor 按它决定自己画按钮还是只留空（09 §4.5）。
  - Windows：应用画，右边，最小化、最大化 / 还原、关闭。
  - macOS：系统画红绿灯（`NSWindow` 用 full-size content view、透明标题栏），左边；应用只留出位置，标题栏的其余部分照样是 `Caption`。
    「最大化」对应 zoom，全屏是 macOS 的原生全屏空间。
  - Linux：Wayland 上客户端自己画装饰，应用画；按钮的位置和顺序取桌面设置（GNOME / KDE 的 button layout，经 xdg-desktop-portal 读取），
    取不到时右边、最小化 / 最大化 / 关闭。X11 同样用自绘标题栏。
  - `Caption` 区域的双击和右键行为由平台决定：Windows 双击最大化、右键系统菜单；macOS 按系统设置双击 zoom 或最小化，没有系统菜单；
    Linux 跟随窗口管理器。
- **修饰键和快捷键**：快捷键写成 `KeyChord`（按键加修饰键），修饰键里有一个逻辑上的 **`Primary`**：Windows、Linux 上是 Ctrl，macOS 上是 Command（`ModifierKeys::Super`）。
  - 文本框的复制、粘贴、全选、撤销等都用 `Primary`，不直接判断 `Control`。
  - 菜单里的快捷键提示由 `PlatformInput::FormatChord` 生成：Windows、Linux 是 `Ctrl+S`，macOS 是 `⌘S`。不再写死字符串。
  - 和平台惯例绑定的命令（退出：Windows `Alt+F4`、macOS `⌘Q`、Linux `Ctrl+Q`；偏好设置等）的默认快捷键放在 Config 里按 `@if Platform` 分层。
- **可执行文件名和路径**：
  - 拉起子进程时用 `PlatformProcess::ExecutableSuffix`（Windows `.exe`，其他平台空），不在调用处写 `MARLIN_PLATFORM_WINDOWS ? ".exe" : ""`。
  - 路径比较和哈希（项目根的管道名、EngineId、`AssetJournal`）是否忽略大小写，由 `PlatformFile::IsCaseSensitive(path)` 按卷查询，
    不按平台猜：macOS 的 APFS 默认不区分大小写，Linux 区分。
- **渲染**：Linux 用 `RHIVulkan`，surface 创建放在 Linux 层的 overlay（Windows 的在 `VulkanPlatform.cpp`）；macOS 用 Apple 层的 `RHIMetal`（07 §1）。
- **Splash**：`PlatformSplash` 在每个桌面平台层各实现一份：Windows 用 GDI，macOS 用 AppKit（`NSWindow` 加 Core Graphics），Linux 用和主窗口相同的窗口后端加软件绘制。
- **菜单栏**：**决策**：平台有全局菜单栏时用它，macOS 上把 `EditorMenuSubsystem` 的菜单映射成 `NSMenu`，窗口里不画菜单栏；
  Windows、Linux 在窗口里自绘（Linux 桌面的全局菜单不接）。
  - 接口在 `Application`：`PlatformMenu::HasGlobalMenuBar()`，`PlatformMenu::SetMenuBar(model)`。model 是一份快照：菜单和菜单项的 Id、文字、
    `KeyChord`、是否可用、是否勾选、分隔线、子菜单；没有回调。
  - 线程：`NSMenu` 在窗口线程上，菜单项的状态（Signal、可用条件）在引擎主线程上。所以 `EditorMenuSubsystem` 在菜单内容或状态变化时
    发一份新快照，窗口线程只用快照；选中菜单项变成事件队列里的 `MenuItemActivated { Id }`，下一帧在主线程上执行，和其他输入一样。
  - 快捷键：菜单项的 `KeyChord` 设成 `NSMenuItem` 的 key equivalent，系统显示成 `⌘S` 并负责触发；AppKit 吃掉这次按键，
    应用收不到对应的 KeyDown，所以同一个快捷键不会执行两次。
  - 应用菜单（第一个菜单，以应用名命名）由 macOS 层补上系统要求的项：About、Settings…（`⌘,`）、Services、Hide / Hide Others / Show All、
    Quit（`⌘Q`，走 `RequestClose`，和关闭主窗口一样会询问未保存内容）；编辑器的 File、Edit… 菜单排在它后面。Edit 菜单里的标准项
    （Undo、Copy、Paste…）是编辑器自己的命令，由当前焦点（文本框或面板）处理，不走 AppKit 的 responder 链（编辑器的控件不是 `NSView`）。
  - 自绘菜单栏右端的构建号在 macOS 上改放在标题栏右端（红绿灯在左边，右边是空的，09 §5）。
- 尚未实现：除 Windows 外的全部；`GetFrameControls`、`Primary`、`FormatChord`、`ExecutableSuffix`、`IsCaseSensitive` 在 Windows 上也还没有，
  现在的代码按 Windows 写死（`EditorShellSubsystem::BuildWindowButtons`、`UIControls.cpp` 的 Ctrl+V、菜单里的 `"Alt+F4"`、5 处 `.exe`、3 处大小写判断）。

## 2. ThirdParty 格式

```
Source/ThirdParty/<Lib>/
  <Lib>.module.py          ThirdPartyRules：include 路径、add_static_library、add_shared_library，以及必填的 version、url、license（SPDX）
  LICENSE*                 原始许可证文本（打包时汇总到 Licenses.txt）
  Include/                 公开头文件（header-only 的库只有这一部分）
  Lib/<Platform>/<Arch>/[<Variant>/]   静态库或导入库；Variant 取 Debug | Release，默认 Release，只有 ABI 不兼容的库才分开
  Bin/<Platform>/<Arch>/   动态库（运行时复制到 Binaries 旁边）
  Build/                   可以复现二进制的构建脚本（不在构建时运行，只在升级版本时手动执行）
```

- 版本、来源和许可证只写在 `<Lib>.module.py` 里（toolkit 在发现阶段校验），**不写进目录名**。
- `Extras/<Tool>/` 中预编译的工具用同样的规则：`<Tool>.tool.py`（ToolRules，同样必填 version、url、license）、上游的 LICENSE 文本，
  程序放在 `Extras/<Tool>/<Platform>/<Arch>/`，toolkit 按宿主平台找到对应的程序。
- 主机平台的二进制放在 `Platforms/<P>/Source/ThirdParty/<Lib>/Lib/...`，由 toolkit 合并到同名库的规则中。
- 第三方库必须能接入引擎的分配器（提供 malloc 回调），否则只能在 Program 专属模块中使用。
- 第三方代码可以使用 std 和异常：它们是预编译的；header-only 的库只允许出现在 Program 专属模块或 `.cpp` 中，并且不能违反 Runtime 的头文件政策。

**初始清单（草案）**：HarfBuzz、ACL（动画压缩）、Oodle 或 zstd、xxHash、Tracy client、DirectStorage、Agility SDK、Vulkan headers、
FLIP（测试用）、libclang（只给 Program 使用：仓库里只放头文件，库本身从 toolkit 找到的 LLVM 中运行时加载，16 §3.1）、md4c（MarlinDoc 用）。物理和音频混音器自研。

## 3. 扩展模块（没有 Plugin 层）

**决策**（10/28）：引擎不设 Plugin 层，原来的 `Plugins/<Name>/`、`*.plugin.py`、`[plugins] enabled`、`plugin_modules`、`/Plugins` 挂载
和 `.mproject` 的 `RequiredPlugins` 都已移除。理由：Plugin 本来是为工作室 fork 减少合并冲突准备的，但当时的七个插件都是引擎自己的
codec、编码器和解码器，多一层间接只让「Game 到底链接了什么」更难看清；按平台挑选实现，在 target 规则里写条件就够了。

- 扩展就是普通模块：按种类放在 `Source/Runtime/`（Game 也要用，如 `Lz4Codec`、`ZstdCodec`）或 `Source/Developer/`（只进 Editor 和工具，
  不进 Shipping，如 `AstcTextureEncoder`、`Bc7EncRdoEncoder`、`IspcTextureEncoder`、`FbxSceneDecoder`、`GltfSceneDecoder`），
  它们包装的库放在 `Source/ThirdParty/`。
- 实现通过扩展点被发现（13），不写注册代码；**链接哪些由 target 规则决定**：`Game`、`Editor`、`MarlinConverter`、`MarlinPackager`
  在 `extra_modules` 里列出要用的 codec、编码器和解码器，平台相关的选择写成 `ctx.platform` 条件。
- 工作室 fork 的扩展也是普通模块，放在同样的目录里；为了少和主线冲突，fork 的模块和 target 规则里的改动尽量集中在自己的文件中。
  真的出现需要单独分发、或者大量 fork 各自维护扩展的情况，再按当时的需要设计插件层。
