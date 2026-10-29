# 04 · 反射与代码生成

## 1. 标记写法

**决策**：用宏做标记，由自研的 C++ 解析器（`MarlinHeaderTool`）扫描，生成 `.gen.h` 和 `.gen.cpp`。

```cpp
#include "Physics/RigidBody.gen.h"   // 必须是最后一个 include

MSTRUCT(Component)
struct RigidBody
{
    MGENERATED_BODY()

    MPROPERTY(Replicated, Range=(0, 10000), Unit=Kilogram)
    float Mass = 1.0f;

    MPROPERTY(EditorCategory="Damping")
    float LinearDamping = 0.05f;
};

MENUM()
enum class MotionType : uint8 { Static, Kinematic, Dynamic };

MCLASS()
class Texture final : public Asset
{
    MGENERATED_BODY()
public:
    MFUNCTION(ScriptCallable, Remote)
    Vector2 GetSize() const;
};
```

| 宏 | 作用 |
| --- | --- |
| `MCLASS` / `MSTRUCT` / `MENUM` / `MINTERFACE` | 声明类型 |
| `MPROPERTY` | 字段：序列化、编辑器、复制、脚本可见性 |
| `MFUNCTION` | 方法：`ScriptCallable`、`Remote`（10）、`ConsoleCommand`、`Exec` |
| `MGENERATED_BODY` | 插入生成的友元、`StaticType()`、序列化入口 |

元数据（`Range`、`Unit`、`EditorCategory`、`Tooltip`……）**只存在于类型元数据里，不占对象内存**。
编辑器专用的元数据和运行时的元数据放在同一份描述中，但只有 Editor 才会读取。

**决策**（实现）：

- `MSTRUCT` 是值类型；`MCLASS` 是多态类，对象通过虚函数 `GetType()` 知道自己的动态类型，必须有虚析构；`MINTERFACE` 是可被类实现的抽象基类，不能有反射字段。
- `MGENERATED_BODY()` 必须是类体的第一项；`.gen.h` 必须是头文件的最后一个 include（`.clang-format` 已按此排序）。
- 反射父类必须是第一个基类（偏移为 0），其余基类中的 `MINTERFACE` 记为实现的接口；不支持虚继承。
- `MENUM` 需要固定底层类型（scoped enum 可省略），因为 `.gen.h` 要在枚举定义之前前向声明它。
- 不支持：类模板、嵌套的反射类型、匿名命名空间、位域、指针和引用字段、一个 `MPROPERTY` 声明多个字段、重载的 `MFUNCTION`、运算符。HeaderTool 对这些直接报错。
- `static` 字段可以是 `MPROPERTY`（CVar 就是这样，03 §7）；`MFUNCTION` 可以是静态、const 或虚函数。
- 字段或函数前的 `///` 注释的第一段（去掉 Markdown 标记）自动成为 `Tooltip` 元数据，完整文档由 `marlin doc` 抽取（16 §1.1）；值是字符串字面量时存去掉引号后的内容，否则原样保存（`(0, 10000)`、`Cheat|Scalability`）。
- `MSTRUCT(Tuple)` 在 MarlinText 中写成 `(x, y, z)`；有 `FormatTo` 和 `TryParse` 且没有反射字段的类型（`Guid`、`DateTime`）写成字符串。
  Core 的 `Vector2/3/4`、`IntVector2/3`、`Quaternion`、`LinearColor`、`Color32`、`WorldPosition` 是 Tuple，`Transform`、`Box`、`Sphere`、`Plane`、`Ray` 是普通 MSTRUCT。

## 2. MarlinHeaderTool

- C++ Program（无 std 限制）。用一个快速分词器，只理解受限的语法子集：带标记的声明、模板参数、命名空间、默认值表达式（原样保留）。
  遇到看不懂的语法就报错，绝不猜测。
- 由 `marlin build` 在编译前调用：按模块并行处理，只对改动过的头文件增量生成，输出到
  `Intermediate/Generated/<Platform>/<Module>/Xxx.gen.h|.gen.cpp`。
- 每个模块额外生成 `Module.gen.cpp`，其中包含 `Register_<Module>()`。toolkit 再生成全局的 `MarlinModuleTable[]`（按依赖拓扑排序）；
  monolithic 和 modular 两种链接方式都由 `LaunchMain` 显式遍历这张表 → **不依赖静态构造器**。
- 生成内容：`TypeInfo`（布局、字段偏移、属性、方法 thunk）、序列化（MarlinText 和二进制两种）、脚本绑定 thunk、Remote 方法注册、
  `Replicated` 字段的差分编码表，以及脚本侧的声明文件 `<Module>.gen.mscript`（给 LSP 用，08）。

**实现现状**（与上面的偏差）：

