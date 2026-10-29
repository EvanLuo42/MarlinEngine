# 06 · 资产、MarlinText 与管线

参考：David Marcelis, *Scaling Tools for Millions of Assets*（Guerrilla, GDC 2023）。

## 1. MarlinText

在 CoreText 的基础上做了小范围扩充。所有 `.masset`、Config、`.mproject`、`Product.mtext` 和编辑器主题都用这种格式。

### 1.1 基本结构（与 CoreText 一致）

```
!Version "1.0"

TextureSource
{
    !UUID   = "01832501-7918-3ee4-b177-2f200aab91b3"
    !Name   = "Rock_Albedo"
    !Schema = 3                                    // 类型的 schema 版本（04 §5）
    Format      = BC7
    MipMaps     = true
    MaxSize     = 4096
    ColorSpace  = Srgb
    SourceFile  = `source:Rock_Albedo.png#7c1e…`   // 二进制文件链接：相对路径 + 文件 GUID
}

Material
{
    !UUID = "…"
    !Name = "Rock"
    Shader     = </Project/Language/Shaders/Rock:Rock#9a0b…>  // 对象链接：路径:对象名#UUID
    BaseColor  = <Rock_Albedo#01832501-…>           // 同文件内可省略路径
}
```

- 一个文件可以包含**多个对象**，每个对象都有 `!UUID` 和 `!Name`。
- **对象链接** `<Path:Name#UUID>`：以 UUID 为准，路径只是给人看的提示。移动文件后由 Indexer 按 UUID 找到对象，并在后台修正路径提示，**不需要 Redirector**。
- **二进制链接** `` `source:Path#GUID` ``：二进制源文件同样有 GUID。GUID 记录在 `Content/.mfiles` 数据库中，由 Indexer 维护，按「路径 + 内容哈希」跟踪重命名；
  在编辑器里移动文件时同步更新。所以用 Lore 在编辑器外移动二进制文件，也能通过内容哈希重新关联上。

### 1.2 扩充内容

| 扩充 | 语法 |
| --- | --- |
| 类型化标量 | `1.5`、`-3`、`true`、`0x1F`、`"str"`、枚举标识符 `BC7`、向量 `(1, 0, 0)`。CoreText 里所有值都是字符串，MarlinText 能解析得更快，diff 也更干净。 |
| 继承 / 覆盖 | `Rock_Big : <Rock#UUID> { Scale = 2 }`：只写差异；Prefab override、材质实例、Config 层叠都用这一套 |
| 数组操作 | `Tags += [Foliage]`、`Tags -= [Static]`、`Tags = []` |
| 条件块 | `@if Platform == PS5 { … }`、`@if Configuration != Shipping { … }`：只允许出现在 Config 中 |
| Schema 迁移 | `!Schema` 加反射中注册的升级函数 |

### 1.3 实现

- 解析器在 `Runtime/MarlinText`（Config 在运行时也要用），手写递归下降，零拷贝 `StringView`，可以按对象懒解析。
- 写回时保留注释和原有的键顺序，修改时只改动受影响的行，以保证 Lore 中的 diff 最小。
- 语义级三路合并工具 `MarlinTextMerge`：按 UUID 对齐对象，按字段合并，供 Lore 作为 merge driver 使用。

**实现现状**（`Runtime/MarlinText`）：

- 对象头是 `TypeName [: <Link>] { … }`，开头的标识符总是类型名（Config 中是段名，可以带点，如 `Engine.Renderer`）；
  1.2 中的 `Rock_Big : <Rock#UUID> { Scale = 2 }` 应写成 `Material : <Rock#UUID> { !Name = "Rock_Big" Scale = 2 }`。
- 值：`null`、`true`/`false`、整数（含 `0x`）、浮点（含 `inf`/`nan`）、`"字符串"`（`\n \t \" \\ \uXXXX`）、标识符和标志 `A|B`、元组 `( )`、数组 `[ ]`、
  块 `{ Key = value }`（结构体或 map，map 的键可以是标识符、字符串或数字）、带类型的对象 `TypeName { }`（指针字段的多态对象）、对象链接 `< >`、二进制链接 `` ` ` ``。
  字段之间的 `,` 和 `;` 可写可不写。
- 条件块支持 `@else` 和嵌套，用于对象内的字段，也可以包住整个对象；`MarlinTextConditions` 提供变量，未知变量视为不成立。不是 Config 的文件出现 `@if` 是错误。
- `MarlinTextDocument`：解析时只找出对象边界，对象第一次被访问时才解析字段；编辑接口（`SetField`、`RemoveField`、`AddObject`、`ReplaceObject`、`RemoveObject`）直接改原文，
  新字段加在对象末尾、指令加在其他指令后面，删除字段连同行尾注释一起删掉。
- `MarlinTextSerializer` 遍历 `TypeInfo` 读写任意反射类型：`WriteObject` 默认只写与默认值不同的字段；`ReadObject` 先读被覆盖的父对象（由调用方解析链接），再做 schema 升级，未知字段只是警告；
  `UpdateObject` 读出文件当前的值（包括父对象和 `+=`/`-=`），只重写不同的字段。`Transient` 字段不读不写。
- 尚未实现：`MarlinTextMerge`、按对象懒解析之外的增量重解析（每次编辑重新扫描对象边界）。

### 1.4 Config 层叠

`/Engine/Config/X.mtext` → 平台层栈（Generic → … → Windows）→ `/Project/Config` → 项目平台层
→ `/User/Config`（Saved）→ 命令行 `-Config:Section.Key=Value`。
Config 段落绑定到反射类型（`MCLASS(Config="Engine.Renderer")`），热重载时会通知标了 `MPROPERTY(HotReload)` 的字段。

**实现现状**（`Runtime/Config`）：

- 段名 `File.Section`：`Engine.Renderer` 是 `Engine.mtext` 文件里的 `Renderer { }` 对象。文件在第一次用到时读入并解析，之后缓存。
- 层的顺序：`/Engine/Config` → `/Project/Config` → `/User/Config` → `-Config:File.Section.Key=Value`。
  每个目录用 `Vfs::ReadEachLayer` 读，所以平台层（Generic → … → Windows）自动夹在对应目录里（02 §5.4），不需要单独列出。
- 条件变量：`Platform`、`Configuration`、`Target`。
- `Config::Apply(type, object)` 按顺序把每一层读进对象，后面的层覆盖前面的，`+=`/`-=` 在上一层的结果上增删。
  `Config::ApplyStatics()` 在启动时把所有带 `Config` 元数据的类型的静态字段填好（`MarlinTextReadOptions::StaticFieldsOnly`，实例字段被跳过）。
- `Config::TryGet<T>("Engine.Loop.FixedRate", value)` / `GetOr<T>(key, default)` 读单个值，不需要反射类型；`Config::ForEachSection` 访问每一层的原始对象，`ForEachSectionWithConditions` 另外给出 `@if` 要测试的变量，供自己遍历字段的读者（如编辑器主题）使用；段名的文件部分可以带子目录（`Editor/Theme.Colors` 读 `Config/Editor/Theme.mtext`）。
- `Config::Reload()` 丢掉缓存重新读，并重新填静态字段。
- **玩家可写的层**：`ConfigDesc::UserDirectories`（`/User/Config`）和 `-Config:` 是玩家能改的层。Shipping 里（`ConfigDesc::RestrictUser`）
  它们只作用于类型标了 `MCLASS(Config="…", UserConfigurable)` 的段（如 `Engine.Window`），并且该类型不能有 `Flags=Cheat` 的字段，
  否则整个段都不让玩家设置（启动时报错）；没有类型的原始键（`TryGet`、`ForEachSection`）也只读不可写的层。Shipping 的 `Engine` 也不收
  `-Config:` 和 `-ExecCommands`。所以 Shipping 中 Cheat CVar 只能由 `/Engine`、`/Project` 的 Config 设置，玩家改不了（03 §7）。
- 尚未实现：文件监控和 `HotReload` 通知、把设置写回 `/User/Config`（编辑器偏好）。

## 2. Asset Indexer

**决策**：守护进程 `MarlinAssetIndexer`，每台机器每个项目一个实例；手动启动时常驻，由 Editor、Game 拉起时在没人用之后自动退出（见下文「守护进程」）。

- **只索引引擎的实例**（决策，尚未实现）：没有项目时（编辑器的项目浏览器，01 §5.1）也需要转换 `/Engine/Content`，所以 Indexer 也可以不带项目启动，
  只索引 `/Engine`：管道名和锁按 EngineRoot 而不是项目根，快照放在 `<EngineRoot>/Intermediate/AssetIndex/`。转换结果照样进 GlobalStore，
  key 不含项目，所以之后打开任何项目时，引擎资产都已命中。

- 扫描 `/Engine`、`/Project` 下的所有 `.masset` 和二进制文件，用 OS 文件监控（ReadDirectoryChangesW / inotify / FSEvents）做增量更新。
- **只索引所有类型共有的部分**：文件表（路径哈希、目录、扩展名、内容哈希、对象区间）、对象表（UUID、Name、Type、链接区间）、链接表（正向，反向按需构建）、二进制文件链接表、文件的版本控制状态。
- 自研的内存映射列式数据库，存放在 `<Project>/Intermediate/AssetIndex/`，写入 WAL 后周期性 compaction；启动时直接 mmap，不需要重新扫描。
- 查询接口（通过 Remote 协议的命名管道，10）：按类型、名字、路径、标签过滤；正向和反向引用；依赖闭包；按 UUID 解析路径。
- 客户端：Editor（Asset Browser、Reference Viewer）、开发版 Game（按 UUID 定位资产）、Converter、LSP、DCC 插件、MCP。
- 每个文件的扫描结果作为**提取结果**（extract）存进 GlobalStore（§3.3）：新机器、换分支时按版本控制记录的已提交内容哈希批量取回，不打开文件（「换项目或分支只需要一分钟」），
  只解析本地有差异的部分。
**实现现状**（`Developer/AssetPipeline`、`Programs/MarlinAssetIndexer`）：

- **数据库镜像**（`AssetDatabase`、`PackedColumn`）：只读的内存镜像，可以原样写进文件或共享内存再映射回来。整数列按 4096 行分页，
  每页存基值和位宽，超过 56 位的列存原始 64 位；路径拆成目录、文件名、扩展名三个字符串池，对象名、类型、标签、失效路径各一个池，
  池内去重。按路径、UUID、二进制 GUID 查找走开放寻址哈希表，反向链接是建库时排序生成的 CSR。100 万对象、25 万文件的镜像约每对象 52 字节，
  建库 0.24 s（Profile，6 线程）。
- **索引**（`AssetIndex`）：基础镜像加增量镜像。`Update / Remove` 先进内存，`Commit` 只用变化的文件生成新的增量镜像，被替换或删除的路径记进
  失效池；没有新变化的 `Commit` 什么也不做。自基础镜像以来变化的文件超过 `FoldThreshold`（4096）个时折叠进新的基础镜像：旧基础镜像的行
  按列直接拷贝（字符串池按 id 重新映射），不还原成记录，再和变化的文件按路径归并。写者（`Commit / Compact / Save / Load / Clear`）轮流持有
  一把写锁，新镜像在读写锁之外生成、写盘、映射，只有换上去的一瞬间才独占读写锁，查询不必等折叠。`Compact` 在没有变化、基础镜像也已经是快照时
  直接返回。文件数、对象数等计数每个基础镜像只数一次，增量镜像在此基础上增减。`AssetIndexView` 的查询：按 UUID 单个或批量查找（`FindMany`，
  单线程约 1800 万次/秒）、规范化哈希和二进制哈希、按类型、名字通配符、目录、标签过滤（`Query`，按页用 `Jobs::ParallelFor` 并行，
  每次一批页，够了 `Limit` 就停）、正向和反向引用、依赖闭包；镜像里的文件按路径排序，所以某个目录下的文件（`ForEachFileBelow`）是二分查找出的一段。
