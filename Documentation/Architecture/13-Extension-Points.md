# 13 · 扩展点

模块怎样扩展引擎：引擎自己的模块、fork 的模块和项目都走同一套扩展点（扩展模块放在哪里、怎样被链接见 12 §3）。

## 1. 前提与目标

扩展是**静态链接进同一个 fork** 的普通模块，和引擎一起编译，由 target 规则决定链接哪些（12 §3）。所以：

- 不需要动态加载、ABI 稳定或版本协商。扩展点的接口变了，编译器会把所有实现都找出来，同一次提交里一起改。
- 「存在」就等于「链接了」：扩展模块的类型在反射注册阶段（01 §2 第 5 步）进入类型表，引擎通过反射发现它，**扩展不写任何注册代码**。

三条目标：

1. **没链接 = 零开销**：模块不链接，代码和数据都不存在。
2. **链接了 = 和引擎自己的代码一样快**：发现和排序只在启动时做一次；热路径上只剩一次虚调用，或者干脆是调度图里的一个普通节点。
3. **引擎这边写起来简单**：开一个扩展点就是声明一个抽象 `MCLASS`，调用方一两行代码，没有订阅、退订和生命周期管理。

**不做**：任意函数的 detour 或「每个函数前后都能插 hook」、全局可写的函数指针表、热路径上的多播委托、按模块名写 `if`。
扩展点是引擎作者**有意开放**的契约，不是事后打的洞。

## 2. 四种机制

| 机制 | 回答的问题 | 每次调用的开销 | 举例 |
| --- | --- | --- | --- |
| **Provider**（§3） | 「X 由谁来做」：按 key 选一个，或者按顺序用全部 | 启动时查好表，之后一次虚调用 | Importer、Decoder、Converter、压缩、版本控制、Capture 工具、PropertyDrawer、EditorMode、资产校验 |
| **Observer 接口**（§4） | 「X 发生了」：低频的生命周期通知，不能改变结果 | 遍历一个预先建好的指针数组 | 资产保存、项目打开、World 创建和销毁、PIE 开始和结束、打包完成 |
| **调度工作**（§5） | 每帧、每个元素都要做的事 | 和引擎自己的 System / Pass 完全一样 | ECS System 加 Phase / SystemSet、渲染 Pass 加 RenderStage |
| **数据贡献**（§6） | 不需要写代码的扩展 | 0 | Config 层、WorldProfile、Content、Shader、Command、CVar |

选哪一种只看一个问题：**扩展需要影响结果吗，多频繁？** 要选出实现或者参与计算 → Provider；只是知道发生了什么 → Observer；
每帧或每个元素 → 调度工作。

## 3. Provider

现在的 `AssetConverter`（06 §3）就是这种形式：抽象 `MCLASS` 基类，实现类通过 `Reflection::GetDerivedTypes` 被找到。
**决策**：把它推广成所有「选实现」类扩展点的统一做法。

### 3.1 声明扩展点（引擎这边）

```cpp
/// Creates source objects from a file the user imports: one per mesh, texture, animation… found in it.
MCLASS(Abstract, ExtensionPoint)
class ASSETPIPELINE_API AssetImporter
{
    MGENERATED_BODY()
public:
    AssetImporter() = default;
    virtual ~AssetImporter() = default;
    MARLIN_NONCOPYABLE(AssetImporter);

    /// File extensions without the dot, lower case.
    [[nodiscard]] virtual Span<const StringView> GetExtensions() const = 0;
    virtual bool Import(ImportContext &context) = 0;
};
```

`ExtensionPoint` 元数据只用于列举和文档（§8），不影响行为。

### 3.2 实现（扩展模块这边）

```cpp
MCLASS(Priority = 100)
class AcmeUsdImporter final : public AssetImporter
{
    MGENERATED_BODY()
public:
    MFUNCTION()
    static bool ShouldCreate(const ExtensionContext &context);   // 可选，同 Subsystem 的约定（01 §3）

    Span<const StringView> GetExtensions() const override;
    bool Import(ImportContext &context) override;
};
```

### 3.3 `ExtensionSet<T>`

**决策**：Core 提供 `Reflection/ExtensionSet.h`，由**扩展点的拥有者**持有，不是全局单例：

```cpp
class ImportSubsystem final : public EditorSubsystem
{
    ExtensionSet<AssetImporter> _importers;
    HashMap<String, AssetImporter *> _byExtension;
};

void ImportSubsystem::Initialize(SubsystemContext &context)
{
    _importers.Build(context);                      // 发现、过滤、排序、创建
    for (AssetImporter &importer : _importers)      // 按优先级从高到低
        for (StringView extension : importer.GetExtensions())
            _byExtension.TryAdd(String(extension), &importer);
}
```

