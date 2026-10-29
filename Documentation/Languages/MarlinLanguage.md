# Marlin 语言规范（公共子集与 Shader 方言）

本文细化 [08 · Shader 语言与脚本语言](../Architecture/08-Languages.md) 和 [07 §4 材质](../Architecture/07-Rendering.md)，
是 `LanguageFrontend`、`LanguageIR`、`ShaderBackend*` 的实现依据。Script 方言（GC 类型、VM、`[Kernel]`）见 [MarlinScript](MarlinScript.md)；
本文只规定前端为它预留的部分。

约定：「错误」指编译期诊断，编译失败；「未指定值」指结果是该类型的某个值，但不保证是哪个，不会出现未定义行为。

## 1. 源文件与编译单元

### 1.1 文件

- UTF-8，可以带 BOM。换行 `\n` 或 `\r\n`。方言由扩展名决定：`.mcommon` 公共子集，`.mshader` Shader，`.mscript` Script。
- 没有预处理器。条件编译用 `static if`（§6.4），变体用 `static` 参数（§9.4）。

### 1.2 命名空间

- 与 C# 相同：`namespace Marlin.Shading;`（文件级，必须在所有声明之前）或 `namespace A.B { … }`（块级，可嵌套）。不写就在全局命名空间。
- 命名空间与文件、目录无关；同一个命名空间可以分布在任意多个文件中。
- `using Marlin.Shading;` 引入命名空间的成员；`using Tex = Marlin.Texture2D<half4>;` 定义别名；`using static Marlin.Math;` 引入类型的静态成员。
  `using` 只作用于所在的文件或命名空间块。
- `public using float3 = Vector<float, 3>;`（带可见性修饰）是命名空间成员级别的别名：它像类型一样属于所在命名空间，按 §1.4 的可见性对其他文件可见。
  标准库用它定义 `float3`、`half4x4` 这类名字。
- `Marlin` 命名空间里的内建类型和内建函数（§4、§10）始终可见，不需要 `using`。

### 1.3 源集（Source Set）

一次编译看到的全部源文件叫源集。

- 每个挂载根的语言代码都在它的 `Language/` 目录下，按方言分成三个子目录：`Language/Common/`（`.mcommon`）、`Language/Shaders/`（`.mshader`）、
  `Language/Scripts/`（`.mscript`）。文件必须放在与扩展名对应的子目录中，放错是错误。
- 源集 = 当前项目可见挂载点下所有 `Language/Common/` 中的文件，加上对应方言子目录中的文件：`/Engine/Language`、
  `/Project/Language`。Shader 编译看到 Common + Shaders，Script 编译看到 Common + Scripts（MarlinScript §1.3）。
  这和 Config 的层叠顺序一致（06 §1.4），但源集不做覆盖：同一个全限定名定义两次是错误。
- 前端先对源集中每个文件做一次只看顶层声明的**声明扫描**，得到「全限定名 → 文件区间」的索引；函数体和类型成员在第一次被引用时才完整解析和检查。
  所以编译一个入口点只需要解析它真正用到的声明。
- 声明扫描的结果（每个文件定义了哪些命名空间成员、引用了哪些名字）交给 Indexer 和 Converter，用来算结果 key 的依赖集合（06 §3.2）：
  一个入口点的依赖 = 它在语义检查中实际解析到的声明所在的文件。
- 源集通过接口 `ISourceProvider` 提供（列出文件、读取文本、文件的内容哈希），引擎内的实现基于 Vfs；测试可以用内存中的实现。

### 1.4 可见性

- `public`：任何文件可见。`internal`（默认）：同一个**包**内可见。包 = 一个挂载根（`/Engine`、`/Project`）。`private`：同一个类型内可见。
- 顶层函数、类型、常量都可以加可见性修饰。

### 1.5 `Language/Shaders/` 的目录约定

命名空间与目录无关（§1.2），但引擎和项目的 `Language/Shaders/` 都按下面的约定组织，目录与命名空间一一对应（`Language/Common/` 与 `Language/Scripts/` 用同样的 `Core/`、`Library/<Area>/` 划分，Script 的约定见 MarlinScript §1.3）：

```
<EngineRoot>/Language/Shaders/
  Core/                  Marlin                      核心库（§9.10）：内建类型和内建函数。只有各方言的 Core/ 允许 [Builtin] 与无函数体的内建成员
  Library/<Area>/        Marlin.<Area>               通用工具：Math、Color、Packing、Sampling、Noise、Parallel（wave + groupshared 的归约、扫描、压缩，08 §5.5）
  Shading/               Marlin.Shading              材质接口：Surface、Slab、MaterialInputs、IBsdf、BsdfContext、LightSample，以及内置 BSDF（Bsdfs/）
  Scene/                 Marlin.Scene                GPU Scene 的数据布局：实例、灯光、材质表、View 常量；与 C++ 共享，用 §10 的布局校验
  Passes/<Feature>/      Marlin.Passes.<Feature>     渲染管线的 pass shader（07 §3）：Culling、VisibilityBuffer、MaterialResolve、Lighting、
                                                     Shadows、Translucency、Post、Ui、Debug
  Materials/             Marlin.Materials            引擎自带的材质：默认材质、Unlit、调试与可视化材质（对应的 .masset 在 Content/）
  Editor/                Marlin.Editor               只有编辑器的 pass 引用的 shader：网格、gizmo、选中描边、缩略图
  Tests/                 Marlin.Tests                GPU 测试用的 shader（11）
```

- **依赖方向**：`Core` ← `Library` ← `Shading`、`Scene` ← `Materials`、`Passes`、`Editor`。下层不引用上层。
- **可见性**：`Core`、`Library`、`Shading`、`Scene` 是对项目公开的 API，声明用 `public`；`Passes`、`Editor`、`Tests` 保持默认的 `internal`
  （包 = 引擎，§1.4），所以项目代码引用不到 pass 的内部实现。pass 调用材质函数不靠名字引用，而是由 Converter 为每个「材质 × pass」生成入口（§12）。
- **项目**：`/Project/Language/Shaders/` 用同样的布局（`Materials/`、`Library/`、需要自定义渲染特性时的 `Passes/`），
  根命名空间是项目名。材质代码写在 `Language/Shaders/Materials/`，材质资产（`.masset`，06）在 `Content/`，通过对象链接指向 `[Material]` struct（§9.4）。
- **平台层**：`Platforms/<Layer>/Language/` 按相同的相对路径叠加（02 §5.4），用同名文件整体替换某个 pass 的实现；Vfs 对同一路径只返回胜出的那个文件，
  所以不会出现同一个全限定名定义两次。
- `Language/Common/` 中的 `.mcommon` 文件也属于 Script 的源集（MarlinScript §1.3），所以与 gameplay 共享的数学和布局写成 `.mcommon`，放在 `Language/Common/Library/<Area>/`。

### 1.6 Shader 代码与资产

- **源文件不是资产**。`.mshader`、`.mcommon` 和 `.mscript` 一样是代码：Indexer 把它们当源码索引（文件表、声明、引用，06 §2），不给文件分配 UUID，
  也不是 `.masset` 对象。依赖关系由前端给出：每次编译实际解析到的声明所在的文件就是它的依赖（§1.3），比按文件或资产引用记录更精确，
  核心库、`Library`、`Shading` 中任何被用到的代码改动，都会让相应的结果 key 失效。
- **材质是资产**。`Material` 和 `MaterialInstance` 是 `Content/` 中的 `.masset` 对象，走 06 的资产表、GlobalStore、打包流程。`Material` 用对象链接
  指向一个 `[Material]` struct（§9.4）：

  ```
  Material { !UUID = "…"  !Name = "Rock"  Shader = </Project/Language/Shaders/Materials/Rock:Project.Materials.Rock#9a0b…> }
  ```

  链接里的 UUID 来自声明上的 `[Id("9a0b…")]`，由编辑器或 LSP 在新建材质时自动写入（§13 问题 1 采用方案 A）。Indexer 把带 `[Material, Id]` 的
  struct 当作对象索引（类型 `MaterialShader`），所以它们和资产一样出现在 Asset Browser、Reference Viewer 中，移动文件后路径提示由 Indexer 修正。
  Converter 为每个「材质 × 使用材质的 pass × 档位 × 变体」特化一份产物（§9.4、§12）。
- **Pass shader 不是资产**。渲染管线、UIPainter、compute 工具和编辑器用的 shader 由 C++ 按入口点的全限定名引用（如 `Marlin.Passes.Post.Tonemap`）。
  引擎和项目的 `Language/Shaders/Passes/` 中的每个入口点都是**全局 shader**；`Language/Shaders/Editor/` 中的入口点只给编辑器用。
- **全局 shader 不在打包引擎时预编译**：安装版引擎（02 §2.2）可能用来面向与宿主不同的平台和设备档位，所以它只带源码。
  编辑器第一次启动、或第一次打开某个项目时，为当前设备匹配的档位（§9.7）编译引擎和项目的全局 shader，写入 GlobalStore 并显示进度；
  之后只有源文件或档位变化才重新编译。打包游戏时，Packager 再通过 Converter 为目标平台的每个档位编译（或从 GlobalStore 取回）并放进 mpak。
  热重载与材质相同：源文件变化 → 依赖它的结果 key 失效 → 重新编译（06 §6）。
- **不是资产也走 GlobalStore**：GlobalStore 按内容寻址（06 §3.3），key 里没有任何东西要求资产 UUID。全局 shader 的转换单位是一个入口点，由全限定名标识：

  ```
  Key = Hash128( ShaderConverter.TypeId, ShaderConverter.Version, 入口点全限定名,
                 依赖的每个源文件的内容哈希（§1.3），档位, TargetPlatform, 编译选项 )
  ```

  它和资产一样经过 Converter 进程池、GlobalStore 和 CI 预热。运行时由 `ShaderLibrary`（EngineSubsystem）按名字加载：开发期名字 → 结果 key，
  打包后名字 → mpak 条目。mpak 的目录按 Guid 索引（06 §5），全局 shader 的 Guid 取入口点全限定名的 UUIDv5（基于名字、确定性），
  所以 mpak 格式不变，也不需要在任何地方保存这个 UUID；改名就是另一个 shader。

## 2. 词法

### 2.1 注释与空白

`// …`、`/* … */`（不嵌套）、`/// …`（文档注释，LSP 用，语义上等同于普通注释）。

### 2.2 标识符

`[A-Za-z_][A-Za-z0-9_]*`。`@` 前缀可以把关键字当标识符用（`@struct`）。以两个下划线开头的名字保留给编译器。

### 2.3 关键字

公共关键字：

```
namespace using public internal private static const readonly struct enum interface where
if else for while do switch case default break continue return true false null
in out ref new with this operator implicit explicit is as sizeof typeof nameof
void bool int uint short ushort long ulong float double half
```

上下文关键字（只在特定位置是关键字）：`var`、`get`、`set`、`extension`、`type`（interface 中，§3.5）、
`discard`（Shader）、
`class`、`virtual`、`override`、`abstract`、`async`、`await`、`yield`（Script，在其他方言中写出来会报「此方言不支持」，而不是当作标识符）。

### 2.4 字面量

| 种类 | 形式 | 类型 |
| --- | --- | --- |
| 整数 | `123`、`0x7F`、`0b1010`，可以用 `_` 分隔数字 | 默认 `int`；装不下时依次是 `uint`、`long`、`ulong` |
| 整数后缀 | `u`、`l`、`ul`（大小写都行） | `uint`、`long`、`ulong` |
| 浮点 | `1.0`、`.5`、`1e-3`、`1.5e+2` | 默认 **`float`**（与 C# 不同，C# 是 `double`） |
| 浮点后缀 | `f`、`h`、`d` | `float`、`half`、`double` |
| 布尔 | `true`、`false` | `bool` |
| 字符串 | `"…"`，转义 `\n \t \" \\ \uXXXX` | 只能出现在属性参数中（Script 例外） |