- **持久化**：`<Project>/Intermediate/AssetIndex/Index.<代数>.snapshot` 就是基础镜像本身，启动时直接映射，不解析、不拷贝；之后的每次变化追加到
  `Index.log`（每条带校验和，末尾写了一半的记录在重放时丢弃），每次提交的记录一次写入，启动时重放。日志超过 64 MiB 或正常退出时写新代数的快照并删掉日志；
  这次提交本来就要写快照时（日志会超限，或者还没有快照），不再先写日志。
  `-Seed=<快照>` 用 CI 生成的快照起步：路径都是虚拟路径，换机器也能用；大小和时间不一致的文件重新算内容哈希，哈希相同就不重新解析。
  同步后本地的修改时间和 CI 不同，所以种子里的文件几乎都要重新读；提取结果（下文）上线后删掉 `-Seed`。
- **共享给其他进程**（`AssetIndexPublisher` / `AssetIndexReader`）：基础镜像留在快照文件里，每次 Commit 的增量镜像拷进一段新的命名共享内存
  （`<管道名>.Index.<代数>`，保留最近 3 代），控制块用 seqlock 记录当前代数和两者的位置。Editor、Converter、开发版 Game 映射之后在自己进程里
  查询，一次查找的开销和在 Indexer 里一样，不经过管道；每次读取用一次原子读检查有没有新代数。
- **扫描**（`AssetScanner`）：扫描 `Engine.AssetIndex` 段 `Folders` 列出的子目录（默认 `Content`），范围是 `/Engine`、`/Project`；
  通过 VFS 读，所以看到的是平台层叠加之后的结果。大小和修改时间都没变的文件跳过，大小和时间直接用列目录时得到的，不再逐个查询；
  时间变了但内容哈希和索引里的相同（touch、版本控制重新检出、原样保存）时只更新时间戳，不算变化，不会让任何转换结果失效；目录只有一层时
  也不必用 `Vfs::FindLayer` 查文件在哪一层。`.masset` 解析出每个对象的 UUID、名字、类型、行号、对象链接（包括被覆盖的父对象）、二进制链接、
  `Tags` 字段和规范化哈希（§3.2），读一次文件，同时算内容哈希；其他文件都是二进制源文件，内容哈希是映射文件后算的 XXH3-128。要读的文件用
  `Jobs::ParallelFor` 并行哈希、解析，再按顺序写进索引。重新扫描目录时，目录里已经不存在的文件从索引里删掉。每次扫描（`ScanAll`，
  或者 `ScanPaths` 处理的一批变化路径）是一批：改过的 `.mfiles` 和索引的 `Commit` 都在批末各做一次。
- **平台层**：扫描器按 `Vfs::GetLayers` 逐层扫描每个目录（根层和各平台层），`Vfs::FindLayer` 判断一个文件实际取自哪一层；高层覆盖低层的同名文件时
  沿用低层文件的 GUID，所以无论哪一层生效，链接都能解析。
- **`.mfiles`**：每个内容目录（如 `/Project/Content`）、每一层一个，记录这一层里该目录下所有二进制文件，`BinaryFileEntry { !UUID Path Hash Size }`，
  按路径排序写出，diff 稳定。路径没变就沿用 GUID；新文件的哈希和某个已经不存在的条目相同，就认为是改名，沿用那个 GUID（按哈希查找候选，
  本次扫描已经确认还在的条目不再查询文件系统）；剩下的才分配新 GUID。文件删除后条目也删掉。扫描器记下每个 `.mfiles` 读入或写出时的大小和
  修改时间，只有变了才重新读，所以自己写出 `.mfiles` 引起的变化事件、以及重新扫描时没变的 `.mfiles` 都不会再解析一遍。
  `.mfiles` 用 `Vfs::WriteAll(layer, …)` 写进文件**所在的那一层**，而不是最高的可写层；导入器写 `.masset` 也一样（§3.1）。
- **变化来源**：NTFS 卷上读 USN journal（`ChangeJournal`，非特权读取，打开目录句柄即可），游标存在 `Index.journal`，所以 Indexer 没运行期间的
  变化在下次启动时重放，不必重新扫描全部文件。非特权记录没有文件名：存在的文件按文件 ID 解析路径，已删除的文件报告它所在的目录，
  由扫描器重新扫描那个目录。没有 journal 的卷（以及其他平台）退回到每个目录一个 `DirectoryWatcher`，丢事件时重新扫描整个目录。
  监控覆盖每个目录的每一层。
- **服务**（`AssetIndexService`）：在管道 `Marlin.AssetIndexer.<项目根路径哈希>` 上提供 `AssetIndex.Find / FindBinary / CanonicalHash / BinaryHash /
  Query / References / Referencers / Dependencies / Status / Rescan`、`Asset.Locate`、`Shader.Locate` 和 `Script.Locate`（§3）、`Converter.Take / Finish`；
  每次变化推送 `AssetIndex.Changed { Paths }`，转换结果因源变化而过期时推送 `Asset.Changed { Uuids }`（§6）。
  `Asset.Locate` 或 `Shader.Locate` 没命中 Indexer 记住的定位结果、派给 worker 时，只向发出这次请求的连接推送
  `Asset.Converting { Uuid, Name, Shader }`（`RemoteServer::Notify`，不用订阅；`Name` 是对象名或 shader 入口点），
  客户端据此把这一项标成「转换中」或「编译中」，供 Splash 显示具体在转换什么（09 §4.6）；命中时不发。
  worker 之后可能发现结果已在 GlobalStore 里，所以它说的是「交给 worker 了」，不保证真的重新转换，这种情况一闪而过。
  `Log.Stream` 推送 Indexer 自己的日志，以及 worker 用 `Converter.Log` 通知转来的日志（带 `Source`，如 `MarlinConverter 1234`）；worker 只发 Info 及以上，
  没人订阅时 Indexer 不格式化也不发送。Editor、开发版 Game 订阅后记在 `LogAssetIndexer` 下（`[来源] 原分类: 消息`，`Engine.GlobalStoreAssetStore.ShowIndexerLog`，默认开）。
  `AssetIndex.Status` 另外给出进度：`FilesToRead / FilesRead`（自启动以来各次扫描发现要读的已变化文件和读完的数量）、
  `ConversionsQueued / ConversionsRunning`、`Converted / ConversionCacheHits / ConversionsFailed`（`ConverterPool::GetProgress`），供 Splash 显示（09 §4.6）。
  多数客户端只在订阅变化和请求转换时才用管道，查询走共享映射。第一次全量扫描完成前到达的 `Asset.Locate` 先排队，扫描完再应答，不会失败。
- **Shader 源和 Config**：同时监控每个被索引的内容目录旁边的 `<根>/Language` 和 `<根>/Config`（每一层）。任何语言文件变化都让 shader、
  反射 shader 的材质（转换器输入 `MaterialShader:`，`ConversionOutcome::ReadLanguage`）失效，脚本文件变化让脚本模块失效；任何 config 文件变化
  让读过 config 的结果（`ReadConfig`）失效，并把 config 代数加一：任务带上代数（`ConversionJob::ConfigGeneration`），worker 发现比自己读到的新时先
  `Config::Reload`。是否真的变了由 worker 按 key 判断（§3.2），Indexer 只负责不漏。
- **失效与重新验证**：扫描提交、发布新索引之后才失效（worker 要看到新索引）。失效的定位结果先丢掉；有人订阅 `Asset.Changed` 时，Indexer 在后台把它们
  重新转换一遍，**只有结果 key 变了**（或转换失败）才推送：编辑同一个 `.masset` 里的另一个对象、只改了 shader 没用到的文件、关卡里没变的 cell，
  客户端都不必重载。没人订阅时只丢掉，下次 `Locate` 再转换。定位结果带上请求时的代数，期间又有失效的不记。
- **重启之后**：正常退出时把定位结果写进 `<StoreDirectory>/Located.json`（shader、脚本模块和读过 config、shader 源的除外，它们的目录只在运行时监控），
  下次启动在重放 journal 之前读回，只留 GlobalStore 里还有的，读完删掉文件；所以重启后第一次 `Asset.Locate` 不必经过 worker。
- **守护进程**：`MarlinAssetIndexer -Project=<.mproject>`，用 `Intermediate/AssetIndex/Indexer.lock`（独占打开、关闭即删）保证每个项目只有一个；
  `Engine.Quit` 让它写快照后退出。
  客户端自动拉起的 Indexer 带 `-ExitWhenIdle`：除了 Converter worker 没有客户端连着（worker 随 Indexer 一起退出）、没有扫描、也没有等待中的转换（`AssetIndexService::IsIdle`）
  持续 `Engine.AssetIndex.IdleExitSeconds`（默认 10）秒后，写快照并退出，所以 Editor、Game 关掉后它不再占着文件（模块化构建下会锁住引擎 DLL，
  导致下一次构建失败）。手动启动的 Indexer 不带这个开关，照旧常驻。下次再拉起时直接映射快照、重放 journal，不需要重新扫描。
- **客户端**（`AssetIndexClient`）：连不上时从当前可执行文件的目录启动 `MarlinAssetIndexer`（隐藏窗口）并等它响应；连上后同时打开
  `AssetIndexReader`，查询都在本进程里完成。MarlinConverter worker 也一样：`ClientSourceResolver` 经 `AssetIndexClient` 读共享映射，
  查对象、二进制文件和规范化哈希都不走管道；索引没有发布映射时退回管道（`AssetIndex.CanonicalHash / BinaryHash`）。
- **提取结果**（决策，尚未实现）：一个文件的扫描结果（`AssetFileRecord`：对象的 UUID、名字、类型、行号、链接、二进制链接、Tags、规范化哈希；
  二进制文件是内容哈希）存进 GlobalStore，key = `Hash128(提取域, 扫描器版本, 虚拟路径, 大小, VCS 名, VCS 记录的已提交内容哈希)`。
  Guerrilla 的 key 是（路径、大小、修改时间）；同步后的修改时间通常是本地写入时间，不能跨机器比较，所以换成版本控制记录的已提交内容哈希。
  它由 `ISourceControl::QueryTracked`（09 §7）给出：每个受控文件的大小、已提交内容的哈希，以及 VCS 用它自己的 stat 缓存（大小、修改时间）确认的
  「工作副本等于已提交内容」（`Clean`），整个过程不读文件内容。哈希只当不透明的标识（各家算法不同：Lore 是 BLAKE3，SVN 是 SHA-1），
  所以 key 里带 VCS 名，不同 VCS 的哈希不会混用；扫描器版本变了，所有提取结果自然失效。
  扫描时，本地索引里没有或大小、时间变了的文件先查 `QueryTracked`，`Clean` 的用 `GetMany` 批量取提取结果，命中就直接 `AssetIndex::Update`；
  未命中、未受控或本地改过的文件才读、解析，其中 `Clean` 的再 `Put`。CI 上的 Indexer 每次提交后扫描一遍，预先填好。
  没有 `ISourceControl` 实现（或识别不出工作区）时照常读文件。
- 尚未实现：版本控制状态和 `QueryTracked`（`ISourceControl`，09 §7）、提取结果、正向链接以外的链接区间（目前只存 GUID）。

## 3. 转换（Converter）与 GlobalStore

### 3.1 转换