- `Build`：收集 `T` 的非抽象子类 → 调用反射的 `ShouldCreate`（有的话）→ 去掉 Config `Engine.Extensions.Disabled` 里列出的类型 →
  按 `Priority`（默认 0）从高到低、再按类型名排序 → 用 `TypeInfo::New` 创建实例并持有。结果和启动顺序一样是确定的。
- 建好之后**不可变**，多线程并发读不需要锁；按 key 的查找表由拥有者自己建一次。
- 生命周期跟着拥有者走：拥有者是 Subsystem，扩展的生命周期就是这个 Subsystem 的；拥有者是 Program 里的工具对象（`MarlinConverter` 里的 `AssetConversion`），
  就不需要 Subsystem。这样不违反「生命周期归 Subsystem」（01 §3），因为扩展本身没有 Initialize / Shutdown。
- 扩展实例应当是无状态的（最多缓存 Config）。需要长期状态的扩展另写一个 Subsystem，在调用时通过 `Engine::GetSubsystem<T>()` 取得。

### 3.4 冲突、选择与替换

- **同一个 key 有多个实现**：`Priority` 高的胜出；`Priority` 相同时启动报错并列出双方，不静默按名字选一个。
- **按 key 指定**：扩展点的拥有者可以在自己的 Config 段里提供选择表，优先于 `Priority`：

  ```
  // Config/Engine.mtext
  AssetPipeline
  {
      Importers { fbx = UfbxImporter }
  }
  Extensions
  {
      Disabled = [FbxSdkImporter]
  }
  ```

- **替换并包装**：`MCLASS(Replaces = "TextureConverter")` 让一个模块接管引擎的某个实现，同时通过 `ExtensionSet::GetReplaced(*this)` 拿到被替换的那个实例，
  可以先做自己的处理再交给它。被替换的实现不再出现在遍历中。对 Converter 来说，结果 key 同时包含两者的 TypeId 和 Version（06 §3.2）。
- **引擎自己的实现也是普通 Provider**：`TextureConverter`、`PngDecoder` 和其他模块的实现地位相同，引擎代码里没有「内置路径」和「扩展路径」两套分支。

## 4. Observer 接口

**决策**：通知类的扩展点是一个 `MINTERFACE`，由 **Subsystem 实现**。引擎在 Subsystem 集合建好时，按接口把实现者整理成数组；发通知就是遍历这个数组。

```cpp
/// Called on the main thread after a source asset was written to disk.
MINTERFACE()
class IAssetSaveObserver
{
public:
    virtual void OnAssetSaved(const AssetSaveEvent &event) = 0;
};

// 扩展模块
MCLASS()
class AcmeNamingSubsystem final : public EditorSubsystem, public IAssetSaveObserver
{
    MGENERATED_BODY()
    void OnAssetSaved(const AssetSaveEvent &event) override;
};

// 引擎：发通知
Subsystems::Notify(&IAssetSaveObserver::OnAssetSaved, event);
```

- 实现者列表在 `SubsystemCollection` 创建或改变（World 切换 Profile，05 §1.1）时重建，按接口缓存；通知的开销是一次查表加遍历数组，不分配内存。
- 顺序就是 Subsystem 的初始化顺序（依赖拓扑序），确定且可以用 `Dependencies` 控制；Subsystem 被销毁时自动退出列表。
  所以没有订阅句柄，也就没有忘记退订、悬空回调这一类问题。
- 每个 Observer 接口在注释里写明调用线程，默认是主线程。
- **Observer 不能改变结果**。需要否决或修改的钩子（保存前校验、导入后处理）是 Provider，并在扩展点上写明组合规则：
  全部通过才算通过（`AssetValidator`），第一个处理的胜出（`AssetImporter`），或者按优先级依次处理（流水线）。
- 对某个具体对象的动态订阅（某个面板关心某个资产）不是扩展点，用编辑器的 Signal（09 §4.1）或 Remote 通知（10）。

## 5. 调度工作（热路径）

热路径上的扩展**不是回调，而是调度图里的节点**；没启用就不在图里，引擎代码里没有任何判断分支。

- **ECS**：模块定义 Component、`ISystem`、`SystemSet`，需要时定义自己的 Phase 插在锚点之间（05 §3）；WorldProfile 启用对应的 Set 才会被调度。
- **渲染**：**决策**：`RenderFeature` 是 Provider，在 `RenderStage` 上往 RenderGraph 添加 Pass。RenderStage 和 Phase 一样是开放注册的反射类型：

  ```cpp
  MCLASS(RenderStage, After = GBuffer, Before = Lighting) struct Decals {};

  MCLASS(Stage = PostProcessBeforeTonemap)
  class AcmeOutlineFeature final : public RenderFeature
  {
      MGENERATED_BODY()
  public:
      bool IsActive(const SceneView &view) const override;          // 每个 View 每帧一次
      void AddPasses(RenderGraphBuilder &graph, const SceneView &view) override;
  };
  ```

  引擎提供锚点 Stage（`DepthPrepass`、`GBuffer`、`Lighting`、`Translucency`、`PostProcessBeforeTonemap`、`PostProcessAfterTonemap`、`UI`）。
  开销是每个 Feature 每个 View 每帧一次虚调用；RenderGraph 会剔除没有被使用的 Pass（07 §2）。
