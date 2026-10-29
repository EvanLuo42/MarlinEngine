# 05 · World 与 ECS

## 1. World

`World` = 一个 ECS `EntityManager` + 该 World 的 `WorldSubsystem` 集合（调度器、PhysicsScene、RenderScene、ScriptDomain、NetDriver……）。

一个 World 可以同时被多个 `SceneView` 观察（07 §5），也就是共享同一个 World 和同一个 RenderScene。

### 1.1 WorldProfile 与 SystemSet：没有写死的 WorldMode

**决策**：World 本身没有模式枚举。某个 World 里跑哪些 System、建哪些 Subsystem、时钟怎么走，全部由一个
**数据驱动的 `WorldProfile`** 决定；System 和 Subsystem 则通过**开放的 `SystemSet`** 声明自己属于哪些集合。
编辑器新增一种模式（地形雕刻、动画录制、Sequencer、Simulate……）= 新写一个 Profile，需要时再加几个新的 Set，
**不改引擎核心代码**。其他模块和项目也用同样的方式扩展。

**SystemSet** 是一个反射标记类型，谁都可以定义（Runtime、Editor 模块、项目、脚本都行）：

```cpp
MSTRUCT(SystemSet) struct CoreSimulationSet {};     // Transform 传播、层级……任何 World 都需要
MSTRUCT(SystemSet) struct GameplaySet {};           // 玩法逻辑、脚本行为
MSTRUCT(SystemSet) struct PhysicsSet {};
MSTRUCT(SystemSet) struct PresentationSet {};       // 动画姿态、粒子、需要渲染的东西
MSTRUCT(SystemSet) struct NetServerSet {};  MSTRUCT(SystemSet) struct NetClientSet {};
// Editor 模块里（示例，尚未实现）：
MSTRUCT(SystemSet) struct EditorVisualizationSet {};   // gizmo、图标、包围盒可视化
MSTRUCT(SystemSet) struct TerrainSculptPreviewSet {};  // 某个编辑模式自己的 Set
```

一个 System 可以属于多个 Set（`Sets=(CoreSimulationSet)`）；没写 Set 的 System 默认属于 `GameplaySet`。
（代码里 Set 和 Phase 的类型名带 `Set` / `Phase` 后缀，标记用 `MSTRUCT`，见 `World/System.h`。）

**WorldProfile** 是 Config 里的一个段 `WorldProfiles.<Name>`，按名字查找；引擎自带的在 `Config/WorldProfiles.mtext`，
平台层、项目和用户在各自的 `WorldProfiles.mtext` 里覆盖或新增：

```
EditorEdit
{
    Sets      = ["CoreSimulationSet", "PresentationSet"]
    Subsystems { Exclude = ["NetDriver"] }         // 按类型名或 Set 名增删 WorldSubsystem（Include / Exclude）
    Clock     = Paused                             // Fixed | Variable | Paused | Manual（单步、录制）
    NetRole   = None                               // None | Standalone | Server | Client
    Rendering = true
}

EditorSimulate { Base = "EditorEdit"  Sets += ["PhysicsSet", "GameplaySet"]  Clock = Fixed }
```

（与早先设计的偏差：Profile 不是带 UUID 的对象，继承用 `Base = "<Name>"` 而不是 `: <Base#UUID>`；`Sets += / -=` 改的是 Base 链上的列表。）

引擎和编辑器自带的 Profile（都只是数据，可以覆盖或继承）：

| Profile | 用途 |
| --- | --- |
| `Game` | 打包游戏、开发版 Game、PIE |
| `Server` / `Client` | 专用服务器、多人 PIE 中的客户端 World（`Client` 继承 `Game`，加 `NetClientSet`） |
| `EditorEdit` | 编辑器里的关卡 |
| `EditorSimulate` | 只运行模拟，不生成玩家 |
| `AssetPreview` | 资产编辑上下文的预览 World；具体的上下文可以继承它，再加上自己的 Set |