- `MarlinConverter` worker 进程池，由 Indexer（后台预热）或 Editor、Game（按需）调度；转换崩溃不会影响编辑器，可以分布式运行。
- 转换单位是**对象**，不是文件。输入是 Source 对象加上它的依赖闭包，输出是平台相关的 Runtime 二进制块。
- 每个 Converter 声明自己的 `Version` 和它会读取哪些依赖（例如材质 → shader 源文件、引用的纹理格式）。
- **实现现状**：`AssetConverter` 是扩展点（13 §3），任何模块都用同样的方式添加，用 `Priority` 选择、用 `Replaces` 包装另一个转换器。
  Indexer 里的 `ConverterPool` 排队任务、管理 worker 进程（`Engine.Converter.Workers`，默认 4，0 表示只接受外部 worker）；
  `MarlinConverter -Indexer=<管道>` 用 `Converter.Take` 取任务、`Converter.Finish` 交结果，`Asset.Locate` 的调用方拿到的是结果 key 和逻辑偏移（§4.2）。
  worker 死掉的任务换一个 worker 重试两次再失败；worker 按需启动，空闲后退出。
  两个调用都可以成批：`Take` 带 `{"Max": N}` 时一次拿到最多 N 个任务（`{"Jobs": […]}`，按等待的任务数在 worker 之间平分），`Finish` 带
  `{"Outcomes": […]}` 一次交多个结果；不带参数时仍是一个任务、一个结果。worker 每次要 8 个，先用一次 `ConvertMany` 查出其中的命中，
  结果最多攒 20 ms 再交。任务按 Guid、目标和入口点合并（哈希表），紧急和后台各一个队列，`Tick` 只检查 worker 手上的任务。
  每个 worker 按自己分到的核数（逻辑核数 / `Engine.Converter.Workers`）起 job 系统，纹理编码等按条并行的转换真正并行。
- **过期**：转换读到的源文件和索引扫描时的不一致（`ConversionResult::Stale`），这次转换失败、不写 GlobalStore；worker 等索引跟上后重试（最多 5 次，间隔 100 ms）。
- **源格式**：转换器不自己读文件格式，而是调用 `ConversionContext::Decode`，按扩展名和文件头选 `ImageDecoder` / `SceneDecoder`（13 §7.1），
  用到的解码器记进依赖记录（§3.2）。引擎自带 `PngDecoder`；FBX（`FbxSceneDecoder`，ufbx）和 glTF 2.0（`GltfSceneDecoder`，cgltf）的 `SceneDecoder` 各是一个 Developer 模块。
  源文件引用的其他文件（如 glTF 的外部 `.bin`）由解码器经 `ISourceFileReader` 读取，不直接碰文件系统：路径相对源文件所在目录解析
  （`ResolveReferencedPath`：不许绝对路径、带 `:` 的 scheme 或盘符，也不许 `..` 跑出源文件所在的挂载点），
  转换时 `ConversionContext::ReadReferencedBinary` 经索引按路径查到它的 GUID（`IAssetSourceResolver::FindBinaryAt`，Indexer 的 `AssetIndex.FindBinaryAt`），
  记进依赖记录，所以改了 `.bin` 会重新转换；导入时直接经 VFS 读。读取可以只读一段（`ReadReferencedRange`），大文件不必整个进内存：
  读整个文件时像 `ReadBinary` 一样按索引的哈希检查是否过期，只读一段时改为比较索引扫描到的大小和修改时间（每次都哈希整个文件太贵）。
  `ConversionContext::Decode` 可以给出网格名，解码器只需为这个网格解出图元，能跳过的就只读它用到的数据。
  解码后的场景已经换算到引擎空间（右手、Y 向上、米，03 §6）。
- **导入**：`AssetImporter` 也是扩展点，只看文件、不做重活：把一个源文件变成它旁边 `.masset` 里的 Source 对象（`Rock.png` → `Rock.masset`）。
  对象 UUID 由导入器类型、文件 GUID 和子路径（如场景里的网格名）派生，所以重新导入只增加新的部分，已有对象保持人改过的样子；
  已有的 `.masset` 只解析一次，新对象一次追加。`MeshImporter` 用 `SceneDecoder::DecodeOutline` 只读出节点、网格和材质的名字，
  不解码几何体（ufbx 的 `ignore_geometry`，glTF 不加载缓冲区）。
  `.masset` 写进源文件所在的 VFS 层。
  **决策**（导入选项，参照 UE Interchange 的导入窗口）：导入器可以声明一个反射的**导入选项**类型（如 `MeshImportOptions`），只放决定「生成哪些对象」的选项，
  影响转换的设置留在 Source 对象上。选项作为一个对象存进 `.masset`，相当于 UE 的 AssetImportData：重新导入（包括 `-Import` 和源文件改动后的自动导入）
  读存下的选项，不弹窗；取值顺序为本次导入窗口给的 > `.masset` 里存的 > 类型的默认值。编辑器导入文件时，导入器有选项类型就先弹窗，
  用反射属性面板显示选项（预填存下的值），确认后才导入；另有「带选项重新导入」再次打开窗口。改了选项再导入仍只追加：之前生成、现在不再生成的对象保留，不自动删除。
  实现现状：`AssetImporter::GetOptionsType` 声明选项类型，`ImportContext::GetOptions<T>()` 读本次的选项；`AssetImport::Import(path, binary, options)`
  先读 `.masset`，按上面的顺序取选项，选项对象的子路径是 `:Options`（名字 `<文件名>_ImportOptions`），第一次导入时追加，给了选项时原地改写。
  选项对象没有转换器，预热只转有转换器的类型，所以不受影响。`MarlinConverter -Import=<路径> -ImportOptions="Combine = false"` 代替导入窗口给选项。
  `ImportContext::AddObjectText` 给不是反射 struct 的对象（如 `PrefabEntity`）写整段文本，`GetUuid(subPath)` 让对象在添加前就能互相链接。
  尚未实现：导入窗口和「带选项重新导入」（等编辑器的 Import 和反射属性编辑器）。
  目前的导入器：`TextureImporter`（§3.4）、`MeshImporter`（§3.5）、`FontImporter`（§3.6）、`SvgImporter`（§3.6.1）；
  `MarlinConverter -Import=<虚拟路径,…>` 做批量导入。
- **全局 shader**：worker 也接 `Shader.Locate` 派来的任务，交给 `GlobalShaderConversion`（ShaderPipeline，MarlinLanguage.md §1.6）。
- **脚本模块**：worker 也接 `Script.Locate` 派来的任务（`ConversionJob::Script` 是包名），交给 `ScriptModuleConversion`（ScriptPipeline，§3.8）。
- **CI 预热**：`MarlinConverter -Warm -Platforms=Windows,PS5 [-Threads=N]` 在本进程里扫描项目并转换所有对象（`WarmGlobalStore`），写进 GlobalStore，已有的只计命中。
  `-Warm`、`-Import` 和 MarlinPackager 先只读地载入 Indexer 的快照和日志（`LoadProjectIndex`），扫描只读之后变化的文件。
- **源文件解析缓存**：一个 `AssetConversion` 里的转换共用解析过的 `.masset`（按路径，大小和修改时间不变就复用，最多 64 个），所以一个文件的多个对象
  （网格导入、Prefab、关卡）只读、只解析一次。

### 3.2 结果 key 与依赖检查

```
结果 key = Hash128( Converter.TypeId, Converter.Version,
                    SourceObject 规范化后的 MarlinText 哈希,
                    每个依赖的「影响转换的部分」的哈希,
                    二进制源文件的内容哈希,
                    用到的每个 SourceDecoder 的 TypeId 和 Version（13 §7.1）,
                    TargetPlatform, TargetKind(Client/Server), 相关 Config 的哈希 )
```

**决策**：**依赖检查**（dependency checking）只读索引，不读源文件。key 分两步：

1. **输入 key** = 转换器链的类型和版本、块布局哈希、块格式版本、目标、UUID、源对象的规范化哈希。
2. 输入 key 对应一条**依赖记录**（dependency record，存在 GlobalStore 里，§3.3），记着上次转换**实际读过**的输入；用索引里这些输入**当前**的哈希
   补全成**结果 key**。

检查时用 `GetMany` 批量取依赖记录（转换一个对象时，它链接到的对象一起查，一次流水线往返），再用 `HasMany` 查结果 key：都在就命中，
不打开任何源文件；缺的才读源对象、运行转换器，然后 `Put` 转换结果和依赖记录。依赖记录只含转换器真正读过的输入，范围比整个链接闭包窄。

- 依赖记录的输入种类：对象、二进制文件、Config 段、解码器（`ImageDecoder` / `SceneDecoder` 的类型和版本），以及转换器自定义的输入
  （`ReadConverterInput`，检查 key 时调 `AssetConverter::WriteInput` 取当前值，如纹理转换器为某个格式可选的所有编码器）。
- Config 段按目标平台读（`GetConversionConfig`）：目标平台的层、`@if Platform` 取目标的值、`Target` 按 Client/Server 取 Game/Server，
  `Configuration` 不定义，因为转换结果所有构建配置共用。`ReadConfig(section, …, part)` 可以只依赖段里的一部分（`Engine.Textures#Compression.Color`）：
  只哈希每层里这条路径上的字段（连同它们的运算和 `@if`），所以给另一种纹理加一个预设、改它的格式，不会让这种纹理重新转换。
- **重定向**：GlobalStore 的值只写一次，输入 key 下的依赖记录永远是第一次转换的。之后的转换读了别的输入（依赖对象多链接了一个对象、shader 多用了
  一个文件），而输入 key 不变时，它的依赖记录写在 `RedirectKey(上一条记录给出、却未命中或不对的结果 key)` 下；查找时从输入 key 的记录出发，沿重定向
  最多走 8 次（每次一个往返，批量查的对象一起走）。没有它，这种对象每次检查都会重新转换。
- **全局 shader** 的 key（`GlobalShaderConversion`）：转换器版本、编译器各模块的源码哈希、入口点、目标、`Engine.Shaders.Profiles`、编译选项；
  在这个输入 key 下记录上次编译用到的源文件，再加上它们现在的哈希。编译器本身或入口点用到的任何源文件（直到核心库）改动都会未命中。
  - 用到的文件以外的变化由**名字记录**判断（`ShaderNameRecord`，按结果 key 存）：前端记下编译在命名空间和类型里查找过的每个（容器限定名, 名字），
    分「找到了命名空间」和「没找到命名空间」两类，以及是否匹配过 extension；还记下当时源集里每个文件的哈希。检查时，新增或改过的、编译没用到的文件
    只解析声明（`GetDeclaredNames`，每种文本解析一次），声明了查找过的名字（同名命名空间只对「没找到命名空间」的名字算）或在匹配过 extension 时声明了
    extension，才未命中。所以加一个更近的同名函数会重新编译，加一个无关的材质文件不会。删除没用到的文件不影响结果。
  - **精确记录**：`ExactRecordKey(结果 key, 源集哈希)` 表示这个结果对这个源集是对的，同一状态下不用再解析；名字记录判断「可能有影响」、重新编译却得到
    同一个结果时，也靠它不再重复编译。