- HeaderTool 是**只依赖 std 的独立程序**，不链接 Core，所以 Core 自己也能有反射类型。toolkit 总是为主机构建它的 Development 版本，不受 generate 过滤条件影响。
- 输出在 `Intermediate/Generated/<Platform>/<Arch>/<Module>/`（按架构区分，因为架构层也能覆盖头文件）：`Include/` 下是 `.gen.h`，加入该模块和所有依赖它的模块的 include 路径；
  `Source/` 下是每个头文件的 `<FileId>.gen.cpp` 和 `Module.gen.cpp`，单独编译（不进 unity，不用 PCH）。
- 每个平台、架构、模块一个 FASTBuild `Exec` 节点，模块或其依赖的源码编译前先运行；头文件或源码变化、或 HeaderTool 本身变化时重跑，只重写内容变化的输出，并删除不再生成的文件。
  `.gen.cpp` 在构建时扫描，增删反射头文件不需要重新 generate。
- HeaderTool 也扫描模块的源文件，收集 `MARLIN_UPGRADE`（§5）和 `MARLIN_TEST`（11 §1），一起登记在 `Register_<Module>` 中。
- `MarlinModuleTable[]` 由 toolkit 为每个链接了 Core 的可执行文件生成（`Reflection/ModuleTable.h`）。monolithic 直接指向各模块的 `Register_<Module>`；
  modular 只给出 DLL 文件名，`Reflection::RegisterModules` 加载后按名字查找。启动流程（01 §2 第 5 步）调用 `Reflection::RegisterModules(GetModuleTable())`。
- 运行时描述的类型（脚本 struct 和脚本 System，MarlinScript §11.2）用 `Reflection::RegisterDynamicType` 一个个登记，同名已存在时返回 false；
  它们的字段用直接持有 `TypeInfo` 的 `FieldInfo` 构造函数，不需要 `TypeGetter`。登记后和生成的类型一样能按名字查找、遍历字段。
- **只生成 `TypeInfo` 和方法调用 thunk**。序列化不是逐类型生成的代码，而是遍历 `TypeInfo` 的通用实现（MarlinText 见 06 §1.3）。
  二进制序列化、脚本绑定、Remote 注册、复制差分表和 `.gen.mscript` 在对应模块建起来时再做，它们都从 `TypeInfo` 和元数据读取所需信息。

## 2.1 运行时类型信息

`Core/Public/Reflection/`：

| 接口 | 作用 |
| --- | --- |
| `TypeOf<T>()` | 反射类型、内置类型（整数、浮点、`String`、`StringView`、`Name`）或容器的 `TypeInfo` |
| `TypeInfo` | 名字（全局唯一，序列化用）、种类、大小、版本、父类、字段、函数、枚举值、元数据、通用操作（构造、拷贝、比较、哈希、`New`……） |
| `FieldInfo` / `FunctionInfo` | 偏移或静态地址；参数名、默认值文本、`Invoke(object, arguments, result)` |
| `Cast<T>(object)` | 沿类层次或到实现的接口转换，失败返回 null |
| `Reflection::FindType` / `ForEachType` / `GetDerivedTypes` | 查找；Subsystem 发现用 `GetDerivedTypes` |
| `Reflection::Equals` / `Copy` / `FormatValue` / `ParseValue` | 通用的比较、深拷贝（指针也拷贝指向的对象）、可读文本和标量解析 |

- 反射类型的描述是常量初始化的静态数据，指向其他类型时存 getter 而不是指针，所以没有静态构造器；`Register_<Module>` 只负责登记、按名字查重、链接父类和登记 CVar。
- 容器（`List`、`T[N]`/`Array`、`HashMap`、`HashSet`、`Optional`、`UniquePtr`、`SharedPtr`）的 `TypeInfo` 第一次使用时创建，按 C++ 类型去重，所以所有模块共用一份。
- 其他模块的句柄类型（如将来的 `AssetRef<T>`）可以特化 `Private::ContainerType`，描述成 `TypeKind::Opaque`，并给 MarlinText 注册读写格式。

## 3. 对象模型

**决策**：没有 UObject，也没有全局 GC。

| 类别 | 基类 | 所有权 | 说明 |
| --- | --- | --- | --- |
| 组件 | 反射 `MSTRUCT(Component)` 的 POD | ECS chunk | 可平凡复制（或者声明 move 语义），放在 archetype 里 |
| 资产 | `Asset` | 引用计数，`AssetRef<T>` | 带 `Guid`、`Name`、`TypeInfo`；运行时只读，编辑器通过 Source 类型修改 |
| Subsystem | `*Subsystem` | 所属作用域 | 01 §3 |
| 普通反射类 | 任意 `MCLASS` | `UniquePtr` / `SharedPtr` | 设置、工具对象 |
| 脚本对象 | VM 堆 | VM 增量 GC | 08 |