- **运行时切换**：`world.SetProfile(profile)` 在帧边界生效。调度器重建 DAG，Subsystem 按差集创建或销毁（`Initialize` / `Deinitialize`）。
  比如 Edit 切到 Simulate 再切回来，用的是同一个 World，不用复制。
- **叠加**：一个编辑模式激活时，可以在当前 Profile 上临时叠加一组 Set（`world.PushSets({TerrainSculptPreview})`），模式退出时弹出。
  所以编辑模式不需要各自持有一份完整的 Profile。
- **Subsystem 也一样**：`WorldSubsystem` 可以用 `Sets=(...)` 声明归属，只在 Profile 启用了对应 Set 时才创建；
  更复杂的条件写在 `static bool ShouldCreate(const WorldContext&)` 里，查询 `context.HasSet<PhysicsSet>()` 或 `context.GetProfile()`。
- **Target 裁剪仍然靠链接**：Server 没有链接渲染模块，所以 `PresentationSet` 里属于渲染的 System 根本不存在。
  Profile 只能在「已链接的 System」里做选择。

**实现现状**（`Runtime/World`）：

- `World` = `EntityManager` + `ResourceStore` + `EventStore` + `SystemScheduler` + World 作用域的 `SubsystemCollection`；`WorldSubsystem` 有
  `BeginFrame` / `EndFrame` 钩子。`SetProfile`、`PushSets` / `PopSets` 记脏，在构造和 `BeginFrame` 里统一生效：解析 Set 名（没有链接的 Set 名只警告）、
  加上压栈的 Set、重建调度器（留下的 System 保留状态）、按差集更新 Subsystem。
- Subsystem 过滤：Profile 的 `Exclude`（类型名或 Set 名）优先，然后 `Include`，再看 Subsystem 自己的 `Sets=(…)`；没写 Sets 的 Subsystem 总是创建。
- 时钟：`Fixed` → `FixedUpdate` 跑 Fixed Stage、`Update` 跑 Variable Stage；`Variable` → 两个 Stage 都在 `Update` 里用帧间隔跑；`Paused` → 只跑 Variable，
  delta 为 0；`Manual` → 同 Paused，`World::Step(delta)` 跑一次 Fixed。
- `WorldProfile::Load(name, profile)` 沿 Base 链从根往下应用；段不存在或 Base 成环时警告并返回 false。段里没写的 `Clock` / `NetRole` / `Rendering`
  取 Fixed / None / true（临时决定，引擎自带的根 Profile 都显式写了这三项）。`Config/WorldProfiles.mtext` 自带 `Game`、`Server`、`Client`、
  `EditorEdit`、`EditorSimulate`、`AssetPreview`（继承 `EditorEdit`，`Clock = Variable`）；上面示例里的 `Subsystems { Exclude }` 没有写进 `EditorEdit`，
  编辑器的 Set 也还不存在。
- `WorldManagerSubsystem`（Engine 作用域，`Config="Engine.World"`，依赖 `AssetSubsystem`）持有进程的所有 World，按创建顺序驱动 `BeginFrame` →
  `FixedUpdate`（固定步长）→ `Update` → `EndFrame`（01 §4）。`FindProfile(name)` 按名字加载一次后缓存。`LoadStartupLevel` 为 true 时（`Config/Engine.mtext`
  只对 Game 和 Server 打开）用 `-Level=<UUID>`，否则用 `Product.StartupLevel`，在 `StartupProfile`（Game 为 `Game`，Server 为 `Server`）的新 World 里打开
  启动关卡（一个 `WorldPartition`，§6；在 `Start` 里打开，即 01 §2 第 10 步）；`OpenLevel(level, profile)` 等关卡资产加载完再建 World，交给它的 `WorldPartitionSubsystem` 流式加载。`Engine.mtext` 的注释里提到的 `-Map=` 尚未实现。
- 每个 World 的 ScriptDomain 由 `ScriptWorldSubsystem`（ScriptVM）持有（MarlinScript §9.5）。
- 尚未实现：PhysicsScene、RenderScene 这些 WorldSubsystem，编辑器的 Set 和编辑模式，以及 `SceneView` 观察 World（07 §5）。