- **材质** 的转换器输入 `MaterialShader:<struct|变体>` 是 struct 的反射结果；每个进程按（材质, 变体）缓存反射，用到的文件和名字记录都没变就不重新编译。
- **脚本模块** 的 key（`ScriptModuleConversion`）：转换器版本、编译器各模块（LanguageFrontend、LanguageIR、ScriptBackend、ScriptPipeline）的源码哈希、
  `ScriptModuleAsset` 的布局哈希和字节码格式版本、包名、目标、编译选项，以及源集里**所有** `.mscript` / `.mcommon` 文件的路径列表；输入 key 下记录这些文件，
  再加上它们现在的哈希。一个包能看到源集的全部文件（MarlinScript §1.3），所以新增文件（改变路径列表）和任何文件的改动都会未命中，
  而不只是上次编译读过的那些。这个源 key 再经一条记录指向**按内容寻址**的结果 key：重新编译写出同样的模块（如 `/Project` 下的改动让 Engine 包重编）时
  结果 key 不变，已加载的模块不必重载。文件哈希按大小和修改时间缓存。
- **编译器源码哈希**（`<Module>.SourceHash.gen.h`）：MarlinHeaderTool 哈希模块的源文件时去掉注释、把空白压成一个（保留换行），所以只改注释和排版
  不会让 shader、脚本模块全部重新编译。
- **派生结果**（关卡的 cell）的 key 按自身内容：`DerivedKey(Uuid, 块的哈希)`，父结果的 `DerivedList2` 记录列出每个派生结果的 key。编辑关卡时，内容没变的
  cell 保持原来的 key。

### 3.3 GlobalStore

参考 Marcelis（Guerrilla, GDC 2023）的 GlobalStore 与 GlobalStore streaming。

**决策**：转换结果（converted result）和依赖记录**只存在 GlobalStore 里**。Converter 直接 `Put`，Editor、Game、Packager 按结果 key 分段 `Get`，
本机不留副本。没有「不用 GlobalStore」的方式：单人开发也跑一个本机 GlobalStore，和 Indexer 一样由客户端自动拉起。

**决策**：GlobalStore 是自研的键值服务，而不是 HTTP 或文件共享：固定 48 字节请求头、32 字节响应头的二进制协议，跑在 TCP 上，请求可以流水线发送，
同一连接上按顺序应答；操作只有 `Get`（可以读一段）、`Put`、`Has`。值不可变、按 key 寻址，所以不需要任何失效机制。服务器把值放在内存（按 `MemoryBudget`
LRU 淘汰）和只追加的段文件里，启动时扫描段文件头重建索引。它不只存转换结果：key 是 `Hash128`，每种值（转换结果、依赖记录、提取结果 §2、
以后的构建映射）用自己的域常量做种子，互不冲突。

```
Engine.GlobalStore
{
    Host = ""             // 空：本机 GlobalStore，连不上就启动 MarlinGlobalStore（-ExitWhenIdle）；否则连这台服务器
    IdleExitSeconds = 10  // 客户端拉起的 MarlinGlobalStore 没有连接这么久后退出，不再占着引擎 DLL
    Port = 7480
    Codec = "Zstd"        // 转换结果分块压缩的格式，空或 None 为不压缩
    Level = 0             // Codec 的级别，0 为默认
    // 以下由 MarlinGlobalStore 进程读取
    AllowRemote = false   // 工作室服务器设为 true，监听所有网卡
    DataDirectory = ""    // 默认 <Engine>/Saved/GlobalStore
    MemoryBudget = 1073741824
    Upstream { Host = "" Port = 7480 }   // 配了就是 GlobalStore proxy
}
```

三种部署，只是 `Host` 和 `Upstream` 的组合，不是模式开关：

| 部署 | 配置 | 场景 |
| --- | --- | --- |
| 本机 GlobalStore | `Host` 空，无 `Upstream` | 单人开发 |
| 工作室 GlobalStore | `Host` 指向服务器 | 内网：所有工作站、devkit、构建机共用一台 |
| GlobalStore proxy | `Host` 空，`Upstream` 指向工作室 | 远程办公：未命中从上游取来并留在本机，写入转发给上游 |

- **连不上**：`Host` 指向的服务器连不上时，这次运行退回本机 GlobalStore（启动它）并警告，转换照常进行；期间的结果只留在本机，服务器恢复后不补传
  （按内容寻址，别人需要时自己转换）。远端最多每 10 秒重试一次。
- **转换结果的格式**（`ConvertedResult`）：头（Magic、版本、结果 key、块区和 bulk 区的逻辑位置、codec 名、块大小、块表），然后是按 64 KiB 逻辑块分别压缩的数据，
  压缩不变小的块原样存。读取方按逻辑偏移只 `Get` 覆盖到的块（`IRegionCompression`，与 `.mpak` 同一套），所以纹理 mip、网格页按需流入。
  没链接该 codec 的进程视为未命中，重新转换。服务器本身不感知压缩，只存字节。
- **CI 预热**：每次提交后 CI 用 `MarlinConverter -Warm` 转换所有平台，写进工作室 GlobalStore；本机 Indexer 启动时另有一步预热，见 §3.7。
- 开发版的 Editor 和 Game **直接从 GlobalStore 加载转换结果**（GlobalStore streaming，§4.2），不需要先完整 cook；缺失的由 Indexer 现场转换，界面上显示进度。

**决策**（服务器 I/O）：网络层本来就是完成端口（14 §2，Windows IOCP、Linux io_uring）。在此之上：

1. **零拷贝发送**：应答是「头 + 对缓存值的引用」，用分散/聚集发送（14 §4），值的引用计数持有到发送完成；数据从服务器内存经网卡 DMA 发出，CPU 不再搬运。
2. **冷数据异步读**：段文件用重叠 I/O 关联到同一个完成端口，读完再发，不在 IO 线程上阻塞；大值直接 `TransmitFile`（Linux `sendfile`），内核从文件缓存送到网卡。
3. **分片的条目表**：按 key 哈希分片，各有自己的锁。
4. Registered I/O（RIO，内存缓存放进预注册的内存块直接发送）视基准结果再定。基准 `GlobalStoreBenchmarks`：单线程 4 KiB `Get` 次数/秒、
   多连接吞吐、冷数据读取；参照 Guerrilla 公布的单线程约 75,000 次/秒。主机平台的等价物写在平台层（12）。

**实现现状**（`Runtime/GlobalStore` 协议和客户端，`Developer/GlobalStoreServer`，`Programs/MarlinGlobalStore`）：数据目录用
`GlobalStore.lock` 保证只有一个服务器。连接走 Net（14）：客户端每个请求立即发出、回调按请求顺序在 IO 线程上执行，同步调用等这些回调；
服务器在 IO 线程上解析请求，每个请求占一个应答槽，前面的槽都填好才按顺序发出，所以上游未命中时只等上游的应答，不占用 IO 线程。
IO 线程数取 `Engine.Net.Threads`。没有 NetSubsystem 的进程（Indexer、Converter）自己起一个 NetLoop。

- **服务器 I/O**（上面的决策 1–3）：应答槽里放的是 32 字节头加「对值的引用」：内存里的值用 `NetStream::Send(owner, bytes)` 引用发出，
  和头一起 gather（`WSASend` 一次最多 64 段），值的引用计数持有到发送完成，淘汰只是让缓存放手。不在内存里的值：小于 1 MiB 的用重叠 `ReadFile`
  在 NetLoop 的同一个完成端口上读（`IoPort::Read`），读完放进缓存再发；不小于 1 MiB 的不进内存，`NetStream::SendFile` 把请求的那一段用
  `TransmitFile` 从段文件直接发出，头作为它的 head。每个段文件开两个句柄：同步的写句柄追加，overlapped 的读句柄关联到端口；段对象引用计数，
  条目、在途的读和发送都持有它。`Has` 和读不到内容的 `Get` 只看条目，不读文件。条目表按 key 高 6 位分成 64 片，各有一把锁；追加段文件另有一把锁，
  在片锁里拿，所以同一个 key 只写一次；缓存字节数是原子计数，淘汰同时只有一个线程做（跨片按 LRU）。`Put` 写段文件仍是同步的。
  基准 `GlobalStoreBenchmarks`（`Developer/GlobalStoreServer/Tests`）：单线程逐个和流水线的 4 KiB `Get`、8 和 32 个连接的吞吐、
  `MemoryBudget = 0` 时的冷读（4 KiB 走重叠读，4 MiB 整值和 64 KiB 块走 `TransmitFile`）。Linux 没有平台层，`sendfile` 和 io_uring 的读尚未实现。

- **迁移**（从本机 `/Cache` 的 DDC 转到只用 GlobalStore）：
  1. `ConvertedResult` 分块格式：已完成（`GlobalStore/ConvertedResult.h`）。值是 96 字节的头（Magic `MSCR`、版本、结果 key、块区和 bulk 区的逻辑位置、
     逻辑总长、codec 名、块大小 64 KiB、块数、数据起点），接着块表（每块 16 字节：在值里的偏移、存储大小、是否原样存），再接各块的数据；
     bulk 在逻辑流里 16 字节对齐。`ConvertedResultReader` 解析头和块表、解压单块，`ConvertedResultWriter` 在 job 系统上并行压缩各块。
     `FileRegion` 的 GlobalStore 来源（02 §5.5）见 §4.2。
  2. 删掉 `DerivedDataCache` 和 `IDdcBackend`：已完成。`AssetPipeline/ConversionStore` 直接用 `GlobalStoreClient`：key 用 `GlobalStoreDomain` 的种子
     （`ConvertedResult`、`DependencyRecord`）；`ConvertMany` 先用 `GetMany` 一次取所有对象的依赖记录，再用一次 `GetMany` 只取各结果的头
     （要拿到逻辑位置，所以用读头代替 `HasMany`；只问有没有时用 `HasMany`），缺的才转换，先 `Put` 结果再 `Put` 依赖记录。
     Converter、`WarmGlobalStore`（原 `-Warm`）、`GlobalShaderConversion` 和 Packager 都已切过去；`Host` 为空时连本机、连不上就启动 `MarlinGlobalStore`，
     远端连不上时退回本机并警告，最多每 10 秒用单独的探测连接重试一次。`ReadResult` 给 Packager 取出解压后的块和 bulk；
     测试用 `ConversionStoreDesc::InProcess` 在进程里起一个服务器。
  3. 改名：`Runtime/DdcAssetStore` → `Runtime/GlobalStoreAssetStore`、`Runtime/DdcShaderStore` → `Runtime/GlobalStoreShaderStore`；`DdcInput` → `DependencyInput`、
     `DdcLocation` → `ConvertedResultLocation`、`WarmDdc` → `WarmGlobalStore`；`Mode` 已去掉；`/Cache` 的物理目录 `Marlin/DDC` 改为 `Marlin/Cache`：已完成。
  4. 服务器的零拷贝发送、异步冷读、分片锁和基准：已完成（见上面的「服务器 I/O」）；RIO 视基准结果再定。
- 尚未实现：认证；CI 流水线本身（每次提交后调用 `-Warm` 的作业）；段文件回收（现在只增不减）；构建映射（一次构建里所有 UUID → 结果 key 的映射存进 GlobalStore，
  一个 id 定位整个构建）。

### 3.4 纹理

**实现现状**（`Developer/TexturePipeline`，编码器模块 `Developer/{Bc7EncRdoEncoder,IspcTextureEncoder,AstcTextureEncoder}`）：

- **源对象** `TextureSource { Compression MipMaps MaxSize ColorSpace SourceFile }`。`Compression` 是纹理的**种类**（`Color`、`Normal`、`Mask`、
  `Grayscale`、`Hdr`、`Uncompressed`，可以自己加），不是格式；§1.1 示例中的 `Format = BC7` 实际写成 `Compression = Color`。