- **批量接口**：扩展点永远按批传数据（chunk、`Span`、整个 View），不提供「每个实体」「每个三角形」的回调。

## 6. 数据贡献

不需要扩展点代码，已经由现有机制覆盖：平台层和项目的 Config 进入 Config 层叠（06 §1.4）、Content 被索引（06 §2）、Shader 可以被 import（08）、
WorldProfile 是资产、`Command`（09 §4.3）、`MFUNCTION(ConsoleCommand)` 和 CVar（03 §7）、`MFUNCTION(Remote)`（10）、`MFUNCTION(ScriptCallable)`（08）。

## 7. 场景：资产导入

### 7.1 管线里的三类扩展点

```
.psd / .fbx / .usd / .wav                                        运行在
   │  AssetImporter      文件 → Source 对象（.masset，链接到二进制）  Editor；MarlinConverter -Import（批量、CI）
   ▼
TextureSource / MeshSource / …
   │  AssetConverter     Source 对象 → 运行时块（进 GlobalStore）    MarlinConverter worker
   │    └─ SourceDecoder 读取二进制源文件：ImageDecoder / SceneDecoder / AudioDecoder
   ▼
Texture / Mesh / …
```

- **`SourceDecoder`**（已实现 `ImageDecoder`、`SceneDecoder`；`AudioDecoder` 尚未实现。模块 `FbxSceneDecoder`、`GltfSceneDecoder`）：把一种文件格式解码成引擎的中间表示（`DecodedImage`、`DecodedScene`、`DecodedAudio`）。按扩展名注册，必要时再看文件头（magic）。
  `SceneDecoder` 读源文件引用的其他文件（glTF 的外部缓冲区）只能经传入的 `ISourceFileReader`，转换时它们由此记进依赖记录（06 §3.1）。
  Converter 不直接读格式，而是调用 `context.Decode(binaryGuid, image)`：`ConversionContext` 选出 Decoder，并把它的 TypeId 和 Version **记进依赖记录**（06 §3.2），
  所以升级 Decoder 会让用它转换的结果失效，而不影响其他格式。
- **`AssetImporter`**：只负责「这个文件里有哪些东西，各生成一个什么 Source 对象、默认设置是什么」，生成的对象用二进制链接指向源文件（06 §1.1）。
  重型的解析放在 Converter 里，结果存在 GlobalStore 里。通用 Importer 从 Decoder 集合取扩展名：`TextureImporter` 接受所有 `ImageDecoder` 支持的格式。
- **`AssetConverter`**：已有（06 §3）。
- **`AssetValidator`**：按 Source 类型注册，在保存、提交前和打包时运行，全部通过才算通过（命名规范、贴图尺寸预算……）。

`ImportContext` 提供 `CreateObject<TSource>(subPath)`。重新导入时，对象的 UUID 由「Importer 类型 + 源文件 GUID + 文件内的子路径」决定，
所以再导入一次是更新已有对象（只写有差异的字段，06 §1.3），不会生成重复对象，引用也不会断。在编辑器里，导入的写入经过 `Transaction`（09 §4.2）。

**决策**（已实现）：`MarlinConverter` 的 target 规则在 `extra_modules` 里列出 codec、贴图编码器和场景解码器模块（12 §3），worker 通过反射找到它们。
Indexer 不链接这些扩展：它只索引所有类型共有的部分（06 §2），所以扩展的 bug 不会损坏索引；转换在独立的 worker 进程里，扩展崩溃也不会带走编辑器。

### 7.2 几种需求分别要写什么

| 需求 | 扩展模块里要写的 | 引擎改动 |
| --- | --- | --- |
| 支持 `.psd` 贴图 | 一个 `ImageDecoder` | 无；已有的 TextureImporter / TextureConverter 自动支持 |
| 支持 USD 场景 | 一个 `SceneDecoder`（可选：专门的 `AssetImporter` 处理 USD 的 variant） | 无；Mesh / Skeleton / Animation 的 Importer 和 Converter 复用 |
| 全新资产类型（如从 `.xlsx` 导入对话表） | Runtime 模块：运行时类型；Developer 模块：Source 类型、Converter、Importer；可选 Editor 模块：编辑上下文、缩略图 | 无 |
| 改变引擎对某种资产的处理 | `MCLASS(Replaces = "TextureConverter")` 的 Converter，内部调用被替换的实例 | 无 |
| 项目规范检查 | `AssetValidator` | 无 |