常量表达式可以隐式收窄：整数常量转换成能精确表示它的整数或浮点类型（`ushort n = 3;`），浮点常量转换成更窄的浮点类型时按就近舍入，
只要不超出目标类型的有限范围（`half3(0.2126, 0.7152, 0.0722)` 不需要写 `h` 后缀）。非常量的收窄转换必须显式写出。

## 3. 声明

### 3.1 顶层与命名空间成员

函数、`struct`、`enum`、`interface`、`const` 常量、`static readonly` 值、Shader 方言的资源与参数声明（§9）。没有全局可变变量
（`[GroupShared]` 除外，§9.6）。

### 3.2 函数

```csharp
public static half Luma(this half3 c) => Dot(c, half3(0.2126h, 0.7152h, 0.0722h));

T Max<T>(T a, T b) where T : IComparable<T> { return a.CompareTo(b) >= 0 ? a : b; }
```

- 顶层函数不需要写 `static`。表达式体 `=> expr` 与块体都可以。
- 参数修饰：无（按值）、`in`（只读，按值语义，编译器可以传引用）、`out`（必须在返回前赋值）、`ref`（读写）。
- 默认参数值必须是常量表达式。调用时可以用命名参数：`Surface.VerticalLayer(top: moss, bottom: rock, thickness: t)`。
- 重载按 C# 规则决议：先看参数个数和命名参数，再选「转换最好」的候选；有歧义是错误。
- 递归（直接或间接）在 Shader 和公共子集中是错误。

### 3.3 struct

```csharp
public struct Slab
{
    public half3 Albedo;
    public half Roughness = 0.5h;
    public static readonly Slab Default = new Slab();
    public half3 Specular => Lerp(half3(0.04h), Albedo, Metallic);
    public Slab(half3 albedo) { Albedo = albedo; }
}
```

- 值类型。字段可以有初始值；`new S()` 和 `default(S)` 的区别与 C# 相同：`new S()` 使用字段初始值，`default` 全部为零。
- 成员：字段、方法、构造函数、只读属性（`=> expr` 或 `get { }`）、`static readonly` 值、`const`、运算符重载。
- `new S(args)` 和 `new S { A = 1, B = 2 }`（对象初始化器）构造值；Shader 中的 `new` 只能用于 struct。
- `x with { A = 1 }` 复制后修改字段。
- 没有继承。可以实现 interface（只用于静态分派，§3.5）。

### 3.4 enum

`enum DamageType : uint { Fire, Ice = 4 }`，底层类型默认 `int`。`[Flags]` enum 允许 `|`、`&`、`~`。枚举与整数之间需要显式转换。

### 3.5 interface 与泛型

```csharp
public interface IBsdf
{
    half3 EvaluateDirect(in BsdfContext ctx, in LightSample light);
    [Optional] BsdfSample Sample(in BsdfContext ctx, float2 u);
}
```

- interface 首先是泛型约束（`where T : IBsdf`），调用通过单态化静态分派。interface 不能作为变量、字段或参数的类型：没有动态分派。
- `[Optional]` 成员可以不实现。`T.Implements(nameof(IBsdf.Sample))` 是编译期常量（§6），供 `static if` 和能力位使用。

interface 的要求不只是实例方法（这部分取自 Slang 的泛型系统，语法按 C#）：

```csharp
public interface ILight
{
    type Sample : ILightSample;                         // 关联类型：每个实现自己决定具体类型
    static abstract this Default { get; }               // 静态要求；类型位置上的 this 是实现者自己的类型
    Sample SampleAt(float3 position, float2 u);
    static abstract this operator +(this a, this b);    // 运算符要求
}

public struct SpotLight : ILight
{
    public type Sample = SpotSample;                    // 绑定关联类型
    public static SpotLight Default => new SpotLight();
    …
}

void Accumulate<T>(T light) where T : ILight
{
    T.Sample s = light.SampleAt(p, u);                  // 通过约束访问关联类型
    T zero = T.Default;
}
```

- **关联类型**：`type Name : IConstraint;` 声明，实现用 `public type Name = Concrete;` 绑定；泛型代码中写作 `T.Name`。
- **类型位置上的 `this`**：在 interface 和 extension 中，`this` 出现在类型的位置上时表示「实现这个 interface 的具体类型」（Slang 的 `This`，按 C# 的关键字写成小写），不需要 C# 的 `TSelf` 递归泛型写法；出现在表达式中仍然是当前实例。
- **静态要求**：`static abstract` 的方法、属性、运算符和构造函数（`static abstract this Create(…)`），泛型代码通过 `T.Member` 调用。
  核心库的数值接口（§9.10）就是这样定义 `T.Zero`、`T.One`、`+`、`*` 的。

**扩展（extension）**：在类型定义之外为它补上成员和 interface 实现（Slang 的 retroactive conformance）：

```csharp
public extension half3 : ILuminance
{
    public half Luminance() => Dot(this, half3(0.2126, 0.7152, 0.0722));
}

public extension<T> Texture2D<T> where T : IFloatingPoint
{
    public T SampleBicubic(Sampler s, float2 uv) { … }
}
```

- 引擎的类型可以由项目代码实现项目定义的 interface，反之亦然；不需要改原来的声明。
- extension 不能添加字段，可以添加方法、属性、静态成员、运算符和 interface 实现。对同一个类型、同一个 interface 的两处实现是错误。
- 可见性按 extension 自己的修饰（§1.4）；extension 生效的范围是能看到它的代码。C# 的 `this` 参数扩展方法（§5.3）仍然可用，是只加一个方法时的简写。
- extension 可以给向量和标量加运算符；只在没有内建运算符适用时才用它（例如 `float3 ^ float3`），不能改变内建运算的含义。

- 泛型参数可以是类型，也可以是**常量值**：`struct Vector<T, int N>`、`float Sum<int N>(float[N] values)`。常量值参数的类型限于整数、`bool`、enum。
- 约束：`where T : IFoo, struct`、`where T : unmanaged`（只包含数值、向量、矩阵、enum 和这些类型组成的 struct）。
- 泛型在使用处全部单态化。类型实参可以推导；推导失败需要显式写出。
- 泛型调用和比较的歧义（`a < b > c`）按 C# 的规则消解：看 `>` 之后的记号。

### 3.6 const 与 static readonly

- `const` 常量和 `const` 函数在编译期求值（§6）。
- `static readonly` 值的初始化表达式也必须能在编译期求值；它们可以是 struct（`Slab.Default`）。

## 4. 类型

### 4.1 标量

| 类型 | 宽度 | 说明 |
| --- | --- | --- |
| `bool` | — | 在缓冲区中按 32 位存储 |
| `short` / `ushort` | 16 | |
| `int` / `uint` | 32 | |
| `long` / `ulong` | 64 | |
| `half` | 16 | **精确的** IEEE binary16。最低硬件要求（07 §1）都支持原生 fp16，所以不提供 `min16` 这类最低精度类型 |
| `float` | 32 | |
| `double` | 64 | Shader 中只能用于计算，不能出现在顶点、varying 和纹理格式中 |

### 4.2 向量与矩阵

- `Vector<T, N>`（N = 2…4）、`Matrix<T, R, C>`（R、C = 2…4，R 行 C 列）。`float3`、`half4`、`uint2`、`bool3`、`float4x4`、`float3x4` 等是别名，写法完全等价。
- 构造：`float3(1, 2, 3)`、`float4(v.xyz, 1)`、`half3(0.5h)`（标量广播）。按类型名直接调用，不需要 `new`。
- 分量访问 `.x .y .z .w`，或者 `.r .g .b .a`，可以组合成 swizzle（`v.zyx`、`c.rgb`）；作为左值时分量不能重复。
- 运算：`+ - * / %` 和比较都按分量计算；比较的结果是 `bool` 向量，`all()`、`any()` 把它归约成一个 bool。标量与向量运算时广播标量。
- 矩阵：`m * v` 是矩阵乘向量（列向量约定，与 Core 的 `Matrix3x4` 一致：`p' = M * p`）；`a * b` 是矩阵乘法；按分量相乘用 `a.MultiplyComponents(b)`。
  `m[i]` 是第 i 行。**缓冲区中的矩阵按行主序存储**，与 C++ 端的布局相同。

### 4.3 数组

- 定长数组 `float[4]`、`Slab[N]`（N 是常量或常量泛型参数）。值语义，赋值时复制。
- 下标越界：常量下标越界是错误；运行时的下标被钳制到最后一个元素，越界的读写都落在最后一个元素上（数组、向量和矩阵的行都一样）。
  `[Unchecked]` 函数里不钳制，越界是未定义行为；`foreach` 的下标已知在范围内，从不钳制。`Span<T>` 与数组的规则相同；`Ptr<T>` 不带长度，越界是未定义行为（§4.5）。
  编译选项 `CheckSpans`（要求同时打开调试输出，§9.11）让 `Span<T>` 的每次下标访问在越界或指针为空时还像失败的 `Debug.Assert` 一样报告：
  ``file:line: assertion failed: `args.Views[i]` reads index 5 of a Span of 2 at <地址>``。访问照样被钳制，空指针的访问仍会读写地址 0 附近，
  报告要等这一帧在 GPU 上完成才读得到，所以它让 GPU 出错时只能靠崩溃报告（07 §1.7）。
- 运行时长度的数组只能在 GPU 内存中，通过 `Span<T>` 或 `Ptr<T>` 访问（§4.5）。

### 4.4 资源类型（Shader 方言）

GPU 资源分两类：**内存**（缓冲区里的数据）用指针访问（§4.5）；**描述符**（纹理、采样器、加速结构）用句柄访问。
语言里没有「绑定」「寄存器」「槽位」，也没有缓冲区视图类型：两者都是普通的值，可以作为字段放进 struct、存进 GPU 内存、作为参数传递、作为入口点的 `[Uniform]` 参数（§9.3）。

| 类型 | 说明 |
| --- | --- |
| `Texture1D`、`Texture1DArray`、`Texture2D`、`Texture2DArray`、`Texture2DMultisample`、`Texture3D`、`TextureCube`、`TextureCubeArray`，都是 `<T, Access A = Access.Read, ImageFormat F = ImageFormat.Inferred>` | 纹理 |
| `Sampler`、`ComparisonSampler` | 采样器 |
| `AccelerationStructure` | 光追加速结构 |

纹理的形状是类型名，读写权限和存储格式是类型参数（取自 Slang 内部的统一纹理类型 `_Texture<T, Shape, …, Access, Format>`，不要它为 HLSL 准备的 `RW*` 别名层）：

- `Access`：`Read`（默认）、`Write`、`ReadWrite`。可写纹理写作 `Texture2D<float4, Access.ReadWrite>`，不再有 `RW` 前缀的类型。
  只有 `Access.Read` 的纹理可以采样（`Sample*`、`Gather*`、`CalculateLod`、`MipCount`），只有 `Write` / `ReadWrite` 的纹理可以 `Store`；用错是错误。
- `ImageFormat`：可写纹理的存储格式，例如 `Texture2D<float4, Access.Write, ImageFormat.Rgb10a2Unorm>`；取代 HLSL 的 `[format("…")]` 修饰。
  `Inferred`（默认）按 `T` 推断（分量数和位宽相同的格式，`half4` 是 `Rgba16f`、`uint` 是 `R32u`；三分量没有对应格式，按无格式访问）。
  格式必须能存放 `T`（浮点、unorm、snorm 格式存浮点，`i` / `u` 格式存有符号 / 无符号整数），只读纹理不能写格式。