- **决策**：种类到格式的映射在 Config `Engine.Textures.Compression`（`TextureSettings`），每项 `{ Format Quality RdoLambda }`，
  平台层覆盖。`/Engine/Config/Engine.mtext` 是任何 GPU 都能读的非压缩格式（RGBA8 / RG8 / R8 / RGBA16Float）；Windows 层映射到
  BC7 / BC5 / BC7 / BC4 / BC6H / RGBA8。`Quality` 0 最快、1 最好，`RdoLambda` 大于 0 时用率失真换更小的包（编码器支持时）。
- **转换**（`TextureConverter`）：`Decode` 解出图像 → 转到线性空间（sRGB 颜色缩小时亮度正确）→ 按 `MaxSize` 减半 → 逐级生成 mip 直到 1×1
  （`MipMaps = false` 只留顶层）→ 每级按 GPU 读取的布局写进 bulk 数据，最大的一级在前（§4.1）。大的 mip 按块行切成条，在多个 job 上并行编码。
  sRGB 换算查表（`TextureColor`：解码 256 / 65536 项，编码比较 255 个舍入阈值），转线性、缩小和转 8 位按行在 job 上并行。
  `Engine.Textures` 是 Config 输入，所以平台映射一变只重转那个平台。
- **`TextureEncoder`** 是扩展点（13 §3）：每个编码库一个模块，fork 用 `Priority` / `Replaces` 加入或替换。转换器取某格式优先级最高的编码器，
  并把这个格式的**所有**编码器（类型和版本）记进依赖记录，升级一个编码器只重转用它那些格式的纹理。没有编码器写目标格式时报错
  「no TextureEncoder writes X; link an encoder module」。现有编码器（每个格式只有一个）：
  - `Bc7EncRdoEncoder`（bc7enc_rdo）：BC1、BC3、BC4、BC5、BC7，支持 `RdoLambda`。
  - `IspcTextureEncoder`（Intel ISPC Texture Compressor）：只做 BC6H（输入 RGBA32Float），`Quality` 分五档选 profile。
  - `AstcTextureEncoder`（Arm astcenc）：ASTC 4x4 / 6x6 / 8x8，给移动平台；目前没有平台配置用到它。
- **解码**：`PngDecoder` 支持所有颜色类型和位深、透明块、Adam7；16 位解成 RGBA16，其他解成 RGBA8；忽略 gamma 和色彩配置块。
  CRC 一次算 8 字节，只有一个 IDAT 时不拷贝，过滤在解压结果里原地还原、按行选过滤方式，8 位 RGBA / RGB 整行拷贝。
- **导入**（`TextureImporter`）：每个图像文件一个 `TextureSource`，种类看文件名后缀：`_N`/`_Normal` 是 Normal，`_M`/`_Mask`/`_ORM` 是 Mask，
  `_H`/`_Height` 是 Grayscale（三者线性），`.hdr`/`.exr` 是 Hdr，其他是 sRGB 的 Color。
- 尚未实现：HDR/EXR 解码器（导入器认它们，但没有 `ImageDecoder`）、立方体贴图、纹理数组和体纹理。

### 3.5 网格

**实现现状**（`Developer/MeshPipeline`，运行时类型 `Runtime/Mesh`，场景解码器模块 `Developer/{FbxSceneDecoder,GltfSceneDecoder}`）：

- **源对象** `MeshSource { SourceFile Mesh Scale BuildDag MergePartsByMaterial }`：`Mesh` 为场景中某个网格的名字（在它自己的空间里转换）；
  为空时合并场景节点放置的所有网格（场景空间），每个部件 × 材质一个 section，`MergePartsByMaterial` 时每个材质一个（见下面「整个文件导入成一个网格」）。
  计划加上 `Slots`。
  `Scale` 在解码器换算到米之后再乘。
  `BuildDag`（默认开）关掉时不建 DAG：只留源三角形的 cluster，照样分组（`BuildClusterLod` 只做第一次分组）、打包成页，格式不变；
  每个组都是终止组（深度 0，误差为最大浮点数），所以全部常驻（`RootPageCount` = 页数）、全部绘制，渲染器不用区分。
- **转换**（`MeshConverter`）：`Decode` 解出场景（`MeshSource` 指定了网格时只解这一个）→ 补上文件里缺的法线和 MikkTSpace 兼容的切线 →
  每个材质的三角形焊接 → 用 `ClusterLod` 建 **cluster DAG**：源三角形分成 cluster，逐级分组、锁住组边界简化，所以 DAG 的任意切面都没有裂缝 →
  组打包成页，最粗的在前。最多 4 套 UV。
  - 顶点：只有按 UV 生成切线（MikkTSpace 按角算）时才每个角一个顶点；其他情况用文件自己的顶点，缺法线时在源顶点上按 60° 折角分组，
    一组一个顶点、取面积加权的法线，只在硬边处拆开。面法线按 double 求长度归一化：毫米级三角形的叉积远小于 `Math::SmallNumber`。
    解码出的场景在写进 section 后立即释放。法线分组分两遍并行（先数每个顶点几组，前缀和后再填），结果与串行一致。
  - 焊接（`WeldVertices`）并行且与线程数无关：每个顶点算哈希，按哈希高位分桶（桶内保持顶点顺序），各桶各自找逐字节相同的顶点，
    每种保留顶点顺序里第一个用到的，幸存者按顶点顺序重新编号；没有三角形用的顶点丢掉。
  - 打包先串行排好每个组和 cluster 在哪一页、页内偏移和页在 bulk 里的位置，再并行写各 cluster 的顶点和三角形；页之间的填充是零。
  - **`ClusterLod`**（`Developer/ClusterLod`）：算法照 meshoptimizer demo 的 clusterlod.h（MIT，声明在源文件末尾），不含边界膨胀；
    底层的分 cluster、分组、简化仍调 meshoptimizer。每一级先串行分组、锁边界，各组的简化和重新分 cluster 在 job 上并行，再按组的顺序串行输出，
    所以结果和线程数无关。一百万个三角形以内与 `clodBuild` 逐字节一致（测试对比 group、cluster、索引和层级）；更大的网格先按空间填充曲线排序三角形，
    切成每段 26 万个三角形并行分 cluster，段与段交界处的 cluster 可能不满。锁边界也并行：一个位置被不止一个组用到就锁住，
    和顺序无关，所以各组用 `AtomicRef::CompareExchangeWeak` 抢占位置，别的组再碰到就标为共享。每个深度一棵 BVH 也在这里建（`BuildClusterLodHierarchy`）。
  - 性能（Zorah 最大的网格，3205 万三角形，9600X，单个转换进程）：61 s / 7.5 GiB → 17.9 s / 4.8 GiB，其中约 3.3 s 是进程启动和扫描
    （哈希 10 GB 的 `.bin`），约 2.3 s 是写进 GlobalStore。剩下的串行部分主要是 meshoptimizer 的单线程调用：位置去重
    （`generatePositionRemap`）约 0.7 s、三角形空间排序约 1 s、各级的 `partitionClusters`。
  - `MarlinConverter -Warm` 起 job 系统（所有逻辑核），转换器的 job 才真正并行；`WarmGlobalStore` 每次给线程分的对象数随总量缩小，
    对象少时每个线程也分得到活。
- **运行时数据**（`Mesh`，07 §3）：块里是 section、cluster（包围球、误差、背面锥、所在组/页、页内顶点和三角形区间）、组（DAG 节点，深度、简化后的包围和误差）、
  每个深度一棵的 BVH（8 叉，节点存子树最坏误差）和页表；bulk 里是页。页是流式加载的单位：cluster 记录（80 字节），然后每个 cluster 的顶点，
  最后每个三角形三个字节大小的 cluster 内索引。位置量化（Nanite 的做法）：整个 Mesh 一张 2 的幂步长的网格（`Mesh::PositionStep`，
  最大边长不超过 2^`PositionBits` 步，默认 18），所以 cluster 之间共享的顶点落在同一个格点上，不会有裂缝；每个 cluster 存自己在网格上的角点
  （`PositionMin`）和每个轴需要的位数（`PositionBits`），顶点是从低位起打包进 32 位字的偏移，后面多一个字的填充，任何顶点都能一次读两个字。
  位置后面是属性（`AttributeStride` 字节：法线和切线是八面体 snorm16，副切线符号在切线 y 的最低位；UV；可选 RGBA8 颜色）。
  光栅只读位置，所以按位置单独打包；精细的 cluster 一个顶点大约 4 字节，而原来是 12 字节的 float 再加交错的属性。
  材质的双面（glTF `doubleSided`）记在 section 上，也作为 cluster 的 `TwoSided` 标志（`PositionBits` 第 24 位）：
  单面 cluster 在光栅里剔除背面三角形，双面的连背面锥剔除也不做（UE 按材质的 Two Sided 处理）。FBX 默认单面。
  前 `RootPageCount` 页是最粗的几级，常驻；更细的页由渲染器按需流入（`BulkStreamer`，§4.1）。
- **Config** `Engine.Meshes`（`MeshSettings`）：`ClusterVertices`（≤ `Mesh::MaxClusterVertices` = 256）、`ClusterTriangles`（≤ `MaxClusterTriangles` = 128）、
  `PageSize`（默认 128 KiB，比它大的组单独一页）。平台层按 GPU 的 mesh shader 覆盖；`Config/` 里目前没有这个段，都用默认值。
- **场景解码器**：`FbxSceneDecoder`（ufbx，二进制和 ASCII FBX）和 `GltfSceneDecoder`（cgltf，`.glb`，以及缓冲区内嵌或在旁边单独文件里的 `.gltf`）。
  `GltfSceneDecoder` 不让 cgltf 读外部文件，而是在读某个 accessor 之前才给它的 bufferView 取数据：内存里的缓冲区（`.glb` 自带的、data URI）原地读，
  外部文件只读这个视图的那一段；`EXT_meshopt_compression` 的视图只读压缩数据那一段再解压（fallback 缓冲区从不读）。
  所以 `MeshSource` 指定了网格时只读这个网格的数据：Zorah（10 GB 的 `.bin`、2068 个网格）里最大的网格（3205 万三角形）读 139 MB、解码约 0.4 秒。
  `KHR_mesh_quantization` 的整数属性转成 float。文件必需（`extensionsRequired`）的扩展只接受它读的这两个，以及只影响它不读的内容的
  （`KHR_materials_*`、`KHR_texture_*`、`EXT_texture_*`、`KHR_lights_punctual`），其他（如 `KHR_draco_mesh_compression`、`EXT_mesh_gpu_instancing`）报错；
  只是用到（不是必需）的扩展按 glTF 的规则忽略，比如用了 `EXT_mesh_gpu_instancing` 的节点只放一次网格。`DecodeOutline` 不读任何缓冲区。
  **决策**：场景文件只是几何的来源。glTF 为通用交换塞进的东西，比如 PBR 材质和贴图（不论内嵌还是外部）、`EXT_mesh_gpu_instancing`，都不支持：
  引擎有自己的管线和材质系统，材质只取名字作为槽位，实例和摆放由 Marlin 自己的关卡负责。
  没有材质的部分共用一个 `Default` 材质；没名字的材质和网格叫 `Material{n}` / `Mesh{n}`，导入的子路径因此稳定。