- `AssetRef<T>`：强引用，原子引用计数，计数归零后在帧末的安全点释放（排队到 `AssetManager`）。
- `SoftAssetRef<T>`：只存 Guid，不触发加载，用于按需加载和流式加载。
- 脚本持有 `AssetRef` 时，被引用一方会被计为一个外部根，GC 收集脚本对象时再释放这个引用。

### 3.1 运行时资产的字段

**决策**：运行时资产以**固定宽度内存镜像**存放（06 §4），所以 `Asset` 子类的非 `Transient` 字段只能是镜像安全的类型：

| 类型 | 镜像中的形式 |
| --- | --- |
| 定宽整数、浮点、`bool`、`MENUM`、`Guid` | 原样 |
| 只由镜像安全类型组成的 `MSTRUCT`、定长数组 | 原样 |
| `FrozenArray<T>`（Core） | 指针 + 数量；元素放在块的变长区，指针在镜像里是相对块起点的偏移，加载时修正 |
| `FrozenString`（Core） | 同 `FrozenArray<char>`，以 `\0` 结尾 |
| `AssetRef<T>` | 指针大小；镜像里是依赖表的序号，加载时换成依赖资产的槽位 |
| `SoftAssetRef<T>` | `Guid`，不修正 |

- `List`、`String`、`HashMap`、指针这类带堆内存的类型不能进镜像；`Transient` 字段不进镜像，可以是任意类型（运行时状态、GPU 句柄）。
  资产表初始化时（`Assets::Initialize`）检查所有 `Asset` 子类，违反的类型报错并且不能加载；Converter 写块时也会检查。
- `FrozenArray` 和 `FrozenString` 可以拥有自己的堆内存（Converter、编辑器、测试构造时），也可以借用块里的内存（加载后）；只读访问两种情况一样。
- 所有平台都是 64 位 clang、自然对齐，所以同一个类型在各平台上的镜像布局相同，PC 上的 Converter 可以直接给主机生成数据。
  每个类型有一个**布局哈希**，由反射信息算出（字段名、类型、偏移、大小，递归到嵌套类型），写进块头和结果 key（06 §3.2）；布局哈希不一致的块拒绝加载，需要重新转换。
- `AssetRef`、`SoftAssetRef` 在反射里是 `TypeKind::ObjectLink`（Core 提供 `LinkOperations`：取得和设置 Guid），MarlinText 把它们读写成对象链接 `<Path:Name#UUID>`。
  路径和名字只是提示：读取时只用 Guid；写入时由调用方提供的解析器（Indexer）补上提示，没有解析器就写 `<#UUID>`。

## 4. Source 类型 / Runtime 类型分离

**决策**：编辑器数据不进 Runtime。

```
Developer/Editor 模块          Converter                     Runtime 模块
TextureSource (MCLASS)  ──────────────────────▶   Texture (MCLASS, Asset)
  .masset 中的对象                GlobalStore              紧凑二进制，GPU 就绪
  Format, MipGen, SourceFile
```

- 每个运行时资产类型都有一个对应的 Source 类型（简单资产可以共用同一个类型，用 `MCLASS(SourceIsRuntime)` 标记，例如纯数据表）。
- `Converter<TSource, TRuntime>` 注册在 Developer 模块里，带 `Version`，版本号是结果 key 的一部分。
- Editor 永远编辑 Source 对象，预览时走「转换后加载」路径。Editor 和 Game 加载数据的方式完全一致，所以「编辑器里正常、游戏里出错」的情况会大大减少。
- 组件也是一样：Level 和 Prefab 里保存的是组件的 **Source 表示**（MarlinText），转换后得到 archetype 的二进制块。
  对于纯 POD 组件，Source 和 Runtime 布局相同，可以直接复用反射描述。

## 5. Schema 版本

每个反射类型有 `MCLASS(Version=3)`。字段改名或改类型时注册升级函数：

```cpp
MARLIN_UPGRADE(TextureSource, 2, 3)
{
    object.Rename("Srgb", "ColorSpace");
}
```

加载旧版本的 MarlinText 时自动迁移；保存时总是写入最新版本。也可以用 `MarlinConverter -Upgrade` 批量重写。

- `MARLIN_UPGRADE` 定义一个函数，可以写在能看到该类型的任何模块的任何源文件里；HeaderTool 扫描源文件找到它，在 `Register_<Module>` 中按类型名登记（`MarlinText/MarlinTextSchema.h`）。
  从文件的 `!Schema` 出发，每步选能走到最远的升级函数，直到类型当前的版本；缺一步就是错误，文件版本比类型新也是错误。
- `MarlinTextSerializer::UpdateObject` 保存旧版本的对象时，在文本上重放升级的效果（删除被改名或删除的字段行，写入新增或改变的字段），再写入差异和新的 `!Schema`，对象里其余的注释和格式不变。