## 2. Archetype ECS

- `Entity` = 32 位索引 + 32 位世代号。
- Archetype = 组件类型集合（按排序后的 `TypeId` 计算哈希）；数据存放在 **16 KiB 的 chunk** 中，每种组件一个 SoA 列，另外还有 `Entity` 列和每列的变更版本号。
- **Tag 组件**（零大小）只影响 archetype，不占列。
- **Shared 组件**：按值分组（例如 `RenderMesh`、`Material`），同值的实体放在同一个 chunk 里，方便批处理。
- **Chunk 组件**：每个 chunk 一份（例如 chunk 的包围盒）。
- **Enableable 组件**：用位掩码表示启用状态，切换时不移动实体。
- 结构变更（增删组件、创建或销毁实体）在 System 内只能通过 `CommandBuffer` 记录，在 Phase 之间的同步点批量执行（按 archetype 排序，减少移动）。
- 脚本定义的组件：布局由 VM 计算，注册为动态 `TypeInfo`，和 C++ 组件存放在同一种 chunk 里（08 §5）。

**实现现状**（`Runtime/World`：`Entity`、`Component`、`Archetype`、`EntityManager`、`CommandBuffer`）：

- 组件是标了 `MSTRUCT(Component)` 的反射类型，存储方式写在标记里：`MSTRUCT(Component, Tag | Shared | Chunk | Enableable)`。`Components::Find(type)`
  在第一次使用时注册（没标 `Component` 的类型返回 null），`ComponentId` 按注册顺序分配、不复用；`Components::RegisterDynamic` 注册运行时描述的类型（脚本组件用它，类型由 `Reflection::RegisterDynamicType` 注册，MarlinScript §11.2）。
  能否 memcpy 由反射字段推断（数字、枚举、Entity、它们组成的 struct 和定长数组）。
- Archetype 按 `TypeId` 排序组件、对排序后的 TypeId 求哈希，命中后再逐个比较；Archetype 按需创建、不回收，增删一个组件的迁移目标缓存在 Archetype 上。
- Chunk（16 KiB，64 字节对齐）布局：头、shared 值索引、chunk 组件、每列的版本号，然后是 Entity 列和每个组件列（Enableable 组件带每行一位的启用位）。
  行是紧凑的，删除时把最后一行移进来；有空位的 chunk 在空闲列表里。
- Shared 值按哈希加 `Reflection::Equals` 去重，按 chunk 引用计数；Shared 组件不能就地写，`SetComponent` 会把实体移到新值的 chunk。
- 版本号：`EntityManager` 从 1 开始，写入时盖上当前版本；调度器在每个 System 运行前 `AdvanceVersion`，每个 Phase 的同步点回放前再推进一次（回放和 System 之外的写入因此比任何 System 上次运行的版本新）；比较允许回绕。
  `Archetype::GetStructureVersion` 在它的 chunk 增删行时改变，缓存了指向 chunk 内部指针的代码据此判断是否失效。
- 槽位回收时世代号跳过 0（null）和 `Entity::DeferredGeneration`（`~0u`，CommandBuffer 和 EntityTemplate 里的延迟实体）。
- 结构锁：System 运行期间结构变更是错误。`CommandBuffer::CreateEntity` 返回延迟实体，回放时替换成真实实体，记录的组件值里的 Entity 字段也一起替换；
  命令按记录顺序回放（不按 archetype 排序），目标实体已不存在的命令跳过；同一实体上连续的 Add / Remove（含紧跟 `CreateEntity` 的，每个组件一次、没有 Shared 的 Add）合并成一次迁移。
- 空 chunk 每个（没有 Shared 组件的）archetype 留一个备用，不立即释放。
- 内部上限（临时决定）：最多 4096 种组件，一个 archetype 最多 8 个 Shared 组件。
- Shipping 下同样构建（只在断言里用的辅助函数标 `[[maybe_unused]]`）。

## 3. System 与调度