- **导入**（`MeshImporter`）：SceneDecoder 能读的格式都接，生成什么由 `MeshImportOptions` 决定（下面）。
- **决策**（整个文件导入成一个网格，部件分别上材质；或者导入成 Prefab）：一个场景文件可能是一个**模型**（人物、带零件的道具），也可能是一组摆好的独立物体（如 Sandbox 的
  `Shapes.gltf`）。区别由导入选项决定（§3.1），对应 UE 的 Combine Static Meshes 和场景导入：
  - `MeshImportOptions { Combine = true }`（默认）：每个文件一个 `MeshSource`，`Mesh` 为空，按节点的世界变换合并（场景空间），子路径 `:Combined`，以文件命名。
  - `Combine = false`（导入成 Prefab）：每个网格一个 `MeshSource`（子路径是网格名，以文件命名，场景有多个网格时再加网格名），再加一个 `Prefab`
    （子路径 `:Prefab`，以文件命名），保留文件的节点层级：每个节点一个 `PrefabEntity`（子路径 `:Node/<部件名>`，名字是部件名），`Parent` 指向父节点的实体，
    `LocalTransform` 由节点的局部矩阵分解出平移、旋转、缩放（切变丢掉，镜像变成负的 X 缩放），放了网格的节点再加 `RenderMesh` 引用那个网格的 `MeshSource`。
    `Members` 列出所有实体。重新导入只追加：文件里新加的节点会生成实体，但已有的 `Prefab` 对象不改，所以要手动加进 `Members`。
    Sandbox 的 `Shapes.gltf` 属于这一种，已有的四个对象和关卡引用不变。之前文档说按节点摆放是另一个入口，现在改为这个选项（用户决定）。
  - 冒号开头的子路径不会和场景给网格起的名字撞上。
  - **槽位 = 部件 × 材质**：合并时 section 按（节点, 材质）分，而不只按材质，所以共用一个材质的手和脸也能分别上材质。`MeshSection` 加 `Part`（节点名），
    和 `Material` 一起是槽位的名字（编辑器里写作 `Hand/Skin`）；没名字的节点叫 `Node{n}`，重名的加序号。同一网格被两个节点放置时是两个槽位。
    指定材质一律按槽位名匹配，不按下标，所以重新导出、顺序变了也不会错位。指定了 `Mesh` 的 `MeshSource` 只有一个部件，仍按材质分。
  - 代价：每个 section 单独建 cluster DAG，部件之间互不简化，最粗一级每个部件至少剩一个 cluster。`MeshSource.MergePartsByMaterial = true`
    把同材质的部件合并回一个 section（UE 的按材质分槽）。
  - **默认材质**：`MeshSource.Slots` 按槽位名给每个槽位一个默认材质，转换后存进 `Mesh` 的 section；实体在 `RenderMesh` 上按槽位覆盖（07 §5.2）。
    已实现：`MeshSource.Slots = { Default = <…Toon#…> }` 按槽位名（`Part/Material`，没有部件时只是材质名）给默认材质，写进
    `MeshSection::DefaultMaterial`（软引用：渲染器自己加载要画的材质）；写了不存在的槽位名是错误，并列出已有的槽位。
  - **材质资产**（MarlinLanguage §1.6、07 §4.4）：`Material { Shader = </…/Rock:Project.Materials.Rock#id> Variant = { … } 参数… }`，
    `MaterialInstance : <Rock#uuid> { 参数… }` 用继承语法覆盖参数和变体，不能换 shader。转换器（`Developer/MaterialPipeline`）
    通过 shader 管线反射 struct（`ReflectMaterialShader`：字段的名字、类型、偏移、大小、默认值、`[Range]`、`[Color]`、贴图默认、
    变体开关、可能用到的自定义 BSDF），反射结果作为转换器输入记入 key，所以改了 struct 的材质会重新转换；未知参数、类型不符、
    超出 `[Range]` 都是错误，链接的 `#id` 必须是 struct 的 `[Id]`。
  - 部件名（`GetScenePartNames`）导入器和转换器共用，所以 Prefab 实体的名字和合并网格的槽位名一致。
  - 尚未实现：`MeshSource.Slots` 和 `RenderMesh` 的按槽位覆盖（等材质资产）。
- 尚未实现：骨骼网格（部件 × 材质的槽位规则沿用过去）。

### 3.6 字体

**实现现状**（`Developer/FontPipeline`，运行时类型 `Font` 在 `Runtime/UIText`，09 §2）：

- **源对象** `FontSource { SourceFile FaceIndex }`，`FaceIndex` 是 `.ttc` 集合里的第几个 face。
- **转换**（`FontConverter`）：只检查 sfnt 头（集合检查 face 数），把文件**原样**放进块（`Font::Data`）：HarfBuzz 在运行时直接读表，没有要预先烘焙的东西，也不预先光栅化。
- **导入**（`FontImporter`）：`.ttf`、`.otf`、`.ttc`，每个 face 一个 `FontSource`，以文件命名（集合再加 face 序号）。
- 引擎字体在 `/Engine/Content/Fonts/{Inter,NotoSans,JetBrainsMono}/`，每个 `.ttf` 旁边一个 `.masset`；`Engine.UIText.DefaultFonts` / `MonospaceFont` 链接其中几个。

### 3.6.1 图标

**实现现状**（`Developer/IconPipeline`，运行时类型 `Icon` 在 `Runtime/UIIcon`，09 §2）：

- **源对象** `IconSource { SourceFile }`，指向一个 `.svg` 文件。
- **转换**（`IconConverter`，`ParseSvgIcon`）：把 SVG 的一个子集**烘焙**成 UIPainter 能直接画的形状，运行时不再解析 SVG。
  - 元素：根 `svg`（`viewBox`，没有时用 `width`/`height`）、`g`（属性继承）、`path`（M L H V C S Q T A Z 全部命令，绝对和相对，参数可以省略命令字母重复）、
    `circle`、`ellipse`、`rect`（`rx`/`ry`）、`line`、`polyline`、`polygon`。
  - 属性：`fill`、`stroke`、`stroke-width`、`stroke-linecap`、`stroke-linejoin`、`stroke-miterlimit`、`fill-rule`、`opacity`、`fill-opacity`、
    `stroke-opacity`、`display`、`visibility`，写在 `style` 里也认；`transform`（`matrix`、`translate`、`scale`、`rotate`、`skewX`、`skewY`）直接乘进点，
    描边宽度乘以变换的平均缩放。组的 `opacity` 近似为乘到每个形状上。
  - 颜色：`currentColor`、`none`、`#rgb`、`#rgba`、`#rrggbb`、`#rrggbbaa`、`rgb()`/`rgba()` 和常用的命名颜色，sRGB 转成线性。
  - 圆弧按 SVG 实现说明（F.6.5）转成每段不超过 90° 的三次曲线；基本形状也都转成路径，所以 `Icon` 里只有 MoveTo / LineTo / QuadTo / CubicTo / Close。
  - 不支持的内容（渐变和图案、`use`、`text`、clip path、mask、虚线、嵌套 `svg` 的视口等）跳过或近似，每种记一条警告（`LogIconPipeline`）；
    路径数据写错时像浏览器一样画到出错处为止。只有不是 SVG、或者没有尺寸时才转换失败。
- **运行时数据**（`Icon`）：view box 的大小 `Size`，所有形状共用的 `Verbs`、`Points`（view box 单位，左上角为原点，Y 向下），
  每个 `IconShape` 是一次填充或描边：描边宽度、`UILineCap`、`UILineJoin`、`UIFillRule`，颜色是 `currentColor`（乘以形状的不透明度）或固定的线性颜色。
  同一个元素既填充又描边时，两个形状引用同一段路径。
- **导入**（`SvgImporter`）：每个 `.svg` 一个 `IconSource`，以文件命名。
- 引擎图标在 `/Engine/Content/Icons/Lucide/`（Lucide，ISC），每个 `.svg` 旁边一个 `.masset`，`SOURCE.txt` 记录版本和来源。

### 3.7 全局 shader

全局 shader（MarlinLanguage.md §1.6）不是 `.masset` 对象，按入口点定位，Guid 是 `GetGlobalShaderGuid(入口点)`，此外和资产走同一套 GlobalStore、加载和打包。

**实现现状**：

- **定位**：开发期 `ShaderLibrary` 向资产表要这个 Guid，`GlobalStoreShaderStore`（在 GlobalStore 资产存储之上）用 Indexer 的 `Shader.Locate` 按入口点定位，
  未命中时派给 worker 编译（§3.1）；key 见 §3.2。
- **预热**：`MarlinAssetIndexer` 启动后在后台为本机平台编译所有全局 shader（`AssetIndexService::WarmShaders`，非紧急任务），
  这样 Editor 或 Game 第一次 `Shader.Locate` 就能命中；进度在 `AssetIndex.Status` 的 `ShadersWarmed / ShadersToWarm` 里。
- **热重载**：源文件变化让定位结果失效，并通过 `Asset.Changed` 重载已加载的 shader（§6）。
- **打包**：`MarlinPackager` 总是把全局 shader 为目标平台的每个 profile 编译并打进包（`PackageDesc::GlobalShaders`），打包后的游戏从包里加载（§5）。

### 3.8 脚本模块

一个脚本包（`Engine`、`Project`）的字节码模块（MarlinScript §1.2、§11.1）和全局 shader 一样：不是 `.masset` 对象，Guid 是 `GetScriptModuleGuid(包名)`，
资产类型是 `ScriptModuleAsset`（`Package` 和 `Bytecode` = `WriteScriptModule` 的字节），此外走同一套 GlobalStore、加载和打包。

**实现现状**：

- **定位**：开发期 `GlobalStoreScriptStore`（在 GlobalStore 资产存储之上，Shipping 不链接）用 Indexer 的 `Script.Locate` 按包名定位，未命中时派给 worker
  编译（§3.1）；key 见 §3.2。编译失败时 Game 或 Editor 的日志里有完整的诊断（`LogScript` Error）。
- **空包**：没有 `.mscript` 文件的包（比如还没写脚本的项目）转换成 `Bytecode` 为空的模块，而不是失败，所以加载时不报警告。
- **失效**：任何 `.mscript` / `.mcommon` 文件变化（包括新建）都让所有脚本模块的定位结果失效，并推送 `Asset.Changed`（§6）。
- **加载**：`ScriptModuleLoaderSubsystem`（ScriptVM）在第一帧开始加载 `Engine` 和（有项目时）`Project` 的模块，按包的顺序加入 `ScriptVM`。
- **打包**：`MarlinPackager` 总是编译源集的每个包并打进包（`PackageDesc::ScriptModules`）；Shipping 只带 VM，不带编译器（08 §4）。
- 尚未实现：像 `WarmShaders` 那样的启动预热，所以第一次 `Script.Locate` 现场编译。

## 4. 运行时加载

- `Assets::Load<T>(Guid)` 返回 `AssetRef<T>`，加载是异步的，可以查询状态或登记完成回调；`LoadSync` 只允许在加载界面使用。`AssetSubsystem`（EngineSubsystem）在主循环里驱动资产表（§4.2）。
- 依赖由转换器写入块头部（依赖 Guid 列表），加载器递归地并行加载。
- 数据块格式：头部（类型、版本、依赖）+ 可以直接 mmap 或拷贝的结构，指针字段写成偏移量，加载后原地修正（fixup）；GPU 数据直接通过 DirectStorage 送到 GPU 内存。
- 打包后从 mpak 的目录（Guid → 块位置）读取；开发期经 Indexer 定位、直接从 GlobalStore 读取（GlobalStore streaming）。
  两者都是资产表的 `IAssetStore`：`PakAssetStore` 和 `GlobalStoreAssetStore`，见 §4.2。

### 4.1 数据块

**决策**（模块 `Runtime/Asset`）：每个对象转换后得到一个块，块是一段连续的字节，整体读入内存后原地修正，不再拷贝变长数据。