## 8. 其他扩展点一览

| 场景 | 扩展点 | 机制 |
| --- | --- | --- |
| 压缩、加密（Oodle、平台加密） | `CompressionCodec`、`PakCipher` | Provider，按名字选；已有 `Lz4Codec`、`ZstdCodec` 模块和 `Aes256Ctr` |
| 加密包的 key | `PakKeyProvider`（02 §5.6） | Provider，按 key id 问，第一个有的给出；key 不进 config |
| 贴图块编码（BC、ASTC） | `TextureEncoder`（06 §3.4） | Provider，按格式和 Priority，`Replaces` 替换；版本记进依赖记录；已有 `IspcTextureEncoder`、`Bc7EncRdoEncoder`、`AstcTextureEncoder` 模块 |
| 网络传输（自研可靠 UDP、KCP、Steam Networking Sockets、EOS P2P、主机中继） | `NetTransport`（14 §5.1） | Provider，按名字选，`Engine.Net.Transport` 指定默认 |
| 资产来源（网络文件服务器、mpak） | `IAssetStore`（06 §4.2） | Provider，按优先级 |
| 版本控制（Lore、SVN、Perforce） | `ISourceControl`（09 §7） | Provider，`ShouldCreate` 识别工作区，Config 可以指定一个 |
| GPU capture / crash 工具 | `IGpuCaptureTool`、`IGpuCrashTool`（07 §1.7） | Provider；capture 工具启动时选一个，crash 工具所有支持后端的都启用 |
| 低延迟 SDK、帧生成；包装设备的层 | `IRhiPresentHook`、`IRhiDeviceLayer`（07 §1.1） | Provider；present hook 全部启用，device layer 按 Priority 由近到远包装 |
| 调试叠加层的分区 | `DebugOverlaySection`（09 §3） | Provider，按 Priority 排序，无状态 |
| Profiler 后端、日志去向 | `ProfilerHooks`、Log sink（03 §7） | 由模块的 Subsystem 安装 |
| 在线服务（Steam、EOS、主机平台） | `IOnlineService` | Provider，按平台 Config 选 |
| Inspector 自定义 | `PropertyDrawer`（09 §4.4） | Provider，按类型或元数据 |
| 资产编辑器、缩略图 | `AssetEditor`、`ThumbnailRenderer` | Provider，按 Source 类型 |
| 编辑模式 | `EditorMode`（09 §6） | Provider |
| 打包步骤（签名、上传符号） | `PackageStep`；完成通知 `IPackageObserver` | Provider / Observer |
| 音频中间件（Wwise） | Subsystem + System + bank 的 Converter + Decoder | 组合 |
| 玩法系统 | Component + System + WorldSubsystem | 调度工作 |
| 渲染特效 | `RenderFrameFeature` + `MSTRUCT(RenderFrameStage, After, Before)`（07 §5） | 调度工作；feature 用 `MCLASS(Stage = …)` 挂在帧阶段上 |
| 场景渲染 pass（阴影、GI、超分、选中描边） | `SceneViewFeature` + `MSTRUCT(SceneViewStage, After, Before)`（07 §5.4） | 调度工作；对每个 SceneView 加 pass |
| 视口显示模式 | `ViewMode`（07 §5.4） | Provider；编辑器菜单从注册表生成 |
| 可渲染图元（网格、粒子、地形） | RenderScene 的 extract 提供者（07 §5.2） | Provider，往 GPU Scene 写实例 |

**工具支持**：所有扩展点和实现都在反射里，所以 Remote 方法 `Extensions.List` 可以列出每个 `ExtensionPoint`、它的实现、各自属于哪个模块、
每个 key 最后选中了谁；编辑器的扩展面板和 MCP 用的都是它。Development 构建启动时把有竞争的 key 的选择结果写进日志。

## 9. 给开扩展点的人

1. **有真实需求再开**：至少有两个实现，或者有明确的外部扩展需求。不要「什么都能插」，每个扩展点都是要维护的契约。
2. 按 §2 选机制；接口要窄、按批传数据、不暴露全局可写状态；注释写明调用线程、调用时机和组合规则。
3. 解析一次，缓存指针：热路径上不遍历 `ExtensionSet`。
4. 引擎的默认实现也写成普通的 Provider，和其他模块走同一条路。

## 10. 待定

- `ExtensionSet` 现在每次 `Build` 都遍历类型表；consoles 是 monolithic，可以由 toolkit 为每个 `ExtensionPoint` 生成常量表，等到启动时间成为问题再做。