```cpp
MCLASS(Phase = PostPhysicsPhase, Sets = (CoreSimulationSet))
class TransformPropagationSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;   // builder.AddQuery(_attached)：登记 Query 和读写集合
    void Execute(SystemContext &context) override;
private:
    EntityQuery<Read<LocalTransform>, Read<Parent>, Write<WorldTransform>> _attached;
};
```

（代码与早先设计的偏差：Query 是 System 的成员，在 `Configure` 里登记，而不是 `Execute` 的参数。）

- Phase 也不写死：Phase 是反射类型 `MSTRUCT(Phase, Stage=Fixed, After=PostPhysicsPhase, Before=GameplayPhase)`，
  拓扑排序后得到每个 Stage 的执行顺序。引擎提供锚点 Phase：`Input → PrePhysics → Physics → PostPhysics → Gameplay → Net`（Fixed Stage），
  以及 `Camera → Animation → UI → Late`（Variable Stage）。其他模块或编辑模式可以在锚点之间插入自己的 Phase；
  一个 Phase 里如果没有任何被 Profile 启用的 System，整个 Phase 跳过，不产生同步点开销。
- Phase 内部：调度器从 Query 的 `Read` / `Write` 集合，加上 `[UpdateAfter(X)]` / `[UpdateBefore(X)]`，推导出 DAG；没有冲突的 System 并行执行，每个 System 内部再按 chunk 拆成 job。
- 变更过滤：`Changed<T>` 用列的版本号跳过没有改动的 chunk。
- 单例数据用 `Resource<T>`（每个 World 一份，也参与读写冲突分析）。
- 脚本 System 和 C++ System 平等参与调度（08 §5）。脚本中的 `[Kernel, System]` 由参数推导出 Query 和读写集合，按 chunk 自动并行，并在 CPU 上向量化执行（08 §5.4，推迟实现）。
  GPU 上的逐实体计算写成 `.mshader` 的 `[ForEachEntity]` compute 入口，由引擎自动绑定 `[GpuMirrored]` 组件列并生成 RenderGraph pass（08 §5.5）。

**实现现状**（`System`、`SystemScheduler`、`EntityQuery`、`Resources`）：

- `ISystem::Configure(SystemBuilder&)` 调用一次，用 `AddQuery`、`Read/WriteComponent`、`Read/WriteResource`（同时创建该 Resource）、`Read/WriteEvents`
  声明访问；`Execute(SystemContext&)` 每次 Phase 运行时调用。`MCLASS(Phase=…, Sets=(…), UpdateAfter=…, UpdateBefore=…)`，Phase 默认 `GameplayPhase`，Sets 默认 `GameplaySet`。
- 锚点 Phase：`InputPhase` … `NetPhase`（Fixed）、`CameraPhase` … `LatePhase`（Variable），每个 `After` 前一个。先排 Fixed Stage 再排 Variable，平局按名字。
  未知 Stage、跨 Stage 的顺序、成环、元数据里的名字找不到对应类型都是 `Fatal`；构造不出来的 System 报 Error 并跳过。
- Phase 内：`UpdateAfter` / `UpdateBefore` 给出拓扑序（平局按类型名），后面的 System 和前面的在组件、Resource、Events 上有读写冲突或被显式排序时依赖它，
  其余作为 job 并行。Phase 运行期间锁住结构，CommandBuffer 在 Phase 的同步点回放；没有 System 的 Phase 不产生开销。
- System 内按 chunk 并行是显式的：`EntityQuery::ParallelForEach`（`ForEach` 是串行）。`SystemContext::GetCommands(chunk)` 每个 chunk 序号一个
  CommandBuffer，在 System 主 buffer 之后按序号回放，所以结果与线程分配无关。
- Query 项：`Read<T>`、`Write<T>`、`Without<T>`、`WithAny<T>`、`Changed<T>`；未启用的 Enableable 组件算缺失。Query 缓存匹配的 archetype，增量收新建的。
  System 之外调用时所有 chunk 都算已改动。