```
AssetBlockHeader   Magic "MABK"、格式版本、Guid、运行时类型 Id、布局哈希、各区的偏移和大小
Dependencies       Guid[]，AssetRef 在镜像里存的就是这里的序号
Image              对象字段的镜像（只有非 Transient 的反射字段；vptr 的位置是零）
Data               FrozenArray / FrozenString 的元素，按各自的对齐排列
Fixups             uint32[]：镜像和变长区里所有需要修正的指针位置（相对块起点），以及每个位置的类型（块内指针 / AssetRef）
Bulk               可选的大块数据区（GPU 数据、流式数据），不跟块一起读入；位置写在块头，由使用者单独发起读取（DirectStorage）
```

- **加载**：`TypeInfo::New()` 正常构造对象（虚表、`Transient` 字段的默认值都正常），再把镜像里的字段逐个拷进对象；块内存由对象持有，
  `FrozenArray` 指向块里的数据。所以只有对象的定长部分有一次拷贝，变长数据零拷贝。
- **修正**：块内指针 = 块起点 + 偏移；`AssetRef` = 依赖表第 N 个 Guid 的槽位（加引用计数）。修正表在转换时一次算好，加载时不需要遍历反射信息。
- **校验**：Magic、格式版本、类型 Id 能找到且是 `Asset` 子类、布局哈希一致、各区不越界；任何一项不对都按加载失败处理并记日志，不会崩溃。

**实现现状**（Bulk 流式读取，`BulkStreamer`）：

- Bulk 区不随块读入；`Assets::GetBulk(slot)` 给出已加载资产的 bulk 位置。`Texture` 的 bulk 是所有 mip（最大的在前），`Mesh` 的 bulk 是页（§3.5）。
- `BulkStreamer`（`AssetSubsystem` 持有）读 bulk 的一部分，比如纹理 mip 和网格页：`Request(slot, offset, size, priority, callback)`
  （或任意 `FileRegion`）进队列，按优先级从高到低启动（同优先级先来先启动），只要在途的读取不超过预算（`Engine.Assets.StreamingBytes` 默认 64 MiB、
  `StreamingRequests` 默认 16）；什么都没在读时总会启动一个，所以比预算大的请求也能推进。队列是带索引的二叉堆，
  `SetPriority`、`Cancel` 和启动一个请求都是对数时间，每帧调整几千个请求的优先级也不必扫描整个队列。
- 读取在 I/O 线程完成，`ProcessFrame` 在主线程把字节（`BulkBytes`，引用计数）交给回调；读取失败时回调拿到空。渲染器随相机移动用 `SetPriority`
  调整优先级、`Cancel` 掉不再需要的请求（在途的读完后丢弃，不回调）。`Flush` 等所有请求结束，关闭前调用。只能在主线程用。
- 尚未实现：DirectStorage 直接送进 GPU 内存（目前读进 CPU 内存，由使用者上传）。

### 4.2 资产表

- 每个 Guid 对应一个**槽位**（`AssetSlot`）：状态（未加载 / 加载中 / 已加载 / 失败）、对象指针、块内存、引用计数、依赖的槽位、等待者。
  `AssetRef<T>` 只存槽位指针，所以热重载时替换槽位里的对象，所有引用自动看到新对象。
- **来源**：`IAssetStore` 把 Guid 解析成 `FileRegion`（02 §5.5）；按优先级注册多个，先命中先用。mpak 目录是一个 store（Patch 优先级高于 Base），
  开发期的 GlobalStore 是另一个。资产表本身不知道文件格式和路径。
- **启动时的顺序**：资产库由各自的 Subsystem 在 `Initialize` 里注册（包、GlobalStore、全局 shader、脚本），和别的 Subsystem 谁先谁后不固定。
  所以 `AssetSubsystem` 从 `Initialize` 到 `Start` 之间 `Assets::HoldLocates()`：这段时间里的加载照收，但先不去问任何库，
  到 `Start`（所有 Subsystem 都初始化完）`ReleaseLocates()` 时再带着全部的库开始查找。在这段时间里同步等待一个资产会立刻失败并报错，而不是卡死。
- **流水线**：`Load<T>(guid, priority)` → 槽位变为加载中 → 查 store → `Vfs::ReadAsync(region)` → 读完后在 job 里校验、构造对象、
  对每个依赖发起加载 → 所有依赖都加载完 → 修正 → `Asset::PostLoad()` → 已加载。依赖失败时，依赖它的资产也失败。
- **线程**：读取完成在 I/O 线程，解析和构造在 job 里；完成回调和失败回调在主线程的帧开始时执行（`AssetSubsystem::BeginFrame`），
  回调里可以安全地访问引擎状态。`LoadSync` 阻塞当前线程直到完成，只允许在加载界面和工具里用。
- **引用**：加载中的槽位持有它依赖的槽位；加载完成后，引用只来自块本身：对象字段里的 `AssetRef` 由对象的析构释放，
  `FrozenArray` 里的 `AssetRef` 由块在释放时放掉。所以互相引用的资产只要放掉彼此的 `AssetRef` 就能被释放（引用计数本身不处理环）。
  互相引用的资产会各自完成加载，环中 `PostLoad` 的先后顺序不保证。
- **释放**：引用计数归零时，槽位进入释放队列，在帧末（`EndFrame`）的安全点销毁对象、释放块、放掉对依赖的引用；
  同一帧里又被引用的槽位不会被释放。

**实现现状**（加载流水线）：

- **提前加载依赖**：store 不读块就知道依赖时（包的依赖表，§5），在 `AssetLocation::Dependencies` 里给出；资产表在发起块的读取之前
  就开始加载这些依赖，依赖的依赖同样如此，所以包能给出的整个闭包同时开始读，不必每读完一层块才发现下一层。在同一线程上嵌套发起的加载排队，
  不递归，图再深也不耗栈。修正仍只用块自己的依赖表，提前加载只是让槽位早点开始。不知道依赖的 store（GlobalStore）给空。
- **每次加载的开销**：类型的布局哈希和镜像要拷贝的字段区间（非 `Transient` 字段，相邻的合并）在 `Assets::Initialize` 里算一次，
  加载只查表、按区间整段拷贝，不再遍历反射信息或比较元数据字符串；类型表加载期间不变，不加锁。一个块的依赖槽位在一次加锁里全部取得。
  store 列表是不可变的快照，增删 store 时整份换掉，每次加载只多一个引用。
- **环的检测**：块到了以后，对每个还在加载的依赖检查它是否经由加载中的槽位等着自己；检查给访问过的槽位打上本轮的代号，
  不为每次检查分配集合，没找到环的槽位在本轮里不再重复走。语义不变：环里的资产各自完成加载，`PostLoad` 先后不保证。
- `AssetRef::Get` 先比较确切类型，相同就不沿继承链查找。

**实现现状**（资产存储）：

- **包**：打包后的引擎启动时挂载可执行文件旁 `Paks/` 下的所有 `.mpak`（`Engine::GetPackages()`），`AssetSubsystem` 把每个包的资产目录包成
  `PakAssetStore`：二分查找目录，得到块区和 bulk 区，以及依赖表里列出的依赖（提前加载，见上）。优先级 Base 0、其他 chunk 100、
  `Patch_N` 1000 + N，所以补丁先命中；补丁里 `BlockSize` 为零的条目表示删除，这个 Guid 在下面的包里也找不到。
- **GlobalStore streaming**（`Runtime/GlobalStoreAssetStore`）：`GlobalStoreAssetStoreSubsystem` 在有项目时连上项目的 Indexer（没有就启动它，`LaunchIndexer`），
  加一个 `GlobalStoreAssetStore`：`Locate` 调 `Asset.Locate`，未命中时 Indexer 先转换，应答在 IO 线程到达，所以一次转换只耽误等它的资产。
  应答是 `ConvertedResultLocation { Key, BlockOffset, BlockSize, BulkOffset, BulkSize }`（逻辑偏移）；块区和 bulk 区都是来源为 GlobalStore 的 `FileRegion`（02 §5.5），
  `Vfs::ReadAsync` 交给 `GlobalStoreClient::GetAsync` 分段、流水线地读，本机不落盘，资产表和 `BulkStreamer` 不区分来源。
  主机 devkit 直接连工作室 GlobalStore。优先级 `Engine.GlobalStoreAssetStore.Priority` 默认 -100，低于所有包，挂载的包优先；
  `Enabled = false` 只从包加载，模拟打包后的开发版。模块只在非 Shipping 链接（`Launch.module.py`），Shipping 里没有这段代码。
  具体做法：`ConvertedResultLocation::Key` 是 32 位十六进制文本（`FormatResultKey`，高半在前）。打开一个结果（`OpenConvertedResult`）
  先 `Get` 值开头的 64 KiB：头、块表，小资产连同整个资产块都在里面，这些块解压后留在块缓存里，读块区不再往返；块表更大时再取一次。
  `GlobalStoreRegionSource` 把一次读取切成 1 MiB 的段，一起发出，按序到达后写进读取的缓冲区；`ConvertedResultCompression` 是结果的
  `IRegionCompression`，只取覆盖到的存储块，部分用到的块留在它的块缓存里（与包同一套）。连接取 `Engine.GlobalStore` 的 `Host`、`Port`
  （`Host` 空为本机：没人启动时自己拉起 `MarlinGlobalStore`，`GlobalStoreClient::ConnectLocal`，与 Indexer 共用）；`Host` 指向的服务器连不上时这次运行退回本机并警告，
  本机也连不上才只从包加载。没链接结果 codec 的进程视为未命中并记日志。
- 与上文不同：运行时退回本机后不再每 10 秒重试远端；`GetAsync` 的应答先进客户端自己的缓冲再拷进读取的缓冲区，
  每段多一次拷贝。
- **记录加载顺序**：`Engine.Assets.RecordLoadOrder = "/Saved/LoadOrder.txt"` 时，`AssetSubsystem` 记下本次运行开始加载的每个 Guid（一行一个），
  退出时写出，给 `MarlinPackager -LoadOrder` 用（§5）。

## 5. 打包

- `MarlinPackager`：从 `Product.StartupLevel` 和 `Product.AlwaysCook` 出发，计算依赖闭包 → 每个对象从 GlobalStore 取出转换结果（缺失的就转换）→ 按 Chunk 分组 → 写 `.mpak`。
- **`.mpak`**：头部、目录（Guid → 偏移/大小/压缩方式）、按 64 KiB 对齐的压缩块（Oodle，没有授权时用 zstd）、可选加密；
  按加载顺序（来自 profiling 记录）排列数据，提高 I/O 局部性。
- **Chunk**：Base、Language_*、DLC_*、Optional（高清材质包）。Chunk 的划分规则写在项目的 `Config/Packaging.mtext` 里。
- **补丁**：对上一个发行版本做内容差分，变化的对象写入 `Patch_N.mpak`，在 VFS 中优先级更高。
- 平台包装由 `Platforms/<P>/Build/Packager` 负责。

**实现现状**（`Developer/Packaging`、`Programs/MarlinPackager`，读取在 `Runtime/Vfs` 的 `PakFile`）：

- `MarlinPackager -Project=<.mproject> [-Output=<目录>] [-Previous=<上一版目录>] [-Server] [-LoadOrder=<文件>] [-Key=<文件>]`：
  根是 `Product.mtext` 的 `StartupLevel` 和 `AlwaysCook`（对象链接，或字符串表示整个目录）；扫描项目建索引，按索引的依赖闭包收集对象
  （没有转换器的对象跳过），经 GlobalStore 转换（多线程，每个线程一次取 16 个，对象用一次 `ConvertMany` 查），连同派生结果一起写包。默认输出到
  `<Project>/Saved/Packages/<平台>/Paks/`。