- `T` 是纹理元素的类型，必须是标量或向量。
- 单一的泛型 `Texture<T, Shape, A, F>` 加 `using` 别名要等泛型别名和部分特化的 extension（§3.5）实现之后再考虑：不同形状的成员签名不同，现在由各自的类型声明。

**句柄**：一个描述符值在内存中是 32 位的堆索引（bindless，07 §1）。

- 默认值（`default`）是「空句柄」，使用空句柄得到未指定值。
- 索引是否一致：前端对句柄做一致性分析；可能在 wave 内不一致的句柄，后端自动加上 `NonUniform`。作者不需要手写。
- 成员（PascalCase）：
  - 采样（只读纹理）：`Sample(s, uv)`（只在像素阶段）、`SampleLevel(s, uv, lod)`、`SampleGrad(s, uv, ddx, ddy)`、`SampleBias(s, uv, bias)`、
    `SampleCompare(cs, uv, reference)`、`SampleCompareLevel(cs, uv, reference, lod)`、`Gather(s, uv, component)`、`GatherCompare(cs, uv, reference)`、`CalculateLod(s, uv)`。
  - 按整数坐标访问：`Load(p)` / `Load(p, mip)`；可写纹理 `Store(p, value)`；多重采样 `Load(p, sample)`。
  - 尺寸：属性 `Size`（第 0 级；数组纹理的最后一个分量是层数）、`MipCount`、`SampleCount`，方法 `GetSize(mip)`。

### 4.5 指针与 Span

GPU 内存用指针访问（Slang 的指针模型），取代 HLSL 的 `StructuredBuffer` / `ByteAddressBuffer` 这类缓冲区视图：

| 类型 | 大小 | 说明 |
| --- | --- | --- |
| `Ptr<T, Access A = Access.Read>` | 8 字节 | 指向 GPU 内存中的 `T`。默认只读；`Ptr<T, Access.ReadWrite>` 可写 |
| `Span<T, Access A = Access.Read>` | 16 字节 | 指针加 `uint` 长度（再填充到 16），下标按 §4.3 钳制 |

```csharp
public struct CullArgs
{
    public Span<Instance> Instances;
    public Ptr<uint, Access.ReadWrite> VisibleCount;
    public Ptr<ViewConstants> View;
}

[Compute(64, 1, 1)]
void Cull([Uniform] in CullArgs args, [DispatchThreadId] uint3 id)
{
    if (id.x >= args.Instances.Length) return;
    Instance instance = args.Instances[id.x];
    if (IsVisible(instance, args.View.Value))
        Atomic.Add(ref args.VisibleCount.Value, 1);
}
```

- **访问**：`p.Value` 是 `p[0]`；`p[i]` 是第 i 个元素（步长 = `sizeof(T)` 向上对齐到 `T` 的对齐，§10），是一个引用：只读指针得到 `ref readonly`，可写指针得到 `ref`。
  字段直接访问（`p.Value.Count`、`p[i].Count`），不需要 `->`。
- **运算**：`p + n`（按元素偏移）、`p.Offset(bytes)`、`p.Cast<U>()`（重新解释，对齐由作者保证）、`p.IsNull`、`p == q`、`p != q`。
  `Ptr<byte>` 加 `p.Read<T>(byteOffset)`、`p.Write<T>(byteOffset, value)` 取代字节寻址缓冲区。
- **Span**：核心库中的普通 struct `{ Ptr<T, A> Pointer; uint Length; }`（16 字节），构造 `new Span<T>(pointer, length)`；`s[i]`（钳制）、`s.Slice(start, count)`。
  `foreach (T x in s)` 走过 `Length` 个元素，下标已知在范围内，不钳制。
- **隐式转换**：可写的 `Ptr` / `Span` 转成同元素的只读 `Ptr` / `Span`，`Span<T, A>` 转成它的 `Ptr`（`Span<T>` 参数也接受可写的 `Span`）。
- **原子操作**作用在可写引用上：`Atomic.Add(ref p[i].Count, 1)`、`Atomic.CompareExchange`、`Atomic.Max` 等，32 和 64 位整数、`float`（加法）。
- **默认值**是空指针；解引用空指针、`Ptr<T>` 越界都是未定义行为（GPU 可能报页错误，07 §1.5 的崩溃报告会指出地址）。需要安全的地方用 `Span<T>`。
- **一致性与别名**：前端假定只读指针指向的内存在一次调度中不变（可以缓存、提前读取）；可写指针之间可能别名。`[NoAlias]` 放在参数或字段上（包括 `Span` 类型的）表示它不与其他指针别名。
- 指针只能指向 GPU 内存（`Device` 地址空间）。`ref` 局部变量、`out`/`ref` 参数仍然用于函数内的值，不能转换成指针，也不能取局部变量或 groupshared 变量的地址。

**C++ 一侧**：`RhiDevice::GetBufferAddress(buffer)` 返回一个不透明的 `uint64`，加上字节偏移就是一个 `Ptr`；共享结构中的 `Ptr<T>` 在 C++ 中是 `uint64`，
`Span<T>` 是 `ShaderSpan`（`RHI/RhiShaderTypes.h`，16 字节）。RenderGraph 的缓冲区视图没有描述符，pass 用 `RenderGraphContext::GetAddress(view)`、`GetCount(view)`、`GetSpan(view)` 取得它们。

**各目标的实现**：

| 目标 | `Ptr<T>` 的表示 |
| --- | --- |
| Vulkan（SPIR-V） | 缓冲区设备地址，`PhysicalStorageBuffer` 存储类（Vulkan 1.4 核心） |
| Metal | `device` 指针 |
| D3D12（DXIL） | 胖指针：高 32 位是缓冲区的原始视图（raw SRV/UAV）在描述符堆中的索引，低 32 位是字节偏移。访问降低为 `RawBufferLoad/Store`，指针运算只改低 32 位。RHI 为每个缓冲区建一个覆盖整块内存的原始视图 |
| 主机 | 由平台层的后端决定（12） |

`Ptr` 的值对 shader 是不透明的：不能和整数互相转换，所以同一份 shader 代码在所有目标上的语义相同；只有 C++ 一侧知道它的二进制表示。

### 4.6 RayQuery

`RayQuery<RayFlags.CullBackFacingTriangles>`：值类型，不能存进缓冲区。方法与 DXR 1.1 对应（`TraceRayInline`、`Proceed`、`CommittedStatus` …）。光追管线的阶段见 §9.8。

- 只能是局部变量，声明即可使用（不需要初始值，`TraceRayInline` 开始一次遍历）；不能复制、赋值或按值传递，只能调用它的方法。
- 类型参数中的标志是编译期常量，与每次 `TraceRayInline` 传入的标志按位或。
- `AccelerationStructure` 是堆中的描述符句柄，与纹理一样（§4.4）；SPIR-V 后端用 `SPV_KHR_ray_query`，`CommittedObjectToWorld` 按 §11.3 的矩阵约定转置。

### 4.7 类型转换

- 隐式：整数拓宽（同符号或无符号到更宽的有符号）、整数到浮点、`half → float → double`、标量到向量（广播）、常量的收窄（§2.4）。
- 显式 `(T)x`：其余的数值转换。浮点转整数向零截断，超出范围得到未指定值。
- 向量之间的转换按分量进行，分量数必须相同；`float4` 到 `float3` 必须用 swizzle 写出。
- struct 之间没有隐式转换，除非定义了 `implicit operator`。

## 5. 语句与表达式

### 5.1 语句

`{ }`、局部变量（`var x = …;`、`float x;`——使用前必须确定赋值）、表达式语句、`if/else`、`for`、`while`、`do/while`、`foreach`
（定长数组和 `Range(a, b)`）、`switch`（整数、enum；`case` 不允许贯穿，C# 规则）、`break`、`continue`、`return`、`static if`（§6.4）、
`discard;`（Shader 的像素阶段）。

没有 `goto`、异常、`lock`、`unsafe`、指针。

### 5.2 表达式

- 运算符和优先级与 C# 相同：`?:`、`|| &&`、`| ^ &`、`== !=`、`< > <= >= is`、`<< >>`、`+ -`、`* / %`、一元 `+ - ! ~ ++ --`、
  后缀 `x++ x-- f() a[i] a.b`。复合赋值 `+=` 等。
- `&&`、`||`、`?:` 短路求值，对向量是错误（用 `Select(c, a, b)`）。
- 整数溢出按补码回绕；整数除以零得到未指定值；移位量按位宽取模。
- 浮点遵循 IEEE；编译器默认可以做不改变 NaN/Inf 语义的重排（相当于 `reassoc`+`contract`）。`[Precise]` 放在函数或局部变量上可以禁止这种变换：
  作用于该函数体内的所有运算，或该变量的初始化和赋值表达式中直接写出的运算（被调用的函数不受影响，除非它自己也是 `[Precise]`）。
- `sizeof(T)`、`nameof(x)` 是常量。

### 5.3 扩展方法与运算符重载

`public static half Luma(this half3 c)` 让 `color.Luma()` 可以调用。运算符：`public static Slab operator +(Slab a, Slab b)`。

## 6. 编译期执行

### 6.1 常量表达式

字面量、`const`、常量泛型参数、对常量的算术和比较、`sizeof`、`nameof`、`const` 函数调用、`static readonly` 的初始化。

### 6.2 const 函数

`const int Log2(int n) => n <= 1 ? 0 : 1 + Log2(n / 2);`。`const` 函数允许递归（只在编译期执行），用常量参数调用时在编译期求值，
否则当作普通函数（这时禁止递归）。编译期求值有步数上限，超出是错误。

### 6.3 常量折叠

MIR 层还会再做一次常量折叠（§11.3），但「是否是常量表达式」以本节的定义为准，决定 `static if`、数组长度、泛型实参能不能用这个值。

### 6.4 static if

```csharp
static if (T.Implements(nameof(IBsdf.Sample))) { … } else { … }
```

条件必须是常量。没被选中的分支只做语法检查，不做语义检查（所以可以引用当前实例化中不存在的成员）。

## 7. 名字解析

- 查找顺序：局部作用域 → 类型成员 → 当前命名空间及其父命名空间 → `using` 引入的命名空间 → `Marlin`。在同一层找到多个是歧义错误。
- 声明的顺序无关：顶层声明可以引用后面或其他文件中的声明。
- 类型与值共享名字空间，同一作用域里不能同名（与 C# 一致）。

## 8. 公共子集的限制

公共子集 = 本文 §1–§7 中不依赖方言的部分，再加上以下限制：不能使用纹理、采样器和加速结构（§4.4）、`Ptr`/`Span` 之外的 GPU 内存访问（`Ptr`、`Span`
可以作为共享结构的字段出现，但只有 Shader 能解引用）、`RayQuery`、光追管线与 SER 的类型和内建函数（§9.8）、`CoopVector`/`CoopMatrix`（§9.9）、
wave 与屏障内建函数、入口点属性、`[Uniform]`、`[Material]`；不能使用 Script 的 GC 类型。`half` 可以使用（CPU 端用 Core 的 `Float16`）。

## 9. Shader 方言

Shader 方言的概念模型取自 Slang 本身的设计，而不是它为兼容 HLSL / GLSL 保留的那一层；语法仍然是 C#。取舍如下：