- `Resource<T>` 实现为 `ResourceStore`（每 World 一份，首次使用时默认构造），通过 `SystemContext::GetResource<T>` 取。
- 运行时描述的 System 类型（脚本 System）没有 C++ 构造函数：`Systems::RegisterFactory(type, factory)` 登记工厂，调度器用它代替 `TypeInfo::New`，
  元数据照常读。脚本 System 见 MarlinScript §11.2：每个实体一次 VM 入口，脚本 System 之间经 `ScriptDomainAccess` 资源串行。
- 尚未实现：`[Kernel, System]`、`[ForEachEntity]`（08 §5）。

## 4. 易用层

**决策**：下面这些都做，前提是不显著影响性能。

| 层 | 内容 |
| --- | --- |
| `EntityRef` | `entity.Get<T>()`、`Add<T>()`、`Children()`、`Destroy()`。在 System 之外使用（工具、脚本、gameplay 逻辑），内部是查表加 CommandBuffer。 |
| 层级 | `Parent`、`Children`（内联小数组）、`LocalTransform`、`WorldTransform`；Transform 传播 System 按层级深度分批并行。 |
| Prefab | 见 §5 |
| 脚本行为组件 | `ScriptBehaviour`：继承它的脚本 class 可以写 `OnStart` / `OnFixedUpdate` / `OnUpdate` / `OnDestroy`。引擎按脚本类型分组，每组一次 VM 调用批量迭代，而不是每个实体一次虚调用。适合快速写玩法，热点逻辑再改写成 System。 |
| 事件 | `Events<T>`：每帧的双缓冲事件流（碰撞、伤害），由 System 读写。 |

**实现现状**（`EntityRef`、`Hierarchy`、`Resources`）：

- `EntityRef`（World + Entity）：`Get<T>` / `GetForWrite<T>` / `Has<T>` / `Add<T>` / `Set<T>` / `Remove<T>`、`Destroy`、`GetChildren`、`GetParent`、`SetParent`。
  读直接查表；改动在结构没锁时立即生效，System 运行期间走 `World::Defer`（线程安全，在当前 Stage 之后回放）。`Destroy` 不销毁子实体。
- 层级组件：`Parent`（写它或删掉它来挂接 / 脱离）、`Children`（`List<Entity, InlineAllocator<4>>`，只读，可能含刚销毁的实体）、`PreviousParent`、
  `LocalTransform`、`WorldTransform`。`HierarchySystem` 按 `Parent` 维护 `Children` 和 `PreviousParent`（新 `Children` 通过 CommandBuffer 添加）。
- `TransformPropagationSystem`：根实体按 chunk 并行（直接拷贝 `LocalTransform`），挂接的实体按层级深度逐层、每层并行；只写有变化的：
  根实体在 `LocalTransform` 列变了的 chunk 里，挂接的实体在自己的 `LocalTransform` / `Parent` 列变了或父实体被写时，所以没动的 chunk 不盖新版本，
  `Changed<WorldTransform>` 跳过它们。有 `LocalTransform` 没有 `WorldTransform` 的实体（模板实例化出来的都是这样）由它通过 CommandBuffer 补上 `WorldTransform`。分层结果跨帧保留，只在 `Parent` 被写或它指向的 archetype 结构变化时重建；`WorldTransform::Combine`：
  平移 = 父平移 + 父旋转作用于（父缩放 × float 的局部偏移），旋转和缩放相乘。层级深度上限 256（临时决定）。两个 System 都在 `PostPhysicsPhase` +
  `CoreSimulationSet`，互不冲突，并行执行。
- `Events<T>`：发送线程安全；`ForEach` 先访问上一帧再访问本帧；`EventStore::Update` 每帧在 `World::BeginFrame` 里推进一次。
- 尚未实现：`ScriptBehaviour`。

## 5. Prefab

**决策**：Prefab 有独立的编辑上下文，也支持在关卡里就地编辑。

