# 02 · 路径、引擎绑定、产品身份与 VFS

引擎代码**只使用虚拟路径**。物理路径由 `Launch` 在启动时根据「运行形态」决定，然后挂载进来。

## 1. 三种运行形态

| 形态 | 判定方式 | 典型场景 |
| --- | --- | --- |
| **Source** | 没有下面两种标记 | 引擎程序员、自编译编辑器的工作室 |
| **Installed** | `<EngineRoot>/Build/InstalledBuild` 存在（空的标记文件，由 `marlin package-engine` 写入） | 美术、策划用的安装版编辑器 |
| **Packaged** | 可执行文件同目录下有 `Product.mtext` | 玩家机器、主机包 |

Source 和 Installed 两种形态的 `Binaries/<Platform>/<Arch>/<Config>/` 深度相同，所以 EngineRoot 的推导方式一致。

**决策**：引擎根目录下没有描述文件（早期设计里的 `Engine.mtext` 已去掉）。形态和 Id 都能从目录本身推出来，引擎版本编进二进制（`MARLIN_VERSION_MAJOR/MINOR`），
不需要一个会和二进制不同步的文件。和 UE 的 `InstalledBuild.txt` 一样，只用一个标记文件区分安装版。

## 2. 目录布局

### 2.1 源码引擎

```
<EngineRoot>/
  Binaries/<Platform>/<Arch>/<Config>/   Editor, Game, Server, Marlin*.exe, 模块 DLL
  Build/  Extras/  Documentation/
  Config/                 引擎 Config（*.mtext）
  Content/                引擎内容（默认材质、编辑器图标、字体……）  ← 新增目录
  Language/               引擎语言代码：Common/ .mcommon、Shaders/ .mshader、Scripts/ .mscript（内部结构见 Languages/MarlinLanguage.md §1.5）
  Platforms/<Layer>/      平台层（12）
  Samples/<Name>/         随引擎提供的示例项目（结构同 §2.3）；Sandbox 是从 IDE 启动 Editor、Game 时打开的空项目
  Source/{Runtime,Developer,Editor,Programs,ThirdParty}/
  Intermediate/  Saved/   本地产物（不纳入版本管理）
```

从 Visual Studio 启动时，Editor、Game、Server（不含测试程序）的调试参数是 `-Project=<Build 设置 solution.debug_project>`，
默认是 `Samples/Sandbox/Sandbox.mproject`；示例项目在引擎目录里，所以 `.mproject` 不写 Engine 绑定（§3）。

### 2.2 安装版引擎

```
<InstallRoot>/Marlin/<Version>/
  Build/InstalledBuild    标记文件；Id = 版本号
  Binaries/<Platform>/<Arch>/Development/   Editor, Programs, Game(dev)
  Binaries/<Platform>/<Arch>/Shipping/      Game、Server 模板（Packager 从这里复制）
  Config/  Content/  Language/  Platforms/<Layer>/{Config,Language}
```

安装目录是**只读**的。

### 2.3 游戏项目

```
<ProjectRoot>/
  Foo.mproject
  Config/                 项目 Config，其中 Product.mtext 描述产品身份
  Content/                .masset 和并列的二进制源文件（png/fbx/wav……）
  Language/               项目语言代码：Common/ .mcommon、Shaders/ .mshader（材质、自定义 BSDF）、Scripts/ .mscript
  Platforms/<Layer>/Config/  项目的平台覆盖
  Intermediate/           Indexer 数据库、编译后的脚本缓存（不纳入版本管理）
  Saved/                  日志、自动保存、编辑器布局、User/（不纳入版本管理）
```

### 2.4 打包后的游戏

```
<InstallDir>/
  <ProductName>.exe       由 Game 改名，并改写 PE 资源（图标、版本、公司）
  Product.mtext           身份 + Chunk 清单 + StartupLevel
  Paks/Base.mpak  Paks/Lang_zh-Hans.mpak  Paks/DLC_Foo.mpak  Paks/Patch_0001.mpak
  <第三方动态库>
```

主机平台的包格式（PS5 pkg、MSIXVC、NSP）由 `Platforms/<P>/Build/` 里的平台打包器负责把上面这些内容装进去。

## 3. 引擎绑定

**决策**：版本号或 Id 加用户级注册表。