| 采用（Slang 的原生设计） | 不采用（HLSL / GLSL 兼容层或 API 概念） |
| --- | --- |
| 入口点的 `[Uniform]` 参数，参数放在哪里由编译器决定（§9.3） | root constants、push constants、`cbuffer`、`register(t0, space1)`、`[[vk::binding]]` |
| 指针访问 GPU 内存（§4.5），描述符是句柄值（§4.4） | `StructuredBuffer`、`ByteAddressBuffer`、`RW*` 视图类型 |
| 统一的泛型纹理类型，形状、权限、格式是类型参数（§4.4） | `RWTexture2D`、`Texture2DMS` 这类各自独立的类型名，`[format("…")]` |
| interface 加关联类型、静态要求、类型位置上的 `this`、extension（§3.5） | Slang 的 existential（interface 作为值类型的动态分派） |
| 泛型入口点与链接期特化（§9.4） | 预处理器宏变体、`material` 这类专用语法 |
| capability 推导（§9.7） | 手写 shader model / profile 宏 |
| 自动微分（§9.12） | — |
| — | `SV_*` 语义字符串（用 §9.2 的属性）、HLSL 小写内建函数名（§9.10）、`mul()` 与 row/column_major 修饰 |

Slang 的 `ParameterBlock` / `ConstantBuffer` 是为了映射到描述符集合和常量缓冲区，在全 bindless 的引擎里没有对应物：需要一组参数共享时，直接传 `Ptr<T>`。

### 9.1 入口点

入口点是带阶段属性的顶层函数：

| 属性 | 阶段 |
| --- | --- |
| `[Compute(x, y, z)]` | 计算 |
| `[Amplification(x, y, z)]` | Task / Amplification |
| `[Mesh(x, y, z, MaxVertices = 128, MaxPrimitives = 256, Topology = MeshTopology.Triangle)]` | Mesh |
| `[Vertex]` | 顶点（全屏 pass、UI 等不走 mesh 管线的场合） |
| `[Pixel]` | 像素 |
| `[RayGeneration]`、`[ClosestHit]`、`[AnyHit]`、`[Miss]`、`[Intersection]`、`[Callable]` | 光追管线（§9.8） |
| `[ForEachEntity]` | 08 §5.5，由引擎生成调度的 compute |

没有几何着色器和曲面细分（07 §1 的硬件基线已经有 mesh shader）。

### 9.2 阶段输入输出

系统值用参数或字段上的属性标出，**用户定义的 varying 不需要语义名**：

```csharp
struct FullscreenVaryings
{
    [Position] public float4 Position;
    public float2 Uv;                       // 用户 varying：按字段顺序分配 location
    [NoInterpolation] public uint Layer;
}

[Vertex] FullscreenVaryings FullscreenVs([VertexId] uint id) { … }

[Pixel] [return: Target(0)] half4 Tonemap(in FullscreenVaryings v, [Uniform] in TonemapArgs args) { … }
```

- 系统值属性：`[Position]`、`[VertexId]`、`[InstanceId]`、`[DispatchThreadId]`、`[GroupThreadId]`、`[GroupId]`、`[GroupIndex]`、`[Target(n)]`、
  `[Depth]`、`[Coverage]`、`[FrontFace]`、`[SampleIndex]`、`[Barycentrics]`、`[PrimitiveId]`、`[ViewIndex]`、`[ShadingRate]`，以及光追和 mesh 的相应系统值。
  每个属性规定了允许的类型和阶段，不合法是错误。
- 插值：`[NoInterpolation]`、`[Linear]`（默认是透视校正）、`[NoPerspective]`、`[Centroid]`、`[Sample]`。
- 阶段之间按 **struct 类型**连接：顶点（或 mesh）输出的 struct 和像素输入的 struct 必须是同一个类型，location 由字段顺序决定，所以两边不可能对不上。
- Mesh 阶段的输出通过 `out` 参数 `out Vertices<V, 128> verts, out Indices<uint3, 256> tris, out Primitives<P, 256> prims` 和内建函数 `SetMeshOutputCounts(v, p)`。
  Amplification 的载荷用 `[Payload] in/out` 参数，大小上限 16 KiB；`DispatchMesh(x, y, z, payload)` 结束 amplification 阶段。
  每图元的系统值放在 `Primitives` 的元素 struct 中：`[CullPrimitive]`、`[PrimitiveId]`、`[ShadingRate]`、`[ViewportIndex]`、`[RenderTargetIndex]`。
- Mesh 的上限和拓扑来自输出的类型：`MaxVertices`、`MaxPrimitives` 是 `Vertices`、`Indices` 的 N（各不超过 256），拓扑由索引类型决定；
  `[Mesh(…)]` 里写出的同名参数必须与之一致。输出只能按元素写（`verts[i] = v`、`verts[i].Uv = uv`、`tris[i] = uint3(…)`），下标按 §4.3 钳制，
  `SetMeshOutputCounts` 之前的写入无效（硬件规则）。Mesh 读 amplification 的载荷用 `[Payload] in P payload` 参数。
- 每图元的值在像素阶段用 `[PerPrimitive] in P prim` 参数读，写在顶点输入参数之后：location 按 mesh 的输出顺序（先顶点字段，后图元字段）分配，两边一致。

### 9.3 Uniform 参数

入口点的参数分两种：**varying**（每个线程、顶点、像素各不相同：阶段输入、系统值）和 **uniform**（一次调度或绘制中对所有线程相同）。
后者用参数上的 `[Uniform]` 属性标出（对应 Slang 的入口点 uniform 参数），这是 C++ 向 shader 传值的唯一途径，语言里没有 root constants、push constants、常量缓冲区这些 API 概念：

```csharp
[Compute(16, 16, 1)]
void RasterSurface([Uniform] in Ptr<UiRasterInputs> inputs, [Uniform] in Texture2D<float4, Access.Write, ImageFormat.Rgb10a2Unorm> output,
                   [GroupId] uint3 group, [GroupThreadId] uint3 thread) { … }
```

- 一个入口点可以有任意多个 `[Uniform]` 参数，位置不限。它们按声明顺序组成一个隐含的 struct，即入口点的**参数块**，按 §10 布局；C++ 的 `TArgs`（07 §4.1）对应这个 struct。
  `[Uniform]` 和 `[GroupId]` 这类系统值属性一样是参数上的属性，不是关键字；参数写成 `[Uniform] in T name`（只读，不能是 `out`/`ref`），类型可以是任何能放进 GPU 内存的类型：标量、向量、struct、句柄、`Ptr`、`Span`。
- **放在哪里由编译器和 RHI 决定**，不是语言的一部分：参数块不超过目标的直接参数区（当前所有目标都是 128 字节）时直接传递（Vulkan 的 push data、
  D3D12 的 root constants、Metal 的参数缓冲区）；超过时，`ShaderLibrary` 把它写进每帧的上传内存，直接参数区只放指向它的 `Ptr`。反射（§12）写明是哪一种。
  所以作者按数据的意义组织参数，而不是按 128 字节拆分；热路径上想保证直接传递，就让参数块保持在 128 字节以内，大的输入放在 GPU 内存里传一个 `Ptr`。
- 一致性分析把 `[Uniform]` 参数当作 wave 内一致的值（光追的 shader record 例外，见下）。
- 同一个图形管线的各阶段共用一个参数块：每个阶段要么声明相同的 `[Uniform]` 参数序列（类型和名字都相同），要么一个都不声明。
- 光追阶段（§9.8）：`RayGeneration` 的 `[Uniform]` 参数是这次 `DispatchRays` 的参数块；`ClosestHit`、`AnyHit`、`Miss`、`Intersection`、`Callable` 的 `[Uniform]` 参数
  是 shader binding table 中这条记录的数据（不保证 wave 内一致）。
- 入口点可以是泛型的（`void Resolve<TMaterial>(…) where TMaterial : ISurfaceMaterial`），由使用者（Converter、ShaderLibrary）给出类型实参后特化，见 §9.4。

### 9.4 材质与特化

材质不是特殊语法，而是实现 `ISurfaceMaterial` 的普通 struct；pass shader 对材质类型泛型，由 Converter 为每个材质特化（Slang 的「interface + 链接期特化」）：

```csharp
// 材质：Language/Shaders/Materials/Rock.mshader
[Material, Id("9a0b…")]
public struct Rock<bool UseMoss = true> : ISurfaceMaterial
{
    public MaterialTexture2D<half4> BaseColor = DefaultTextures.White;
    [Range(0, 1)] public half Roughness = 0.5h;

    public Surface Evaluate(in SurfaceInputs input) { … }
}

// pass：Language/Shaders/Passes/MaterialResolve/Resolve.mshader
[Compute(8, 8, 1)]
void Resolve<TMaterial>([Uniform] in ResolveArgs args, [Uniform] in Span<TMaterial> materials, [DispatchThreadId] uint3 id)
    where TMaterial : ISurfaceMaterial
{
    …
    Surface surface = materials[pixel.MaterialSlot].Evaluate(inputs);
    …
}
```

- `[Material, Id(…)]` 标出一个材质（Indexer 据此索引，§1.6）。它的 **public 实例字段就是参数**：编译器按 §10 生成布局，材质表在 GPU 内存中就是这个 struct 的数组；
  反射信息（名字、类型、默认值、`[Range]`、`[Color]` 等元数据）交给编辑器生成属性面板，材质实例用 MarlinText 继承覆盖（07 §4.2）。
- **变体**用材质的泛型值参数（`<bool UseMoss = true>`，类型限于 `bool`、enum、整数，可以有默认值）。它们在编译期取值（可以选择 `Surface` 的形状），
  Converter 为每个用到的组合分别特化，取值是结果 key 的一部分；编译器统计每个材质的组合数，超出 Config 中的预算是错误。
- **特化**：引擎的 pass 用泛型入口点（§9.3）声明自己对材质的依赖；Converter 为每个「材质 × 使用材质的 pass」把 `TMaterial` 替换成具体类型后编译。
  路径追踪这类「一个 shader 处理所有材质」的场合也按材质特化（每个材质一个命中组），没有动态分派。
- `[Specialization] static bool UseFog = false;` 声明一个 specialization constant：类型是 `bool`、整数或浮点标量，必须有默认值，不能赋值。
  它在管线创建时取值，对语言来说是一致的运行时值，所以不能用在 `static if`、数组长度、泛型参数这些要求常量的地方。它用于不影响形状、只影响分支的开关；
  不只材质，任何 shader 都可以用。编译器按首次使用的顺序给每个入口点用到的常量分配 id，写进反射信息（名字 → id、类型、默认值，§12）。
- `Surface` 的形状（由哪些 `Slab`、`HorizontalMix`、`VerticalLayer` 组成）必须在编译期确定：选择不同形状只能用 `static if` 或泛型值参数；
  形状依赖运行时值是错误。形状确定以后，编译器把它展平成闭包列表（07 §4.2）。
- `Surface.Custom(new ToonBsdf { … })` 的类型参数必须实现 `IBsdf`；编译器根据实际实现了哪些 `[Optional]` 成员生成能力位，并为每个自定义 BSDF 分配闭包类型 ID（07 §4.3）。

- **编译期辅助**（核心库的 `Compiler`）：`Compiler.Require(condition, message)` 要求条件在内联、展开、折叠之后是常量 true
  （`Surface` 的形状检查就是它），否则报告 message；`Compiler.TypeId<T>()` 是 `T` 的限定名的 16 位哈希（非零，各次编译一致），
  自定义 BSDF 的闭包类型 ID；`Compiler.ToWords(value)` / `Compiler.FromWords<T>(words)` 把值的标量按字段与分量顺序放进 `uint[8]` 并读回。
- 泛型入口点的类型实参可以是普通 struct（不是 `[Material]`、没有变体），满足约束即可：例如 `LightCustom<TBsdf> where TBsdf : IBsdf`。

### 9.5 Wave 与屏障

`Wave.ActiveSum(x)`、`Wave.ActiveMin`、`Wave.Prefix*`、`Wave.ReadLaneAt(x, lane)`、`Wave.ReadFirstLane`、`Wave.IsFirstLane()`、`Wave.LaneIndex`、`Wave.LaneCount`、
`Wave.ActiveBallot(c)` 等；屏障 `Barrier.Group()`、`Barrier.GroupMemory()`、`Barrier.DeviceMemory()`、`Barrier.AllMemory()`。只能出现在相应阶段，否则是错误。