- Prefab 资产（`.masset`）= 一组实体加上组件的 Source 值，可以嵌套其他 Prefab。
- 实例 = 引用 Prefab 的 UUID，再加上 **override 列表**（MarlinText 的继承语法 `: <Base#UUID> { 只写差异 }`，06 §1.2）。
- 在 Asset Browser 中双击 Prefab → 打开 **Prefab 编辑上下文**：一个 `Preview` World 加上自己的 SceneView。
- 在关卡中选中实例 → 「就地编辑」：当前上下文临时切换到该 Prefab，其余实体灰显，保存会写回 Prefab 资产。
- 在关卡里直接修改实例 → 改动保存为 override；属性面板会高亮被 override 的字段，可以 `Apply to Prefab` 或 `Revert`。
- 转换时 Prefab 被展平：运行时没有「Prefab」这个概念，只有实例化模板（预先构建好的 archetype chunk，可以批量 memcpy 实例化）。
- 关卡本身就是一个顶层 Prefab，所以 Level 和 Prefab 共用同一套序列化与 override 机制。

**实现现状**（`Runtime/World` 的 `EntityTemplate`，`Developer/WorldPipeline`）：

- 源格式（`PrefabSource.h`）：`Prefab { Members = [...] }`，成员是 `PrefabEntity`（`Parent`、`Grid`、`Components = { 类型 = { 字段 } }`）或嵌套的 `Prefab`。
  嵌套实例写成 `Prefab : <#Base> { Parent = … Overrides = [...] }`：继承 Base 的 Members，根挂到实例的 `Parent` 下；override 是单独的 `PrefabOverride` 对象
  （`Instance`、`Target` = Base 里实体的 UUID、`Components`、`RemoveComponents`），而不是写在实例对象里的差异。
- UUID 稳定：实例里的实体是 `PrefabIds::Derive(instance, entity)`，override 自己的 UUID 必须等于 `Derive(Instance, Target)`；不对应源对象的部分（关卡的 Cell）用
  `PrefabIds::DeriveNamed(owner, name)`。`Parent`、`Children`、`PreviousParent`、`WorldTransform` 是计算出来的，不能当组件写。读到的每个对象都登记为转换输入。
- 临时决定：嵌套、继承链的深度上限 32；一个展平后的 Prefab 里同一 UUID 出现两次是错误；错误格式 `<Type> <Name>: message`。
- `PrefabFlattener` 展平后在临时 `EntityManager` 里建实体，`PrefabConverter`（源类型 `Prefab`）用 `EntityTemplate::Capture` 产出 `EntityTemplate` 资产。
- `EntityTemplate`：按 archetype 和 Shared 值分组（`EntityTemplateGroup`），每个组件一列原始字节（`EntityTemplateColumn`，带组件的布局哈希，不匹配时拒绝）。
  Entity 字段变成 `{模板索引, DeferredGeneration}`，指向模板外的变成 null；强 `AssetRef` 在字节里清零并记在 `Links`，软引用留在字节里；chunk 组件不捕获。
  只能存纯数据（数字、枚举、它们组成的 struct 和定长数组、Entity、资产引用），否则报 `"<Type>.<Field> is a <Type>, which an entity template cannot hold"`。
  `Instantiate` 先建好所有组的实体，再按连续行段整段拷贝列。
- 尚未实现：Prefab 编辑上下文、就地编辑、override 高亮和 `Apply to Prefab` / `Revert`（09）。

## 6. 关卡与世界规模

**决策**：从小场景、箱庭到开放世界都要支持，各项功能可以按关卡单独开关。

- **关卡子层（Layer）**：一个 Level 由多个 Layer 组成（Art、Design、Lighting、Audio……），每个 Layer 是独立的文件，方便分工，降低锁冲突。
- **一实体一文件（可选）**：开启后，Layer 下的每个实体（或每个顶层 Prefab 实例）存成单独的 `.masset`，放在 `<Level>.Entities/` 目录下。
  适合多人同时编辑开放世界；小关卡可以关掉这个选项。