- `.mproject` 里写 `Engine = "0.1"` 或 `Engine = "Source-1a2b3c4d"`。
- Source 形态的 Id 是 `Source-` 加 EngineRoot 绝对路径（`/` 分隔、去掉末尾分隔符，Windows 上先转小写）的 32 位 FNV-1a，8 位小写十六进制。
  toolkit（`paths.engine_id`）和运行时（`Engine::ComputeSourceEngineId`）用同一个算法，`EngineTests` 检查两边一致。
- 注册表位置：Windows 是 `HKCU\Software\Marlin\Engines\<Id> = <EngineRoot>`；Linux 是 `$XDG_CONFIG_HOME/Marlin/Engines.mtext`；
  macOS 是 `~/Library/Application Support/Marlin/Engines.mtext`。
- 写入时机：Installed 形态由安装器写；Source 形态由 `marlin generate` 写（已实现，Windows），并打印 `Engine Source-xxxxxxxx`。所以源码引擎和安装版引擎可以共存。
  键被 ACL 锁定或机器策略拒绝写入时只告警、不中断生成；受影响的工程用 `-Project=` 打开，修好权限后再生成即可注册。
- 打开 `.mproject` 的入口是 `MarlinLauncher`（一个很小的 Program，注册为 `.mproject` 文件关联）。它解析 Engine 字段，
  找到对应的 Editor 后用 `-Project` 启动；找不到时让用户选一个引擎，并把选择写回 `Foo.mproject.local`（不纳入版本管理）。

## 4. 产品身份

`<Project>/Config/Product.mtext`：