### 9.6 Groupshared

`[GroupShared] static float[256] Tile;`：顶层或类型内的静态字段，只能被 compute、amplification、mesh 入口点（传递地）使用；在其他阶段引用是错误。

### 9.7 硬件特性与档位

硬件基线（07 §1）以外的能力都是**可选特性**。没有这个机制，就没办法在语言里加入 SER、协作向量这类只有部分 GPU 支持的东西。

**特性是开放注册的，不是封闭的枚举**：

```csharp
namespace Marlin.Features;

public const Feature CooperativeVector  = Feature("CooperativeVector");
public const Feature CooperativeMatrix  = Feature("CooperativeMatrix");
public const Feature ShaderExecutionReorder = Feature("ShaderExecutionReorder");
```

- `Feature` 是一个内建的值类型，按名字区分。引擎库（`/Engine/Language/Shaders/Core/Features.mshader`）声明常用的特性，平台层和项目也可以在自己的 `Language/Shaders/` 里声明新的特性。
- 每个后端在 C++ 中注册它认识的特性名，以及每个特性需要的目标能力（例如 SPIR-V 的 capability 和扩展、DXIL 的 shader model 和 feature flag）。
  使用了某个后端不认识的特性，编译到这个后端时是错误。

**设备档位（Profile）**：

- 档位 = 一个后端 + 一组特性，定义在 `Config/ShaderProfiles.mtext` 中，平台层可以添加或覆盖（06 §1.4）。例如：

  ```
  ShaderProfile { !Name = "Vulkan.Baseline"  Backend = Spirv  Features = [] }
  ShaderProfile { !Name = "Vulkan.Ada"       Backend = Spirv  Features = [CooperativeVector, ShaderExecutionReorder] }
  ShaderProfile { !Name = "D3D12.Baseline"   Backend = Dxil   Features = [] }
  ```

- 主机通常只有一个档位。PC 上 Converter 为每个档位分别编译，档位是结果 key 的一部分（06 §3.2）。RHI 启动时根据设备支持的能力选择满足条件的最高档位。
- 基线（mesh shader、Ray Query、光追管线、fp16、int16、int64、bindless 堆）在所有档位中都可用，不需要声明。

**在代码中使用**：

```csharp
[Requires(Features.CooperativeVector)]
half4 EvaluateNetwork(in NetworkWeights w, in CoopVector<half, 32> input) { … }

half4 Shade(…)
{
    static if (Device.Supports(Features.CooperativeVector))
        return EvaluateNetwork(weights, input);
    else
        return EvaluateNetworkScalar(weights, input);   // 用普通 shader 代码写的回退
}
```

- `Device.Supports(f)` 在每个档位中都是编译期常量。
- `[Requires(f, …)]` 可以放在函数、struct 和入口点上。需要特性的内建类型和内建函数（`HitObject`、`CoopVector`、`CoopMatrix` …）隐含了 `[Requires]`。
- **推导**（Slang 的 capability 推导）：非 `public` 的函数不需要写 `[Requires]`，编译器从函数体推导它需要的特性（它用到的声明的要求之并，减去
  `static if (Device.Supports(F))` 分支里已经满足的部分）。
- **检查规则**：`public` 函数和入口点是边界，推导出的要求必须被声明的 `[Requires]` 覆盖，否则是错误，诊断指出是哪一处调用引入了 F。
  所以库的 API 写明自己的要求，内部函数不必逐个标注；缺少回退路径仍然在编译期发现，而不是到了某台机器上才出问题。
- 带 `[Requires]` 的入口点只为满足条件的档位编译；反射（§12）会说明这个入口点在哪些档位上不可用，由引擎决定用哪个 pass 替代它。

### 9.8 光追管线

基线中的光追有两种用法：Ray Query（§4.6，07 中阴影、反射、AO 的主路径）和这里描述的光追管线（以后的路径追踪器，07 §3）。

**阶段签名**：

```csharp
[RayPayload]    struct RadiancePayload { public half3 Radiance; public uint Depth; }
[HitAttributes] struct SphereAttributes { public float3 Normal; }      // 不超过 32 字节

[RayGeneration] void Trace([Uniform] in TraceArgs args) { … }
[ClosestHit]    void Shade(ref RadiancePayload payload, in TriangleAttributes attr, [Uniform] in HitRecord record) { … }
[AnyHit]        void AlphaTest(ref RadiancePayload payload, in TriangleAttributes attr) { if (…) IgnoreHit(); }
[Miss]          void Sky(ref RadiancePayload payload) { … }
[Intersection]  void Sphere() { … ReportHit(t, kind: 0, new SphereAttributes { … }); }
[Callable]      void Evaluate(ref CallableData data) { … }
```

- **载荷和命中属性按类型连接**，与 varying 的规则一样（§9.2）：载荷是 `[RayPayload]` struct，命中属性是 `[HitAttributes]` struct 或内建的 `TriangleAttributes`（重心坐标）。
  反射输出每个入口点的载荷类型和属性类型（以类型的全限定名和布局哈希标识）；引擎创建光追管线时检查同一个管线中的阶段是否一致，不一致就拒绝创建。
- **发射光线**：`TraceRay(scene, ray, ref payload, flags: RayFlags.None, mask: 0xFF, hitGroupOffset: 0, hitGroupStride: 1, missIndex: 0)`，
  其中 `ray` 是 `RayDesc { Origin, TMin, Direction, TMax }`。可以在 RayGeneration、ClosestHit、Miss 中调用。`CallShader(index, ref data)` 调用 Callable。
- **AnyHit / Intersection 的控制**：`IgnoreHit()`、`AcceptHitAndEndSearch()`、`ReportHit(t, kind, attributes)`。
- **系统值**是只读的静态属性，只能在相应阶段使用：`Launch.Index`、`Launch.Size`；`Hit.T`、`Hit.Kind`、`Hit.InstanceIndex`、`Hit.InstanceId`、`Hit.GeometryIndex`、
  `Hit.PrimitiveIndex`、`Hit.ObjectToWorld`、`Hit.WorldToObject`、`Hit.WorldRayOrigin`、`Hit.WorldRayDirection`、`Hit.ObjectRayOrigin`、`Hit.ObjectRayDirection`、`Hit.RayTMin`、`Hit.RayFlags`。
- **局部数据**：命中、Miss、Callable 阶段的 `[Uniform]` 参数就是 shader binding table 中这条记录的数据（§9.3）。在 bindless 下它通常只放材质的 `Ptr` 之类的少量数据，按 §10 布局。
- **阶段接口的实现**：载荷、callable 数据以 `ref` 参数出现，入口点在调用前后与 IncomingRayPayload / IncomingCallableData 变量互相复制；`TraceRay`、`CallShader`、
  `HitObject.TraceRay` 和 `Invoke` 的 `ref` 实参同样经由每个类型一个的 RayPayload / CallableData 变量传递。命中属性以 `in` struct 参数出现；`ReportHit` 写入 HitAttribute 变量，
  所以一个 Intersection 入口点只能报告一种属性类型。
- **递归**：在 ClosestHit、Miss 中再调用 `TraceRay` 是递归发射。反射标出每个入口点是否会（传递地）发射光线，管线的最大递归深度由引擎在创建管线时设置；
  语言本身不限制，但引擎的默认上限是 2。载荷大小上限由 Config 设定（默认 32 字节），超出是错误。

**SER（Shader Execution Reordering）**，需要 `Features.ShaderExecutionReorder`：

```csharp
[RayGeneration]
void PathTrace([Uniform] in TraceArgs args)
{
    RadiancePayload payload = default;
    static if (Device.Supports(Features.ShaderExecutionReorder))
    {
        HitObject hit = HitObject.TraceRay(args.Scene, ray, ref payload);   // 只求交，不执行命中着色器
        ReorderThread(hit, coherenceHint: materialBucket, hintBits: 4);
        hit.Invoke(ref payload);                                             // 重排之后再执行 ClosestHit / Miss
    }
    else
    {
        TraceRay(args.Scene, ray, ref payload);
    }
}
```

- `HitObject` 只能是局部变量，**不能复制**，不能存入缓冲区或载荷：它只能在声明或赋值处就地构造——`HitObject.TraceRay(…)`、`HitObject.MakeMiss(missIndex, ray)`、
  `HitObject.MakeNop()`，或者从 ray query 的已提交命中得到 `query.CommittedHitObject(shaderTableIndex)`（命中组的 shader table 记录要显式给出，SPIR-V 与 DXIL 都需要它）。
- 查询是方法：`IsHit()`、`IsMiss()`、`IsNop()`、`InstanceIndex()`、`InstanceId()`、`GeometryIndex()`、`PrimitiveIndex()`、`HitKind()`、`HitT()`、`RayTMin()`、
  `TraversalFlags()`、`WorldRayOrigin()` … `ObjectToWorld()`、`WorldToObject()`、`ShaderTableIndex()`，与 `Hit.*` 相对应。读取 shader record 只在命中着色器中通过 `[Uniform]` 参数进行。
- `ReorderThread(hit)`、`ReorderThread(hit, coherenceHint, hintBits)`、`ReorderThread(coherenceHint, hintBits)` 只能在 RayGeneration 中调用；`HitObject` 可以在 RayGeneration、ClosestHit、Miss 中使用。
- 后端：SPIR-V 映射到 `SPV_EXT_shader_invocation_reorder`，DXIL 映射到 SM 6.9 的 SER 操作。

### 9.9 协作向量与协作矩阵

用于在 shader 中运行小型神经网络（神经材质、神经纹理压缩、降噪等）和通用的矩阵运算。两者都是可选特性。

**协作向量**，需要 `Features.CooperativeVector`：

```csharp
[Requires(Features.CooperativeVector)]
CoopVector<half, 16> Layer(in CoopVector<half, 32> input, Ptr<byte> weights, uint offset, uint biasOffset)
{
    var w = new MatrixRef<half, 16, 32>(weights.Offset(offset), MatrixLayout.InferencingOptimal);
    var b = new VectorRef<half, 16>(weights.Offset(biasOffset));
    return Max(MatMulAdd(w, input, b), CoopVector<half, 16>(0));      // ReLU
}
```

- `CoopVector<T, int N>`：每个线程一个向量，N 可以远大于 4（上限由后端注册的特性决定）。元素类型：`half`、`float`、`int`、`uint`，
  以及只用于矩阵和输入解释的打包类型（`MatrixElement.Float8E4M3`、`Float8E5M2`、`Int8` 等）。
- 按分量的算术、比较、`Max`/`Min`/`Clamp`/`Exp`/`Tanh` 等内建函数都对它重载；从数组或 GPU 内存加载和存储：`CoopVector<half, 32>.Load(ptr)`、`v.Store(ptr)`。
- 矩阵和偏置放在 GPU 内存中，通过 `MatrixRef<T, int M, int K>(ptr, layout, stride = 0)` 和 `VectorRef<T, int N>(ptr)` 引用（`ptr` 是 `Ptr<byte>`）；
  `MatrixLayout` 有 `RowMajor`、`ColumnMajor`、`InferencingOptimal`、`TrainingOptimal`。最优布局之间的转换由 RHI 在上传时完成，不在 shader 里。
- 运算：`MatMul(matrix, input)`、`MatMulAdd(matrix, input, bias)`；训练用的 `OuterProductAccumulate(a, b, matrix)`、`ReduceSumAccumulate(v, ptr)` 需要另一个特性 `Features.CooperativeVectorTraining`。
- 没有这个特性时的回退：引擎库提供用普通 shader 代码写的同名函数（`Marlin.Ml.Scalar.MatMulAdd`），配合 `static if` 使用（§9.7）。
- 后端：SPIR-V 映射到 `SPV_NV_cooperative_vector`，DXIL 映射到 SM 6.9 的长向量与线性代数操作。