- **网格分区流式加载（可选）**：开启后，实体按位置自动归入 Cell（可以配置多级网格：地形和大物件用大格子，细节物件用小格子），
  由 Streaming Source（玩家、相机）按距离加载和卸载。Cell 在转换时生成独立的数据块，可以独立做异步 I/O。
  不开启时整个关卡一次性加载。
- **大坐标**：`WorldTransform` 中的平移使用 `double`；渲染提取时拆成整数 tile 坐标加 float 偏移，每个 SceneView 在 shader 里减去自己相机的 tile（07 §5.2）；
  物理使用以 Cell 为单位的局部原点（rebasing）。
  小世界同样使用这套逻辑，不额外区分。

**实现现状**（`Developer/WorldPipeline` 的 `LevelConverter`，`Runtime/World` 的 `WorldPartition`）：

- `LevelConverter`（源类型 `Level`）：Level 是带 `Partition = { Enabled = true Grids = [{ Name = "Detail" CellSize = 64 LoadingRange = 128 }] }` 的顶层 Prefab，
  转换成 `WorldPartition` 资产：`AlwaysLoaded` 加上每级网格（`WorldPartitionGrid`：名字、`CellSize`、`LoadingRange`，单位米）的 Cell（`WorldPartitionCell`：整数坐标）。
  每棵层级树按根放置：开了分区时放进根的 `Grid` 指定的网格（默认第一个）里包含根 `LocalTransform` 的 Cell；没开分区、没有网格或根没有 `LocalTransform` 时放进
  `AlwaysLoaded`。每个 Cell 和 `AlwaysLoaded` 各是一个独立的 `EntityTemplate`（派生结果，UUID 为 `DeriveNamed(level, "Cell/<grid>/<x>/<y>/<z>")` /
  `(level, "AlwaysLoaded")`），可以独立异步加载；跨 Cell 的实体引用变成 null。网格默认 128 m 的 Cell、256 m 的加载距离（临时决定）。
- `WorldPartitionSubsystem`（`Config="Engine.WorldPartition"`，没有 Sets，所有 World 都有，没有分区时空闲）：`SetPartition` 后从下一帧起加载并实例化 `AlwaysLoaded`，同时按
  `StreamingSource` 组件（位置取 `WorldTransform`，`RangeScale` 乘到每级网格的 `LoadingRange` 上）异步加载附近的 Cell，加载完就实例化，超出范围的销毁其实体。
  `UnloadRangeFactor`（默认 1.25，临时决定）是卸载的滞后倍数。加载失败的 Cell 记错误日志并当作已加载的空 Cell，不会每帧重试。
- 大坐标：`LocalTransform` 和 `WorldTransform` 的平移是 `WorldPosition`（double）；子实体的局部偏移按 float 参与组合。
- 尚未实现：关卡子层（Layer）、一实体一文件，渲染提取时的 tile 拆分和物理 rebasing（还没有 RenderScene 和物理）。

## 7. 网络复制

**决策**：服务器权威，ECS 快照增量同步。

- 用 `MPROPERTY(Replicated)` 标记字段，可选 `Quantize=`、`Condition=OwnerOnly|InitialOnly`。
- 每个连接维护一份实体相关性和优先级（距离、可见性、上次发送时间），每个 FixedUpdate tick 生成快照，对上一个已确认的基线做差分并按位压缩。
- 客户端预测与回滚：被预测的组件存进环形历史缓冲区，收到权威状态后回滚并重放（依赖固定步长和确定性的 System 子集）。
- RPC：用 `MFUNCTION(Server)` 或 `MFUNCTION(Client)` 标记，脚本中也可以声明（08）。
- 传输层：复制通过 Net 的传输扩展点 `NetTransport`（14 §5.1）建立会话，快照走 `UnreliableSequenced` 通道，RPC 走 `ReliableOrdered` 通道。
  默认传输是自研的可靠 UDP（带通道、分片、加密，建在 `NetDatagramSocket` 上）；KCP、Steam Networking Sockets、EOS P2P、主机平台中继以各自的传输模块提供。

**实现现状**（`Runtime/Replication`，依赖 Core、Engine、Net、World）：