- **不变的包不重写**：每个包有一个指纹（块大小、加密和签名设置、按顺序的每个对象的 Uuid 和结果 key 及压缩格式、每个文件的路径和内容哈希），记在
  `<Project>/Intermediate/Packaging/` 下；指纹相同、包（和签名）还在时，这个包的对象不从 GlobalStore 读，包也不重写（`PackageResult::Kept`）。
  写包前先删掉指纹，写完、签完才记。补丁总是写。可执行文件和 DLL 大小、时间没变就不复制。
  尚未实现：包只变了一部分时复用没变的压缩块。
- **文件**：包里还带 `/Engine`、`/Project` 的 `Config` 目录，`/Engine` 的平台层（`Platforms/<层>/Config`）
  作为对应层的文件写入，所以打包后 Config 仍按层叠加（§1.4）。
- **格式**（`PakFormat.h`）：192 字节的 `PakHeader`（Magic `MPAK`、版本、`BlockSize`、codec 名、chunk 名、各表位置、cipher 名、`KeyId`、`Salt`、
  依赖表位置），然后是逻辑流：按 `BlockSize`（默认 64 KiB）切块，每块用包的 codec 单独压缩，压缩后不变小就原样存；最后是不压缩的块表、字符串表、
  文件表（挂载点、层、路径、标志、位置、内容哈希，排序）、资产表（UUID、块区、bulk 区、内容哈希、依赖表里的一段，按 UUID 排序）和依赖表
  （每个资产的块引用的 Guid，与块自己的依赖表同序，由打包器从块头读出；加载据此提前开始依赖，§4.2）。
  读取时 `Vfs::ReadAsync` 只读并解压覆盖到的块（`IRegionCompression`）；只用到一部分的块解压、校验后留在包的块缓存里
  （`RegionBlockCache`，32 块，最久未用的先丢），相邻的小对象共用一块时不再重复读和解压。一次读到很多块时分到各个 worker 上解压。
  加密的压缩块在读取自己的缓冲区里原地解密，不另拷一份。所有块都原样存、不加密也不签名的包（`ReadsDirectly`）给出的区域就是文件的一段，
  按确切偏移直接读，不按块走。签名的摘要把包头和各表一次拼进待哈希的消息，不先另拷一份。
- **排列**：资产在前、文件在后；资产按 `-LoadOrder` 文件（`AssetSubsystem::RecordLoadOrder` 写的，§4.2）的顺序排在最前面，其余按收集顺序。
- **Chunk**：`Engine.Packaging.Chunks` 的规则（`{ Name Paths }`），源路径在某个目录下的对象进那个 chunk，其他进 `Base`；派生结果跟随源对象的 chunk。
  包名是 chunk 名，压缩格式与 `Package` 不同的数据另成一个包 `<Chunk>.<格式>.mpak`（§5.1）。
- **补丁**：`-Previous` 读上一版的所有包（Base 先，然后按序号的 `Patch_*`），按内容哈希比较；只把变化的对象和文件写进 `Patch_NNNN`
  （序号自动加一），已删除的写删除条目（资产 `BlockSize = 0`、文件 `PakFileDeleted`），下面的包里也就不再可见。
- **安全**：包先写 `<名>.mpak.tmp` 再改名；`OutputDirectory` 必须是绝对路径，打包器从不删除任何东西，输出目录里不属于这次的 `.mpak` 只警告。
- 与上文设计不同：Chunk 规则在 `Engine.Packaging`（项目的 `Config/Engine.mtext`）里，不是单独的 `Packaging.mtext`；块是逻辑流上的 64 KiB 分块，
  不做文件内对齐；压缩格式不是固定的 Oodle/zstd，而是按数据种类配置（§5.1）。
- 尚未实现：平台包装（`Platforms/<P>/Build/Packager`）。

### 5.1 压缩

**决策**：`CompressionCodec` 是扩展点（13 §8），按名字选择；包头写 codec 名，读取的进程用链接进来的同名 codec 解压。每种格式一个模块，
target 规则只链接平台用到的：`Lz4Codec`（LZ4 1.10，解压最快，PC 平台用）、`ZstdCodec`（Zstd 1.5.7，包更小、解压慢一些，
也用于网络传输的数据，如 GlobalStore 里的转换结果，§3.3）。Oodle 或平台硬件解码器也可以作为模块加入。

**实现现状**：

- **压缩矩阵** `Engine.Packaging.Compression`（`PackagingSettings`，平台层覆盖）：键是运行时类型的 `MCLASS(PackageClass = …)`
  （`Package`、`Bulk`、`GpuBuffer`、`GpuTexture` 或类型自己起的名字；`Texture` 是 `GpuTexture`，`Mesh` 是 `GpuBuffer`），没写的和文件都按 `Package`；
  每格 `{ Format Level }`。每个平台写自己最快的格式。一个包只有一种格式：格式与 `Package` 不同的数据按 chunk 另成一个包。
- `BlockSize`：每块的逻辑字节数，0 为 `PakDefaultBlockSize`（64 KiB）；小块让小对象少读，大块压得更好。
- 块由 `PakWriter` 并行压缩（`MarlinPackager` 自己启动 job 系统，没有 subsystem 时也并行）；签名要的每块哈希在压缩时一起算，
  不再把包读回来。各对象的转换结果在 worker 上并行从 GlobalStore 读取（所有线程共用一个 `ConversionStore`，`PackageDesc::Store`，
  空则按 `Engine.GlobalStore` 连接），只读块区和 bulk 区，直接放进一个缓冲区交给 `PakWriter`，不再多拷；
  写包时块直接拼进输出，逻辑流压缩完就释放。矩阵里的格式没有对应的 codec 时打包失败。
- 当前 `Config/Engine.mtext` 和平台层里**没有** `Packaging` 段（丢失后尚未恢复），所以默认所有块原样存储，不压缩。

### 5.2 加密

**实现现状**：

- `Engine.Packaging.Encryption { Cipher KeyId }`：`Cipher` 为 `PakCipher` 的名字，空为不加密；`KeyId` 写进包头。**密钥从不放进 Config**：
  `MarlinPackager -Key=<文件>` 从只有构建机才有的文件读取（十六进制文本，忽略空白），配置了 `Cipher` 却没给 `-Key` 时打包失败。
- `PakCipher` 是扩展点，按名字选，必须保持长度（块表因此不变）。现有 `Aes256Ctr`：AES-256 计数器模式，走平台的加密库；计数器块是包的随机 `Salt`
  加上位置左移 32 位（大端），块用自己的序号作位置，各表整体用 `Pak*TablePosition` 作位置，所以同一密钥下不会有两段 16 字节共用计数器。
  加密的是压缩后存储的字节；包头不加密。
- 运行时由 `PakKeyProvider`（扩展点）按 `KeyId` 提供密钥，产品自己实现（从可执行文件里或平台的安全存储取）；`PakCrypto::AddKey` 可以直接给密钥。
  `PakFile::Open` 缺 cipher 或密钥时打不开这个包。

### 5.3 签名

**决策**：可选的包签名，防止有人把包放进 `Paks/` 来替换内容、Config 或脚本（chunk 名写在包里，一个自称 `Patch_999` 的包会盖过一切）。
做法与 UE 的 pak signing 类似：签名放在包旁边的 `<名>.msig`，包的格式不变。主机依赖平台自带的包签名，不用这一套。

**实现现状**：

- `Engine.Packaging.Signing { Scheme KeyId }`：`Scheme` 为 `PakSignatureScheme` 的名字，空为不签名；`KeyId` 写进 `.msig`。
  **私钥从不放进 Config**：`MarlinPackager -SigningKey=<文件>` 从只有构建机才有的文件读取（十六进制），配置了 `Scheme` 却没给时打包失败。
  `MarlinPackager -GenerateSigningKey=<文件> [-Scheme=EcdsaP256]` 生成密钥对：私钥写进该文件，公钥写进 `<文件>.pub`，两个文件都不能已存在。
- `.msig`（`PakFormat.h`）：`PakSignatureHeader`（Magic `MSIG`、scheme 名、`KeyId`、块数、签名），后面是每个块**存储字节**（压缩、加密之后）
  的 SHA-256。签名覆盖包头和各表的存储字节、去掉签名的 `.msig` 头和所有块哈希。所以挂载时只验证一次签名，不必读完整个包；
  之后每读一个块（`PakFile::Decompress`）就核对它的哈希，对不上则读取失败。签名过的包 `IsPlain` 为假，未压缩的块也要经过核对。
- `PakSignatureScheme` 是扩展点，按名字选。现有 `EcdsaP256`：ECDSA P-256，走平台的加密库（Windows 上是 BCrypt；其他 PC 平台尚未实现）。
- 运行时由 `PakVerifyKeyProvider`（扩展点）按 `KeyId` 提供公钥，产品把公钥编进可执行文件；`PakCrypto::AddPublicKey` 可以直接给公钥。
  **链接了任何 `PakVerifyKeyProvider`，打包形态就要求签名**（`PakCrypto::RequiresSignatures`）：Shipping 不挂载没有签名或签名用了未知密钥的包，
  Development 和 Profile 仍然挂载但会警告，方便测试旁加载的包。`.msig` 存在、scheme 和公钥都有，但签名不对时，`PakFile::Open` 直接打不开这个包。
- 打包器先写包再签名，`.msig` 同样先写 `.tmp` 再改名。不签名的构建遇到输出目录里已有的 `.msig` 会失败：旧签名对不上新包，游戏会拒绝它，
  而打包器从不删除文件。
- 这只能提高篡改的门槛：能改可执行文件的人也能去掉校验。

## 6. 热重载

| 对象 | 流程 |
| --- | --- |
| 资产 | Indexer 检测到变更 → 重新计算受影响的结果 key → Converter 转换 → 通过 Remote 层通知所有连接的进程（Editor、PIE、开发版 Game，包括主机）→ 资产表原子替换数据块，`AssetRef` 的持有者收到 `OnReloaded` |
| 脚本 | 08 §6 |
| Shader | 保存后由 Converter 重新编译 → PSO 缓存失效 → 下一帧重建 |
| Config | 文件监控 → 重新层叠 → 通知 `HotReload` 字段 |

**实现现状**（资产和全局 shader）：

- Indexer 记住每次定位的结果和它来自的文件；扫描到这些文件变化时丢掉对应结果，重新转换后在 `Asset.Changed { Uuids }` 流里推送结果 key 变了的对象（§2）。
  只推送已经被定位过的对象，没人订阅时不推送。读过 config 的结果（纹理、网格）也记住，config 文件变化时重新验证。
- `GlobalStoreAssetStoreSubsystem` 订阅 `Asset.Changed`（`EnableHotReload`；订阅失败时热重载关闭），对其中已加载的槽位调 `Assets::Reload`：
  重新 `Locate`（这时 Indexer 重新转换）并加载新块，旧对象在此期间照常使用；新对象加载完后，在下一帧开始时槽位切换到新对象，所有 `AssetRef`
  都看到它，`WatchReload` 登记的回调在主线程执行（即上表的 `OnReloaded`），旧对象在那一帧结束时释放。加载失败时保留旧对象。
- 全局 shader 同样经 `Asset.Changed` 重载，`ShaderLibrary` 重新建管线（§3.7）。
- 脚本模块同样经 `Asset.Changed` 重载（§3.8）：新模块替换 `ScriptVM` 里的旧模块，每个 World 在下一帧边界换一个新的 ScriptDomain（MarlinScript §12）。
- 与上表不同：通知只经 Indexer 的管道，连接 Indexer 的进程（本机的 Editor、开发版 Game）才收到；主机等远程进程尚未实现。
- 尚未实现：Config 的热重载（§1.4）。