**协作矩阵**，需要 `Features.CooperativeMatrix`，只能在 compute 中使用：

```csharp
[Requires(Features.CooperativeMatrix)] [Compute(128, 1, 1)]
void Gemm([Uniform] in GemmArgs args, [GroupId] uint3 group)
{
    var acc = CoopMatrix<float, 16, 16, MatrixUse.Accumulator>(0);
    for (uint k = 0; k < args.K; k += 16)
    {
        var a = CoopMatrix<half, 16, 16, MatrixUse.A>.Load(args.A + offsetA, stride: args.K, MatrixLayout.RowMajor);
        var b = CoopMatrix<half, 16, 16, MatrixUse.B>.Load(args.B + offsetB, stride: args.N, MatrixLayout.RowMajor);
        acc = MulAdd(a, b, acc);
    }
    acc.Store(args.C + offsetC, stride: args.N, MatrixLayout.RowMajor);
}
```

- `CoopMatrix<T, int M, int N, MatrixUse U>`：一个 wave 共同持有的矩阵；`MatrixUse` 是 `A`、`B`、`Accumulator`。
  合法的 (T, M, N) 组合由设备决定，反射记录用到的组合，RHI 在选档位时检查。
- 运算：`Load`、`Store`、`MulAdd(a, b, c)`、按元素的算术、标量乘法、类型转换、`Length`（每个线程持有的元素数）和按下标访问本线程持有的元素。
- 协作矩阵的变量必须在 wave 内一致地执行（不能出现在可能发散的分支中）；一致性分析（§11.1）检查这一点，违反是错误。
- 后端：SPIR-V 映射到 `SPV_KHR_cooperative_matrix`；DXIL 的对应操作取决于 shader model 的进展，在后端注册之前，D3D12 的档位不提供这个特性。

### 9.10 标准库

标准库（`Marlin` 命名空间）**用语言本身写成**（与 Slang 的 core module 相同的做法），是引擎 `Language/` 下的普通源文件，称为**核心库**：
`/Engine/Language/Common/Core/Marlin.mcommon`（标量与向量数学、内建接口、常用 struct 与 enum）和 `/Engine/Language/Shaders/Core/Marlin.mshader`
（资源、wave、屏障、光追、mesh、协作类型）。它不内嵌在编译器里：源码引擎和安装版引擎（02 §1）都带着 `Language/`，编译器、Converter 和
`MarlinLanguageServer`（01、10）通过同一个 `/Engine` 挂载点读取它，所以 LSP 对标准库的补全、悬停文档和跳转定义与对普通代码完全一样。
引擎自己的 shader 库（`Marlin.Shading` 的材质类型、`Passes/` 的 pass shader、compute 用的并行原语）放在 `Language/Shaders/` 的其他目录中，是普通代码。
源集里缺少核心库时编译直接报错。编译器只认得少数几个属性：

| 属性 | 含义 |
| --- | --- |
| `[Builtin("Vector")]` | 这个 struct 是编译器内建的**魔法类型**族（`Vector`、`Matrix`、`Texture`、`Ptr`、`Span`、`CoopVector` …）。字段由编译器决定，成员函数在序言里声明 |
| `[Intrinsic("Sin")]` | 没有函数体的函数（或魔法类型的成员函数），调用时生成同名的 MIR 内建操作（`MirIntrinsic` 或 `MirOp`）；各后端负责映射到目标指令（§11.3），**语言里不出现任何目标汇编** |
| `[SystemValue("WaveLaneIndex")]` | 没有函数体的静态只读属性，读取同名的 MIR 系统值 |
| `[CompileTime("DeviceSupports")]` | 只在编译期求值的函数（`Device.Supports`、`Feature("…")`、`T.Implements`） |
| `[Stage(ShaderStage.Pixel)]` | 只能在这些阶段（传递地）使用（§9.5、§9.6 的阶段规则就是这样声明的） |
| `[Requires(Features.X)]` | §9.7 的特性要求 |

例如：

```csharp
public using float3 = Vector<float, 3>;

[Intrinsic("Sin")] public T Sin<T>(T x) where T : IFloatingPoint;
[Intrinsic("Dot")] public T Dot<T, int N>(Vector<T, N> a, Vector<T, N> b) where T : INumeric;

public struct Derivative
{
    [Intrinsic("Ddx"), Stage(ShaderStage.Pixel)] public static T X<T>(T x) where T : IFloatingPoint;
}

[Builtin("Texture2D")]
public struct Texture2D<T, Access A = Access.Read, ImageFormat F = ImageFormat.Inferred> where T : IScalarOrVector
{
    [Intrinsic("Sample"), Stage(ShaderStage.Pixel)] public T Sample(Sampler s, float2 uv);
    [Intrinsic("SampleLevel")] public T SampleLevel(Sampler s, float2 uv, float lod);
    [Intrinsic("TextureDimensions")] public uint2 Size { get; }
}

public struct Wave
{
    [SystemValue("WaveLaneIndex")] public static uint LaneIndex { get; }
    [Intrinsic("WaveActiveSum")] public static T ActiveSum<T>(T value) where T : INumeric;
}
```

- **内建接口**：`IScalar`、`INumeric`、`IInteger`、`IFloatingPoint`、`ILogical`、`IScalarOrVector`。标量和向量按它们的标量类型自动满足（`float3` 满足 `IFloatingPoint`），
  所以按分量重载的数学函数只需要一个泛型声明。核心库用一个 extension（§3.5）给所有 `INumeric` 类型加上 `T.Zero`、`T.One`，所以泛型代码可以直接写 `a + b * T.One`。
- **推导与字面量**：泛型实参先从非常量实参推导，常量实参之后再按 §2.4 / §4.7 转换（标量常量可以广播成向量），所以 `Max(color, 0.0)` 对 `half3 color` 推出 `T = half3`。
- 数学函数是 `Marlin` 命名空间的顶层函数（`Marlin` 始终可见，§1.2），按项目的 C# 命名规则用 PascalCase，不沿用 HLSL 的小写名字，直接写
  `Lerp(a, b, t)`。与 HLSL 名字不同的部分：

  | 名字 | 说明（括号内是 HLSL 的旧名，仅供对照） |
  | --- | --- |
  | `Abs`、`Min`、`Max`、`Clamp`、`Saturate`、`Lerp`、`Step`、`SmoothStep`、`Floor`、`Ceiling`、`Round`、`Truncate`、`Sqrt`、`Exp`、`Exp2`、`Log`、`Log2`、`Pow`、`Sin` … `Atan2` | 同名改大小写 |
  | `Fract`（`frac`）、`InverseSqrt`（`rsqrt`）、`FusedMultiplyAdd`（`fma`）、`Ldexp` | |
  | `Dot`、`Cross`、`Length`、`Normalize`、`Distance`、`Reflect`、`Refract`、`FaceForward`、`Transpose`、`Determinant`、`Inverse` | |
  | `IsNaN`（`isnan`）、`IsInfinity`（`isinf`） | |
  | `All`、`Any`、`Select(c, a, b)` | |
  | `PopCount`（`countbits`）、`LeastSignificantBit`（`firstbitlow`）、`MostSignificantBit`（`firstbithigh`）、`ReverseBits` | 没有置位时 `…SignificantBit` 返回全 1 |
  | `AsFloat`、`AsUInt`、`AsInt`、`AsLong`、`AsULong`、`AsDouble`（`asfloat` …） | 按位重新解释，大小必须相同 |
  | `PackHalf2x16(v)` / `UnpackHalf2x16(u)`（取代 `f32tof16`/`f16tof32`） | |
  | `Derivative.X(v)`、`Derivative.Y(v)`、`Derivative.Width(v)`、`Derivative.XFine` / `YFine` / `XCoarse` / `YCoarse`（`ddx`/`ddy`/`fwidth` …） | 只在像素阶段 |
- 方言规则对核心库同样成立：`.mcommon` 只能引用公共子集的声明，`Marlin.mshader` 中的声明只对 Shader 文件可见。
- 只有 `/Engine/Language/<方言>/Core/` 中的文件可以使用 `[Builtin]`，以及声明没有函数体的 `[Intrinsic]` / `[SystemValue]` / `[CompileTime]` 成员。

### 9.11 调试输出

```csharp
Debug.Print("tile {0}: {1} lights, normal {2}", tileId, count, normal);
Debug.Assert(count <= MaxLights, "too many lights");
```

- `Debug.Print(format, a0, … a3)`：`format` 必须是常量字符串，`{n}` 引用第 n 个参数，`{n:x}` 以十六进制输出整数，`{{` / `}}` 是花括号本身；
  参数是标量或向量（bool、整数、浮点）。引用不存在的参数、参数没被引用都是错误。
- `Debug.Assert(condition, message)`：条件为假时输出 `file:line: assertion failed: message`。**GPU 不会因此停下**——任何目标都没有能中止 shader 的断言，
  所以它就是「条件为假时打印」，在前端展开成 `if` 加 `Debug.Print`，目标不需要另外支持。
- 只有编译选项打开调试输出时才生成代码（`LanguageCompileOptions.DebugOutput`，编辑器和 Development 构建默认打开）；关闭时整条调用连同参数一起删掉，参数不会被求值。
- MIR 上是与目标无关的 `DebugPrint` 指令：格式字符串放在模块的字符串表里，按编号引用，并写进反射信息（§12），由运行时用来还原文本。各目标在 §11.4 的表里声明怎么处理：
  - SPIR-V、DXIL 等没有随时可用的输出的目标：通用 Expand（`MirExpandDebugPrintPass`）改写成「向调试缓冲区追加一条记录」。
    SPIR-V 不用 `NonSemantic.DebugPrintf`：它只有在 Vulkan 验证层、RenderDoc、Nsight 改写 shader 时才回到 CPU，而调试缓冲区随时都能读到。
  - Metal（MSL 3.2 及以上）：映射到 shader 内的 `os_log`。
- **调试缓冲区的地址**：前端降级用到调试输出的入口点时，它的 Uniform 全局是 `{ 参数（或间接参数块的 Ptr）; DebugBuffer }`，
  `DebugBuffer` 是缓冲区地址，紧跟在参数之后（8 字节对齐）；没有 `[Uniform]` 参数的入口点也有这个全局，里面只有地址。
  反射写明它在直接传入的参数里的偏移（`DebugBufferOffset`），参数块本身的字段和大小照旧，C++ 端的参数 struct 不变。
  打开调试输出时，参数块能直接传入的上限少 8 字节（120），为地址留出位置，所以一条管线里打印和不打印的阶段对同一份参数的直接/间接选择一致。
- **记录格式**（32 位字）：缓冲区开头是已追加的字数和可用的字数，然后是记录，最后是放不下的记录改写进去的废弃区（64 个字）。
  一条记录是：记录的字数、格式的哈希（格式文本 XXH3-64 的低 32 位），然后每个参数一个描述字（种类：无符号、有符号、浮点、bool；
  标量位数；分量数）加每个分量一个字，64 位的两个字、低位在前，half 先扩成 float。追加用一次原子加法占位，不分支：放不下就写进废弃区，
  CPU 读到字数超出可用字数时报告丢弃了多少。
- **运行时**：`ShaderLibrary` 在 Readback 内存里轮换 4 个调试缓冲区（`Engine.Shaders.DebugOutputWords` 个字，默认 65536），
  每次 dispatch 或 draw 把当前缓冲区的地址写在参数之后。在提交帧开始时（`RhiSubsystem` 的提交帧回调，即上一帧的命令全部提交之后）
  记下上一个缓冲区的退役值，GPU 完成后读出记录、按格式哈希找到已加载变体的格式文本、解码；同一帧里相同的消息合并成一行并注明次数，
  每帧最多 32 条不同的消息。失败的断言以 Error、`Debug.Print` 以 Info 写进 `LogShader`。