- **Schema**（`ReplicationSchema`）：反射里每个带 `MPROPERTY(Replicated…)` 字段的 `MSTRUCT(Component)` 展平成叶子：bool、有符号 / 无符号整数（枚举按符号）、
  float32 / float64、量化浮点（`Quantize=` 步数，四舍六入五成双）、Entity（按网络 id 传）；可以嵌套 struct 和定长数组，其他字段记日志后丢弃。
  `Condition=OwnerOnly|InitialOnly` 是标志位。`Defaults` 是默认值的叶子，作为新实体的基线。组件的 `NetIndex` = 在所有复制组件里按类型名排序的位置，
  两端链接同一组复制组件就一致，不需要握手（临时决定；构建不一致时要换成连接时交换 schema）。
- 组件：`NetworkIdentity`（服务器第一次看到时分配网络 id，从 1 开始）、`NetOwner`（连接 id，0 表示无）、`NetRelevancy`（`Range` 米，0 表示不限；`Priority`）。
- **服务器**（`ServerReplicator`）：每个连接一个观察者实体，按它的 `WorldTransform` 算距离。拥有的实体总是相关，超出 `Range` 的跳过。
  优先级每 tick 累加 `NetRelevancy.Priority`（没有时为 1），发送或没有变化时清零，平局按网络 id。对该连接最后确认的快照做差分（新实体对 `Defaults`），
  写出离开相关范围或已销毁的实体。OwnerOnly 叶子对非拥有者发默认值，InitialOnly 叶子在实体到达客户端后不再更新。
  预算内放不下的实体跳过，让后面小的还能发；队首实体总是发送。每个连接保留 `NetSnapshotHistory` = 32 份快照，只接受历史内的 tick 的确认。
  `Update` 每 tick 为所有连接按 chunk 采集一次（只读上次之后被写过的 chunk，用自己推进的版本号判断），各连接只在此之上做相关性和差分，可以并行写（`NetDriver` 用 `Jobs::ParallelFor`）。
  快照是按网络 id 排序的条目，指向引用计数、可复用的状态块：从基线沿用的实体共享基线的块，不再整份复制。
- **线格式**（`NetBitStream`）：位流，LSB 在前；变长整数是 LEB128，有符号数用 zigzag；每个实体、每个组件前一个继续位，每个叶子一个改动位。读越界给 0 并置 `HasOverflowed`。
- **客户端**（`ClientReplicator`）：用自己保存的基线解码（基线已丢弃时丢掉该快照），不比上一 tick 新的快照忽略，含未知 `NetIndex` 的整包拒绝。
  应用顺序：先建实体再写值（实体之间的引用可以解析），再删掉消失的组件、销毁移除的实体，然后回一个确认。
- **NetDriver**（`WorldSubsystem`，`Config="Engine.Replication"`，依赖 `NetSubsystem`，`Sets=(NetServerSet, NetClientSet)`）：`Listen` / `Connect` 走指定的或
  `Engine.Net.Transport` 默认的 `NetTransport`（服务器 `AllowRemote = true`）。IO 线程只把消息和关闭标记排队，`BeginFrame` 里分配连接 id、通过 World 的 Events 发
  `NetConnectionEvent`、把消息交给 Replicator、发客户端确认。通道 0 是 `UnreliableSequenced`（快照和确认），通道 1 是 `ReliableOrdered`（留给 RPC）。
  `NetSnapshotSystem`（`NetPhase`，`NetServerSet`）每个 fixed tick 调 `SendSnapshots`，预算 = `min(SnapshotBudget, 会话最大消息)`，`SnapshotBudget` 默认 1200 字节。
- 以上编号、优先级、预算、历史长度等都是重建时的临时决定。`NetDriver` 和 `NetSnapshotSystem` 没有测试。
- 尚未实现：客户端预测与回滚、RPC（`MFUNCTION(Server)` / `MFUNCTION(Client)`）、脚本里的复制声明、按 `NetSession::GetStats()` 做带宽分配，
  以及引擎自己的可靠 UDP 传输（14 §7）。