```
!Version "1.0"
Product
{
    !UUID        = "…"
    Company      = "Acme"
    ProductName  = "Starfall"          // 文件系统安全名，用于 exe 名与用户目录
    DisplayName  = <LocalizedText:/Project/Content/Loc/Product#…>
    Version      = "1.2.0"
    Icon         = `source:/Project/Content/Branding/Icon.png#GUID`
    StartupLevel = </Project/Content/Levels/Frontend:Frontend#UUID>
    Platforms { Windows { SaveRoot = SavedGames } Mac { BundleId = "com.acme.starfall" } }
}
```

打包时：`Game` 被复制成 `<ProductName>.exe` → 改写 PE/Info.plist 资源 → `Product.mtext` 精简后放到 exe 旁边。
运行时**只看 `Product.mtext`**，不看 exe 名。

## 5. VFS

### 5.1 虚拟根

| 根 | 读写 | 内容 |
| --- | --- | --- |
| `/Engine` | 只读（Source 形态下编辑器可写） | 引擎的 Content / Config / Shaders，以及平台层叠加 |
| `/Project` | 只读（编辑器可写） | 项目的 Content / Config / Scripts / Shaders |
| `/User` | 可写 | 项目级用户数据：Config 覆盖、SaveGames、Logs、Crashes；`/User/Global/` 是引擎级用户数据（编辑器偏好、最近项目） |
| `/Cache` | 可写 | 本机缓存（RHI pipeline cache 等；转换结果不在这里，在 GlobalStore，06 §3.3） |
| `/Temp` | 可写 | 进程临时文件 |

### 5.2 各形态的物理映射

| 根 | Source 编辑器 / 开发版 Game | Installed 编辑器 / 开发版 Game | Packaged |
| --- | --- | --- | --- |
| `/Engine` | `<EngineRoot>` + 平台层叠加 | `<InstallRoot>` + 平台层叠加 | mpak 层栈 |
| `/Project` | `<ProjectRoot>` | `<ProjectRoot>` | mpak 层栈 |
| `/User` | `<ProjectRoot>/Saved/User` | `<ProjectRoot>/Saved/User` | Windows：`%LOCALAPPDATA%/<Company>/<Product>`（SaveGames 默认放在 `Saved Games/<Company>/<Product>`）；Linux：XDG；Mac：`~/Library/Application Support/<BundleId>`；主机：平台存档 API |
| `/User/Global` | `<EngineRoot>/Saved/User` | `%LOCALAPPDATA%/Marlin/<EngineId>` | 不挂载 |
| `/Cache` | `%LOCALAPPDATA%/Marlin/Cache`（所有引擎共享） | 同左 | 不挂载 |
| `/Temp` | OS temp/Marlin/<pid> | 同左 | 同左 |

- **开发期和产品身份隔离**：开发期运行时，`/User` 一律指向项目的 `Saved/User`，不会污染玩家真实安装的数据；
  用 `-UserDir=` 可以指向真实路径来复现玩家问题。
- **主机 devkit**：`/Engine`、`/Project` 通过 `MarlinFileServer` 走网络映射到开发 PC，转换结果直接从 GlobalStore 读（06 §4.2），`/User`、`/Cache` 在 devkit 本地。
- **叠加**：同一个根可以挂多层，按优先级依次查找，写操作只落到最上面的可写层。平台层（Windows → Microsoft → X64 → Generic）
  就是作为 `/Engine` 的下层叠加进来的。Patch / DLC 的 mpak 同理。

### 5.3 API

```cpp
MountId id = Vfs::Mount("/Project", MakeUnique<DirectoryMount>(projectRoot, /*writable*/ true), /*priority*/ 0);
FileHandle file = Vfs::Open("/Project/Config/Game.mtext", FileAccess::Read);
List<uint8> bytes;
Vfs::ReadAll("/Project/Config/Game.mtext", bytes);
AsyncReadRequest request = Vfs::ReadAsync(path, IoPriority::Streaming, offset, size, onDone);
```

`IFileMount` 的实现有：`DirectoryMount`、`PakMount`、`NetworkMount`（FileServer）。开发版 Game 按对象 UUID 取转换结果不经过 mount，而是资产表的 `GlobalStoreAssetStore`（06 §4.2）。
异步 I/O 由平台层实现（`PlatformAsyncIo`，12 §1.3）：Windows 用 IoRing，旧系统回退到 IOCP，送往 GPU 的数据走 DirectStorage；Linux 用 io_uring，主机用各自的 API。

### 5.4 挂载语义

**决策**：

- **挂载点归属**：包含路径的**最长**挂载点拥有它，和 Unix 一样。`/User/Global` 挂上之后，`/User/Global/...` 不会再落到 `/User` 的挂载上，
  读写都是如此；所以只读的内层挂载不会把写操作漏到外层的可写挂载。
- **同一挂载点的层**：一个挂载点可以挂多个 mount，这就是责任链。读取从优先级最高的开始，取第一个有这个文件的；写入落到优先级最高的可写 mount。
  `Vfs::Enumerate` 合并所有层，每个条目只出现一次（取赢的那一层）；`Vfs::ReadEachLayer` 从低到高访问每一层的同一个文件，Config 层叠就靠它。
- **平台层**：`/Engine`、`/Project` 的根目录挂优先级 0，`Platforms/<Layer>/` 按 `MARLIN_PLATFORM_LAYERS`
  （toolkit 定义，最具体的层在前，例如 `Windows;Microsoft;X64;Generic`）依次挂在更高的优先级上。
- **路径**：只接受以 `/` 开头的虚拟路径；`\` 转成 `/`，合并重复的 `/`，去掉 `.`，出现 `..` 直接拒绝。
- 横切需求（加密、统计、按 Chunk 限制访问）写成包装另一个 `IFileMount` 的装饰器，不往 VFS 里加开关。

### 5.5 异步读取

**决策**：mount 只负责把路径**翻译**成 `FileRegion`（一个 `SharedFile` 句柄加偏移和长度：散文件是整个文件，包里的文件是包的一段），
真正的读取由 VFS 里唯一的 I/O 线程完成。这样 Pak、网络 mount 都不用自己处理异步。

**决策**（例外）：`FileRegion` 也可以没有文件，而是带一个 `IRegionSource`（与 `SharedFile` 二选一）：`ReadAsync` 遇到它时把请求交给它完成，
完成回调同样在 IO 线程上。目前唯一的来源是 GlobalStore（06 §4.2）：`GlobalStoreClient::GetAsync` 分段、流水线地读，所以资产表和 `BulkStreamer`
不区分数据来自文件还是 GlobalStore。`Compression` 照常适用（转换结果按块压缩，06 §3.3）。

- I/O 线程持有一个 `AsyncIoQueue`（Windows 上是 IoRing，深度 64），按 `IoPriority`（`Critical` > `Streaming` > `Background`）从三个队列取请求，
  大请求按 16 MiB 切片提交。有读取在途时至少每 1 ms 看一次新请求（`PlatformAsyncIo` 还没有能唤醒完成等待的接口）。
- **块缓存**：带 `Compression` 的读取（包、GlobalStore 的转换结果）只需要首尾块的一部分时，先查 `RegionBlockCache`（全进程 32 个解压并校验过的块，LRU）；
  命中的块不再读，首尾都命中且中间没有整块时不发 I/O。8 块以上的读取在多个 job 上解压。
- **不压缩的包**：没有 codec、cipher、签名的包（`PakFile::ReadsDirectly`）给出普通的文件区间，按原偏移直接读进目标缓冲区，不经过块。
- 读取可能在 `ReadAsync` 返回前就完成（所需的块都在缓存里，且没有 job 系统时 job 就地执行），所以完成回调要用它收到的请求，不能依赖 `ReadAsync` 的返回值已经存好。
- `AsyncReadRequest` 可以 `Wait`、`IsDone`、`Cancel`（只能取消还在排队的请求）、`GetData`；完成回调在 I/O 线程上调用。
- 只读的 `DirectoryMount` 缓存打开的异步句柄（最多 256 个），省掉每个请求一次 open；可写的 mount 每次重新打开，避免读到旧文件。
- `Vfs::ShutdownAsync` 取消队列里的请求，等进行中的请求完成，然后结束线程；`Engine::Shutdown` 会调用它。

基准（`VfsBenchmarks`，Shipping，本机 NVMe）：`Vfs::Exists` 是直接 `FileExists` 的 1.01 倍；4 KiB `ReadAll` 92 ms / 5000 次，直接读是 89 ms；
256 MiB `ReadAll` 6.95 GB/s，`ReadAsync` 4.69 GB/s；4096 × 64 KiB 的 `ReadAsync` 每秒提交约 45 万个请求，总吞吐 2.69 GB/s。

### 5.6 实现现状

已实现 `DirectoryMount`、`PakMount`、`IRegionSource` 和上面的语义（`Runtime/Vfs`）。GlobalStore 来源是 `GlobalStoreRegionSource`（`Runtime/GlobalStoreAssetStore`），
按 1 MiB 分段一次发出所有 `GetAsync`。

- **包**：`PakFile`（`PakFormat.h`）打开一个 `.mpak`，表常驻内存；文件按挂载点和层（`""` 是根层）登记。`PakMount` 是一个包的一个挂载点加一个层，只读，
  文件没有原生句柄，读取走 `OpenRegion`，`Vfs::ReadAll` 和 `ReadAsync` 按块读出并解压（每个包一种 codec，`Stored` 标记表示不压缩）。
  包的格式和写包见 06 §5。
- **Packaged 形态**：`Engine` 启动时把 exe 旁 `Paks/` 里的每个 `.mpak` 打开，每个（挂载点，层）挂一个 `PakMount`。平台层的优先级高于包之间的差别
  （层的名次 × 4096 + chunk 优先级），所以补丁包改根层的文件不会盖住平台层的文件；Config 按这个顺序读每一层。没有包时报错。
- **补丁删除**：补丁包里的删除标记（`PakFileDeleted`）让这个路径在它下面的 mount 里都看不见，层查询也在那一层停下（`IFileMount::IsDeleted`）。
- **加密**：`PakCipher` 按名字选（如 `Aes256Ctr`，平台加密库实现，计数器 = 包的 salt + 位置），包头写明 cipher 和 key id，头本身是明文；
  块按序号、各个表整块按 `Pak*TablePosition` 加密，长度不变。key 由 `PakKeyProvider` 扩展给出（从 exe 或平台安全存储），从不放进 config；
  `PakCrypto::AddKey` 直接给的 key 先于 provider。
- **签名**：包旁的 `<名>.msig` 带每块的哈希和签名（06 §5.3）。链接了 `PakVerifyKeyProvider` 时，Shipping 只挂载签名通过的包；
  签名不对的包谁都不挂载。
- **层 API**：`Vfs::GetLayers`、`FindLayer`，以及只对某一层的 `Open`/`GetInfo`/`ReadAll`/`WriteAll`/`GetPhysicalPath`，用来把文件写在它所属的那一层
  （比如转换结果写回源文件所在的层，06 §2），而不是最高的可写层。
- **递归删除的保护**：`PlatformFile::DeleteDirectory(path, true)` 对 `IsSafeTreeRoot` 拒绝的路径断言并失败：空路径、卷根和顶层目录、exe 所在目录、
  当前目录、系统 KnownFolder（含 Desktop、Downloads、Pictures、Music、Videos）及其上级、用户目录的直接子目录（如 OneDrive、源码目录）、
  同级的其他用户目录及其直接子目录（直接写出或经由链接到达的都算）。目录链接只删链接本身，不跟进去。`Vfs::Delete` 不删挂载点本身。
- 尚未实现：`NetworkMount`、`IRegionSource`（06 §3.3 的迁移）、DirectStorage，以及加密以外的装饰器。