- `MarlinConverter` 给开发构建编译全局 shader 时打开 `DebugOutput`、`CheckSpans` 和调试信息（08 §7）；打包用 `Packager` 自己的选项。

### 9.12 自动微分

Slang 的前向 / 反向自动微分，导数函数在 MIR 上生成（对 SSA 做前向和反向变换），所有后端都直接得到导数代码：

```csharp
public struct Material
{
    public float3 Albedo;
    public float Roughness;
    [NoDiff] public uint Layer;                 // 不求导：导数中恒为 0
}

[Differentiable]
float Shade(Material m, [NoDiff] float exposure, uint count)
{
    float sum = 0;
    [MaxIterations(16)]                         // 反向模式要保存每次迭代的值
    for (uint i = 0; i < count; i++) { sum = sum * m.Roughness + Square(Dot(m.Albedo, m.Albedo)); }
    return sum * exposure + Autodiff.Detach(m.Roughness) * m.Roughness;
}

DifferentialPair<float> d = Autodiff.Forward(Shade, new DifferentialPair<Material>(m, dm), exposure, count);

var g = new DifferentialPair<Material>(m, Material.DifferentialZero);
Autodiff.Backward(Shade, ref g, exposure, count, dResult);   // 梯度写进 g.Differential
```

- `[Differentiable]` 函数可以求前向（`Autodiff.Forward`）和反向（`Autodiff.Backward`）导数，与 Slang 的 `fwd_diff` / `bwd_diff` 同义。
  `DifferentialPair<T>`（`Primal`、`Differential`）是值和导数，对应 Slang 的同名类型。参数按值（或 `in`）传递：传 `DifferentialPair` 的参数参与求导，
  其余参数直接传值、不求导。`Autodiff.Backward` 中以 `ref` 传入的 pair 接收梯度（替换它的 `Differential`），最后一个实参是 dResult（类型是结果的 `Differential`）。
- **`IDifferentiable`**（Shader 核心库）：

  ```csharp
  public interface IDifferentiable
  {
      type Differential : IDifferentiable;                                     // 导数的类型
      static abstract Differential DifferentialZero { get; }                    // 零导数
      static abstract Differential DifferentialAdd(Differential a, Differential b);  // 导数相加（值被用了两次时）
      static abstract Differential DifferentialScale(Differential d, float s);  // 导数乘以标量
  }
  ```

  由编译器决定谁实现它：浮点标量、向量和矩阵，它们的数组，以及字段都可微或标了 `[NoDiff]`、并且至少有一个可微字段的 struct——不需要声明。
  struct 写 `: IDifferentiable` 只是让编译器检查这一点，有不可微字段时报错（M3028）并提示加 `[NoDiff]`。各项要求由核心库的
  `extension<T> T where T : IDifferentiable` 统一提供（加法和缩放按字段、元素逐个生成），泛型代码写 `T.Differential`、`T.DifferentialAdd(…)`。
  `DifferentialPair<T>` 的约束是 `T : IDifferentiable`。
- **`Differential` 就是类型本身**：Slang 为含不可微字段的 struct 合成一个只有可微字段的新类型；这里不合成，`[NoDiff]` 字段和其它不可微字段
  （整数等）在导数中恒为 0。这样 `DifferentialPair<T>` 是同布局的两份 `T`，MIR 上导数与原值同类型，变换不需要在两种类型之间转换。
  代价是导数多带了这些恒为 0 的字段，以及不能为 struct 自定义 `Differential`。
- **`[NoDiff]`**：写在参数上，该参数不求导、直接传值，给它传 `DifferentialPair` 是错误（M3030）；函数被内联进别的可微函数时，导数也在这个参数处截断。
  写在 struct 字段上，导数不经过这个字段。
- **`Autodiff.Detach(x)`**：值是 `x`，导数为 0，用来截断梯度；参数必须可微，只在 Shader 方言中可用。MIR 上是 `Detach` intrinsic，求导 pass 之后换成它的操作数。
- **可微代码中的调用**：`[Differentiable]` 函数调用的函数必须有导数——标了 `[Differentiable]`（与调用者一起变换），或者有自定义导数——否则报错（M3028）。
  不受此限的调用：核心库函数和内建操作（没有导数规则的在生成导数时报错）、构造函数和属性访问器、没有带导数的非常量实参或者没有带导数的结果
  （返回值、`ref` / `out` 参数）的调用，以及写在 `Autodiff.Detach(…)` 里的调用。
- **两个方向的前向模式**：`DifferentialPair2<T>`（`Primal`、`Dx`、`Dy`）让一次求值同时带两个方向的导数（比如屏幕 x 和 y）；
  参数全部用 `DifferentialPair2` 时 `Autodiff.Forward` 返回 `DifferentialPair2`，只用于前向模式。每条规则对导数是线性的，
  所以规则的系数只算一次，用于每个方向。`[ForwardDerivative2(nameof(F))]` 的自定义导数同时拿到两个方向（例如贴图采样拿到 UV 的
  两个方向导数调用 `SampleGrad`）；没有它的函数按方向各调一次 `[ForwardDerivative]`。
- **对方法求导**：`Autodiff.Forward(material.Evaluate, …)` 通过对象命名实例方法，对象作为不求导的 `this`；泛型代码里同样可用
  （单态化之后是具体类型的方法）。实现某个标了 `[Differentiable]` 的 interface 成员的方法本身就是可微的（`ISurfaceMaterial.Evaluate`）。
- **自定义导数**：

  ```csharp
  [ForwardDerivative(nameof(SquareForward)), BackwardDerivative(nameof(SquareBackward))]
  float Square(float x) => AsFloat(AsUInt(x * x));             // 本身不可微也可以

  DifferentialPair<float> SquareForward(DifferentialPair<float> x)
      => new DifferentialPair<float>(Square(x.Primal), 2 * x.Primal * x.Differential);
  void SquareBackward(ref DifferentialPair<float> x, float dResult)
  {
      x = new DifferentialPair<float>(x.Primal, 2 * x.Primal * dResult);
  }
  ```

  对调用 F 的代码求导时调用 F 的自定义导数，不变换 F 的函数体。F 是非泛型的静态或顶层函数，返回可微类型。F 的每个按值传递、可微且不是 `[NoDiff]` 的参数，
  在前向导数中是 `DifferentialPair<T>`，在反向导数中是 `ref DifferentialPair<T>`（写入梯度）；其余参数原样保留。前向导数返回 `DifferentialPair<R>`；
  反向导数最后多一个 `R dResult`，返回 `void`。名字从 F 所在的作用域查找，找不到或签名不符时报错（M3029），并写出期望的签名。
  F 只有一种自定义导数时，在另一种模式下求导是错误。调用 F 的代码在求导完成之前保留这个调用（内联 pass 跳过 F），之后照常内联。
- **循环**：前向模式适用于任何结构化循环。反向模式先重放原函数：每个需要反向的循环记录执行次数，反向传播要用到的每次迭代的值存进函数局部数组
  （每个值一个数组，每次迭代一格，嵌套循环的值按各层迭代的组合编号，内层循环的次数按外层迭代保存），然后用一个倒数的循环逐次反向执行循环体。
  数组的大小由循环上的 `[MaxIterations(n)]` 给出（1 ≤ n ≤ 65536，写法同 `[Unroll]`）；循环次数是编译期常量时可以不写。需要保存值、又没有上界的循环是错误，
  报在循环上：``cannot differentiate `F` (reverse mode): the loop needs `[MaxIterations(n)]` …``。实际次数超过 n 时导数未定义（下标被截断，不会越界）。
  不需要保存值的循环（例如只做累加）不需要上界。
- 反向模式的限制：循环只能从条件处退出（`for` / `while` 在循环体之前判断，`do` 在之后），`break` / `continue` 是错误；`switch` 在两种模式下都不支持。

## 10. 布局

- GPU 内存、入口点参数块和 shader record 中的布局只有一种（不像 Slang 那样按缓冲区选 std140/std430/scalar）：标量按自身大小对齐；`N` 分量向量按「元素大小 × (N == 3 ? 4 : N)」对齐，
  大小是元素大小 × N（所以 `float3` 后面可以紧跟一个 `float`）；矩阵按行存储，每行是一个向量；数组元素的步长是元素大小向上对齐到元素对齐；
  struct 的对齐是成员中最大的对齐，大小向上取整到对齐。
- `Ptr<T>` 是 8 字节、按 8 对齐；`Span<T>` 是 16 字节、按 8 对齐（指针、`uint` 长度、4 字节填充）；纹理、采样器、加速结构的句柄是 4 字节。
- `[StructLayout(Size = n)]` 和字段上的 `[Offset(n)]` 用于和 C++ 结构精确对齐；违反对齐规则是错误。
- 反射（§12）输出每个 struct 的布局，C++ 端可以用 `static_assert` 校验共享结构。

## 11. 编译器结构

### 11.1 LanguageFrontend

```
ISourceProvider ──► 声明扫描（每个文件）──► 声明索引
                                │
入口点 + 泛型实参（材质类型）──► 按需解析与语义检查 ──► 单态化 ──► 降低到 MIR
```

- **Lexer**：零拷贝，记号带文件和字节偏移；行列号按需计算。
- **Parser**：手写递归下降，方言开关决定哪些上下文关键字和语法可用；错误恢复以语句和声明为单位，一次编译报告尽可能多的错误。AST 分配在 Arena 中。
- **语义检查**：名字解析、类型检查、重载决议、确定赋值、常量求值（AST 解释器）、方言限制、阶段限制（§9）、一致性分析、`Surface` 形状检查。
- **单态化**：泛型函数和类型按实参实例化，结果缓存，键是「声明 + 实参」。
- **诊断**：格式仿 rustc：`error[M3001]: message`，`--> path:line:col`，带行号的源码摘录，主标签用 `^^^`、次标签用 `---` 标出并附说明，最后是 `= note:` / `= help:`。错误码稳定，供 LSP 和测试使用。

前端的产物是「已检查的程序」：入口点和它传递引用到的、已单态化的函数与类型。LSP 复用同一套前端（增量重新扫描单个文件）。

### 11.2 LanguageIR（MIR）

- **类型**：`void`、`bool`、整数（宽度 8/16/32/64，带符号性）、浮点（16/32/64）、向量、矩阵、定长数组、struct、指针（带地址空间：Function、
  Private、GroupShared、Device、Uniform、ShaderRecord、Input、Output）、资源句柄、采样器句柄、加速结构、RayQuery。`Ptr<T>` 是 Device 地址空间的指针；
  入口点的参数块是 Uniform 地址空间（光追命中阶段是 ShaderRecord）中的一个全局变量。类型唯一化，可以按指针比较。
- **值与指令**：SSA。函数由基本块组成，基本块以终结指令结束（`Branch`、`CondBranch`、`Switch`、`Return`、`Discard`、`Unreachable`）。
- **结构化控制流**：选择结构和循环结构显式带 merge 块和 continue 块（与 SPIR-V 的结构化规则相同）；所有 pass 必须保持结构化。这样三个后端都不需要重新做结构化分析。
- **内建操作**直接是 MIR 指令（纹理采样、原子、wave、屏障、导数 …），后端各自映射。
- **通用 pass**：merge-return（把多个 `return` 改成结构化的单出口）、内联（Shader 默认全部内联）、SROA、mem2reg、常量折叠、死分支消除、CSE 与复制传播、
  DCE、CFG 简化（保持结构化）、循环展开（`[Unroll]` / `[Unroll(n)]`）、一致性分析。每个 pass 之后可以跑验证器（Debug 构建默认开启）。
- **循环展开**：`[Unroll]` / `[Unroll(n)]` 写在 `for`、`foreach`、`while`、`do` 前面。`[Unroll]` 的循环如果只从条件处退出、归纳变量从常量开始按常量步进并和常量比较，
  并且次数不超过 1024，就在 MIR 上完全展开；否则把请求原样交给目标（SPIR-V 的 `Unroll` loop control）。`[Unroll(n)]` 只交给目标（`PartialCount n`）。
- **优化只在 MIR 上做一次**：spirv-opt 做的那些优化与目标无关，在 MIR 上做完后 DXIL、Metal 和主机后端都直接受益。后端不再做通用优化，只做目标相关的合法化
  （存储类、描述符堆访问方式等）、按实际用到的指令声明 capability 与扩展、紧凑分配 id、保留或去掉调试信息（§11.3）。
- **Pass 管理**（仿 LLVM 新 pass manager）：
  - pass 分两级：`IMirFunctionPass::Run(MirFunction&, MirFunctionAnalysisManager&) → MirPreservedAnalyses`，`IMirModulePass` 同理作用于整个模块。
  - `MirFunctionPassManager` / `MirModulePassManager` 按顺序运行一串 pass；`MirModuleToFunctionAdaptor` 把一个函数级 pass 管理器套到模块的每个函数上。
  - **分析**（控制流与支配树、一致性、调用图 …）由分析管理器按「分析 ID × 函数/模块」惰性计算并缓存。pass 用 `GetResult<TAnalysis>(function)` 取结果，
    用返回的 `MirPreservedAnalyses` 声明保留了哪些分析（`All()`、`None()`、`Preserve<TAnalysis>()`、`PreserveSet<MirCfgAnalyses>()`），管理器据此失效其余缓存。
    模块 pass 改动了函数时，模块级分析管理器通过代理把失效传给函数级。
  - 分析和 pass 都是开放注册的：分析用自己类型的唯一 ID 区分（`MirAnalysisKey<T>`），不需要中心枚举。
  - 流水线由代码或文本描述（`"function(mem2reg,sroa,constfold,dce),inline,function(simplifycfg)"`）构造，测试可以只跑单个 pass；`VerifyEach` 选项在每个 pass 之后运行验证器。
- MIR 是 Shader 和 Script 共用的，Script 专有的指令（GC 分配、虚调用等）在 Script 章节中定义。

### 11.3 ShaderBackendSpirv

- 目标：SPIR-V 1.6，Vulkan 1.4（07 §1），基线扩展：`VK_EXT_descriptor_heap`（`SPV_EXT_descriptor_heap`、`SPV_KHR_untyped_pointers`）、`VK_EXT_mesh_shader`、
  `VK_KHR_ray_query`、`VK_KHR_ray_tracing_pipeline`、`bufferDeviceAddress`（Vulkan 1.2 起是核心）、`shaderFloat16`、`shaderInt16`、`shaderInt64`。
- 可选特性（§9.7）的注册：`ShaderExecutionReorder` → `SPV_EXT_shader_invocation_reorder`；`CooperativeVector`、`CooperativeVectorTraining` → `SPV_NV_cooperative_vector`；
  `CooperativeMatrix` → `SPV_KHR_cooperative_matrix`。模块只声明实际用到的 capability 和扩展。
- **Bindless**：资源堆和采样器堆是 `BuiltIn ResourceHeapEXT` / `SamplerHeapEXT` 的 untyped 变量；句柄 `h` 的访问是以 `OpConstantSizeOfEXT`（描述符大小）为步长的
  untyped access chain，再按资源类型 load 出 image / sampler / 加速结构。缓冲区不经过描述符堆。
- **指针**：`Ptr<T>` 的值是 64 位整数（设备地址），访问时 `OpConvertUToPtr` 成 `PhysicalStorageBuffer` 存储类的**有类型**指针（`PhysicalStorageBufferAddresses`
  capability，寻址模型 `PhysicalStorageBuffer64`），指向 Block struct `{ T data[]; }`，再用有类型的 `OpAccessChain` 走到元素和成员；pointee 用 §10 的显式布局类型
  （struct 按 FlatStruct 展平，见下）。不用 untyped 指针：NVIDIA 的 pipeline 编译器（2026-10 的驱动）编译 `PhysicalStorageBuffer` 上的 untyped access chain 时崩溃。
  经由设备地址的 `OpLoad` / `OpStore` 都带 `Aligned` 内存操作数，值是被访问类型按 §10 的对齐。`Ptr<byte>` 的 `Read` / `Write` 按 32 位字访问。
  `Span<T>` 是「指针 + 长度」的 struct，下标先钳制再 access chain。只读指针转换成的 Block 类型的成员带 `NonWritable`；
  `[NoAlias]` 的指针先存进一个带 `RestrictPointer` 的 Function 变量再读出（Vulkan 把指针的别名信息放在持有它的变量上）。
- 参数块是 push data（`PushConstant` 存储类）；参数块超过直接参数区时（§9.3）push data 里只有一个指针，参数块本身经它读取。shader record 用 `ShaderRecordBufferKHR` 存储类。
- 矩阵：语言中 R 行 C 列的矩阵是 SPIR-V 中「R 列、每列 C 个分量」的矩阵，列就是我们的行，所以按 `ColMajor` 装饰存储即为 §10 的行主序；
  乘法交换操作数（`M * v` → `OpVectorTimesMatrix(v, M)`，`A * B` → `OpMatrixTimesMatrix(B, A)`），不需要转置。
- 布局用 `Offset` / `ArrayStride` / `MatrixStride` 装饰实现 §10 的规则，只加在缓冲区和 push constant 用的显式布局类型上；它们与函数内使用的普通类型之间用
  `OpCopyLogical` 转换。往缓冲区存 struct 或数组时逐个成员存（access chain 到每个标量、向量或矩阵）：
  整体 `OpCopyLogical` 加 `OpStore` 是合法的 SPIR-V，但会让 NVIDIA 的 pipeline 编译器（驱动 610）崩溃；整体读没有问题。
- 调试信息：`NonSemantic.Shader.DebugInfo.100`（08 §7），可以关闭。
- 测试门禁：`spirv-val --target-env vulkan1.4`；参考路径（08 §4 的 HLSL+DXC 对照）要等 RHI 建好以后，在 GPU 测试里做。

### 11.4 目标抽象

后端的输出是 SPIR-V、DXIL、Metal AIR 这类高层 IR，不是机器码，所以不需要 LLVM 的指令选择、寄存器分配和调度；借用的是 LLVM 的
「目标描述 + 表驱动合法化」。增加一个后端 = 填写它的目标描述、写一个发射器，并尽量复用已有的格式写入器。

```
MIR ──► 通用优化流水线（§11.2）──► 合法化（按目标描述，LanguageIR）──► 目标发射器（各后端）──► 格式写入器 ──► 字节
```

- **目标描述 `MirTarget`**（LanguageIR）：每个后端一个，开放注册到 `MirTargetRegistry`（按名字，如 `spirv`、`dxil`、`metal`、`ps5`），
  由使用者显式调用后端模块的 `Register…Target(registry)`，不靠静态构造。描述的内容：
  - **类型能力**：支持的标量宽度、是否有整数矩阵、是否有 `double` 等。没有整数矩阵的目标（SPIR-V）由 legalize 把整数矩阵改写成行向量的数组：
    矩阵运算先拆成按行运算，再把所有持有它的类型、值和常量换成数组。两者布局相同（§10），所以缓冲区中的数据不变。
  - **阶段**：能发射哪些入口点阶段。
  - **特性**（§9.7）：这个目标认识的特性名，以及每个特性对应的目标能力（由后端自己解释）。档位只能使用目标认识的特性。
  - **操作合法性表**：对每个「MIR 操作或内建函数 × 操作数类别（bool / 整数 / 浮点 / 矩阵）」给出 `Legal`（发射器直接支持）、
    `Expand`（由通用合法化 pass 改写成更基本的 MIR）、`Custom`（发射器自己处理，如 SPIR-V 的纹理结果宽化）或 `Unsupported`（报诊断）。
    没有列出的组合默认是 `Legal`。
  - **合法化流水线**：目标在通用流水线之后追加的 pass，默认是通用合法化 pass；目标可以再加自己的 pass。
- **通用合法化 pass `legalize`**（LanguageIR）：按目标的表改写 `Expand` 的操作，对 `Unsupported` 报告错误并指出位置。已有的通用改写：
  矩阵的按分量运算和转换拆成按行运算，`Saturate` 改成 `Clamp(x, 0, 1)`，`Ldexp(x, e)` 改成 `x * Exp2(e)`，整数 `Dot` 改成乘法加归约。新的通用改写写在这里，所有目标共享。
  前端因此可以按语言语义直接生成矩阵运算，不再自己展开。
- **目标发射器**：只保留与格式无关的映射（MIR 操作 → 目标指令）和真正目标相关的处理（SPIR-V 的描述符堆访问、DXIL 的资源句柄等）。
- **格式写入器**：与 MIR 无关、只管二进制格式的部分。`SpirvModuleBuilder`（id 分配、各个段、指令编码、capability 与扩展集合）；
  DXIL 和 Metal AIR 都是 LLVM bitcode，将来共用一个 LLVM bitcode 写入器（DXIL 要求 LLVM 3.7 方言，AIR 用较新的方言），这是复用最多的地方。
- 编译一个入口点的完整过程：`前端 → MirShaderPipeline → target.AddLegalizationPasses → target.Emit`。

## 12. 编译产物与反射

每个入口点（或材质 × pass 组合）在每个档位（§9.7）上的输出包含：目标代码，或「此档位不可用」及缺少的特性；阶段与线程组大小；
光追的载荷类型、命中属性类型、是否发射光线；用到的协作矩阵组合；参数块（`[Uniform]` 参数，§9.3）的布局以及直接传递还是经指针传递；阶段输入输出；用到的 groupshared 大小；
材质 struct 的布局与参数元数据；specialization constant 的名字、id、类型和默认值；`Debug.Print` 的格式字符串表；材质的泛型值参数及其组合数；自定义 BSDF 的能力位与闭包类型 ID；依赖的源文件列表与内容哈希（结果 key 用）。
反射的格式和 Converter 接口由资产系统一侧确定（06 §3），这里只规定内容。

## 13. 待定问题

1. ~~**Shader 对象的 UUID**~~：已决定采用方案 A，见 §1.6。
2. **`double` 在 Shader 中**：主机和部分移动 GPU 对 fp64 支持很差。建议只在 Script 和公共子集的 CPU 路径中允许，Shader 中是错误。
3. ~~**08 §3 表格中的 `min16`**~~：已从 08 中删掉（§4.1）。
4. **协作向量的跨厂商支持**：SPIR-V 目前只有 NV 扩展，D3D12 侧的线性代数操作还在随 shader model 演进。语言层的 `CoopVector`/`MatrixRef` 按两边的公共部分设计（§9.9），
   哪些档位提供这个特性要等后端实现时按驱动的实际情况确定。
5. **D3D12 上的胖指针**（§4.5）：`Ptr` 在 DXIL 上降低为「原始视图索引 + 偏移」，需要 RHI 为每个缓冲区建一个覆盖整块内存的原始视图，并且单个缓冲区不能超过 4 GiB。
   等 DXIL 后端开工时确认 `RawBufferLoad` 在各厂商上的性能与 `StructuredBuffer` 相当；如果不行，退路是让编译器在指针来源可知时改用带类型的视图。
6. **参数块溢出**（§9.3）：超过 128 字节自动改为经指针传递，代价是一次额外的间接读取，而且对作者不可见。另一种做法是超过就报错、让作者自己传 `Ptr`。
   目前选自动（与 Slang 一致，作者按语义组织参数），编译器对溢出的入口点给一条 note 级诊断。
