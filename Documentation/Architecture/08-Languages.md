# 08 · Shader 语言与脚本语言

两门语言都使用**接近 C# 的语法和命名**，并允许在顶层 namespace 中直接写函数（不需要 class）。
它们由**一套公共语法加上各自的方言**组成，共享前端和公共 IR，后端各自独立。

## 1. 文件与方言

**决策**：

| 扩展名 | 方言 | 可以被谁 import |
| --- | --- | --- |
| `.mcommon` | 公共子集 | shader 和 script 都可以 |
| `.mshader` | Shader | shader |
| `.mscript` | Script | script |

`.mcommon` 用于共享数学函数、常量、结构体定义、枚举（例如 gameplay 和 shader 共用的 `DamageType`，或打包格式的布局）。
公共子集不包含 GC 引用类型、`new`、虚分派，也不包含纹理和 `half` 类型以外的 shader 专有内容。

## 2. 公共语法

- `namespace`、`using`、顶层函数、`struct`、`enum`、`interface`、`const`、属性（attribute）`[Name(args)]`。
- **单态化泛型**：`Vector<T, N>`、`T Max<T>(T a, T b) where T : IComparable<T>`；在 shader 中必须完全单态化，在 script 中也默认单态化（共享代码版本作为以后的优化）。
- **interface 加静态分派**：interface 首先是一种约束（`where T : IBsdf`），用于静态分派。
- **编译期执行**：`const` 函数和常量折叠；`static if`（编译期分支）。
- **运算符重载和扩展方法**：`public static Vector3 operator +(…)`、`public static half Luma(this half3 c)`。
- 内建向量和矩阵类型：`float2..4`、`half2..4`、`int*`、`uint*`、`bool*`、`float4x4` 等是 `Vector<T, N>` 和 `Matrix<T, R, C>` 的别名，两种写法完全等价。

## 3. 方言差异

| 特性 | Shader | Script |
| --- | --- | --- |
| `class` 与 `new`（GC 引用类型） | ✗ | ✓ |
| 虚分派（vtable） | ✗ | ✓ |
| interface 值的动态分派 | ✗（interface 只做泛型约束，MarlinLanguage §3.5） | ✓ |
| `half` / 纹理 / 采样器 / 解引用 `Ptr`/`Span` / RayQuery / wave intrinsic | ✓ | ✗ |
| 入口点属性 `[Compute(64,1,1)]`、`[Vertex]`、`[Mesh]`，`[Uniform]` 参数，`[Material]` | ✓ | ✗ |
| `[Component]` / `[System]` / `ScriptBehaviour` / 协程风格的 `async` 任务 | ✗ | ✓ |
| 递归、动态数组 `List<T>`、字符串 | ✗ | ✓ |
| `[Kernel]` 数据并行函数（§5.4） | ✗（写在 `.mcommon` 里的 kernel 可以当普通函数调用） | ✓ |
| `[ForEachEntity]` compute 入口（§5.5） | ✓ | ✗ |

## 4. 编译器结构

```
Source/Developer/
  LanguageFrontend/      Lexer → Parser(方言开关) → AST → 名字解析 → 类型检查 → 单态化 → 公共 IR (MIR)
  LanguageIR/            MIR：SSA、结构化控制流（保留 if/loop 结构，便于生成 SPIR-V/DXIL/MSL）、类型系统、通用 Pass（内联、常量折叠、DCE、SROA）
  ShaderBackendSpirv/    MIR → SPIR-V（Vulkan）
  ShaderBackendDxil/     MIR → DXIL（LLVM 3.7 bitcode 子集 + 容器 + 签名哈希）
  ShaderBackendMetal/    MIR → AIR → metallib
  ScriptBackend/         MIR → 寄存器字节码
  KernelBackend/         [Kernel] 的 MIR → 列式字节码（§5.4，推迟实现）
Platforms/PS5|Switch2/Source/Developer/ShaderBackend<P>/   MIR → 厂商中间表示
```

- **决策：所有后端都直接输出**目标格式，不经过 HLSL 中转。
  - **风险**：DXIL 要求精确匹配 DXC 产出的 LLVM 3.7 bitcode 方言以及验证器的规则；Metal AIR 的格式没有公开文档；主机的中间格式取决于 NDA 文档是否开放。
    缓解办法：每个后端都配一个「参考路径」测试，同一个 MIR 再经 HLSL 加 DXC / SPIRV-Cross 生成一份，两者在 GPU 上对比结果（11），并用官方验证器（dxil.dll、spirv-val、metal-validation）做门禁。
- 编译器以 Developer 模块的形式存在；Editor、Converter、LSP 和命令行 Program 都链接它。**Runtime 中只有字节码加载器和 VM**，Shipping Game 不带编译器。
- LSP 直接复用前端：增量解析、语义 token、补全（C++ 反射导出的 `.gen.mscript` 声明也是补全的输入）。

**实现现状**：语言规范和编译器细节见 `Documentation/Languages/MarlinLanguage.md`（§11）。已有 `LanguageFrontend`（按扩展名区分
`.mcommon` / `.mshader` / `.mscript` 三种方言）、`LanguageIR`（MIR、pass 管理、验证器、目标描述 `MirTarget`）、`ShaderBackendSpirv`，以及 `Developer/ShaderPipeline`
（全局 shader 编译进 GlobalStore）和 `Runtime/Shader`。Shader 方言按 Slang 的原生设计实现（MarlinLanguage §9 开头的取舍表）：`[Uniform]` 参数块
（超过 128 字节自动经上传内存传指针）、`Ptr` / `Span` 设备地址、带 `Access` / `ImageFormat` 类型参数的纹理、PascalCase 核心库、interface 的静态要求 / 关联类型 /
类型位置的 `this` / `extension`、自动微分（MarlinLanguage §9.12）、特性要求推导；各节注明了尚未实现的部分。`ScriptBackend` 重建中；尚未实现：`ShaderBackendDxil`、`ShaderBackendMetal`、`KernelBackend`、主机后端、LSP。

## 5. 脚本 VM

**实现现状**：`Runtime/ScriptVM` 有字节码加载、解释器、GC、任务和每个 World 一个的 `ScriptDomain`（MarlinScript §6–§9）。ECS 接入的第一版
（MarlinScript §11.2）：`[Component]` struct 注册为动态 TypeInfo 和组件（`Reflection::RegisterDynamicType`、`Components::RegisterDynamic`），
`[System]` 函数注册为动态 System 类型，由调度器通过 `Systems::RegisterFactory` 创建、和 C++ System 一起排进 DAG，脚本 System 之间串行；
现在每个实体调用一次函数（一次 System 运行共用一个 VM 入口，`ScriptBatchCall`），chunk 列的 `Span<T>`（§5.1）尚未实现。
性能基准在 `ScriptVM/Tests/ScriptVMBenchmarks.cpp`（`ScriptVM.Benchmark.*`，`--tags=Benchmark`）。字节码模块经 GlobalStore 和包交付（MarlinScript §11.1，06 §3.8）。

### 5.1 执行

**决策**：只靠高性能解释器；PC 上将来可能加 JIT，现在不考虑具体方案。

- 寄存器式字节码（类似 Lua 5.4 / Luau）：每个函数有固定大小的寄存器帧，指令定长 32 位（有需要时加扩展字），用 computed goto 分派（clang `&&label`）。
- 类型是静态的，所以不需要动态类型检查：指令按类型特化（`AddF32`、`AddI32`、`LoadField.Offset`）。
- 内联缓存只用于 interface 的动态分派。
- 向量和数学类型以值类型存放在寄存器中（128 位寄存器槽），并内建 SIMD 指令。
- ECS 交互：普通脚本 System 得到的是 chunk 列的 `Span<T>`，循环在 VM 中运行，但没有逐元素的边界外开销。
  逐实体的批量计算应该写成 `[Kernel]`（§5.4），自动并行并向量化执行；只有 kernel 也满足不了的热点才下沉到 C++ System。
- 预留 JIT 接口：字节码设计要便于将来翻译成 SSA（指令保留类型信息，基本块边界明确）。

### 5.2 内存

**决策**：以值类型为主，引用类型用增量 GC。

- `struct` 是值类型，可以作为 ECS 组件（`[Component] struct Health { public float Value; }`），布局由 VM 计算后注册为动态 TypeInfo。
- `class` 用 `new` 分配到脚本堆：精确 GC（字节码中有栈图），分代（年轻代用复制回收，老年代用增量标记清除），每帧有时间预算（例如 0.5 ms）。
- 每个 World 一个 `ScriptDomain`（独立的堆和静态变量），所以 PIE 的 World 和编辑器 World 互相隔离；Domain 销毁时整块回收。
- 引擎对象以句柄形式暴露给脚本：`EntityRef`（值类型）、`AssetRef<T>`（GC 对象持有引用计数，被回收时释放）。

### 5.3 与 C++ 的互操作

- C++ → 脚本：`MFUNCTION(ScriptCallable)` 和 `MPROPERTY(ScriptVisible)` 由 HeaderTool 生成直接调用的 thunk（不经过反射查找），同时生成 `.gen.mscript` 声明文件。
- 脚本 → C++：C++ 通过反射拿到脚本函数的 `ScriptFunction<Sig>` 句柄后调用（例如事件回调）。
- 网络：`[Server]` / `[Client]` RPC 和 `[Replicated]` 字段在脚本中同样可用（05 §7）。

### 5.4 Kernel：自动并行的数据并行函数

**决策**：用 `[Kernel]` 属性标记，**只在 CPU 上执行**：自动并行，并用**向量化解释**执行。不需要原生代码，所以 Game.exe 仍然对所有游戏一致，主机上也能用。
GPU 上的数据并行不用 kernel，而是写 `.mshader` 的 compute，再加上 ECS 绑定（§5.5）。

**为什么需要**：游戏工程不写 C++，脚本是游戏团队唯一能写逻辑的地方。大量同构实体的逐帧计算（弹幕、群体 AI、buff 计时、格子模拟、程序化散布、全场统计）
在普通解释器里会比原生慢一到两个数量级，kernel 是不 fork 引擎也能拿到接近原生吞吐的办法。
事件驱动、分支多、实体少、要碰 GC 对象的普通 gameplay 脚本不适合写成 kernel，用普通 VM 即可。

**推迟实现**：kernel 的实现成本高（副作用分析、列式后端、掩码控制流、确定性归约、标量调试路径），另外工作室也可以在 fork 的模块里写 C++ System 来处理热点。所以分阶段：

1. **第一版**：只做普通 VM，不实现 kernel。但要给它预留位置：MIR 保留副作用和均匀性分析需要的信息；字节码的函数头里留出「函数种类」字段；ECS 调度器要能接收脚本声明的读写集合。
2. **等到** 真实游戏脚本 profile 出吞吐瓶颈：先实现语言约束检查，加上「多线程 + 标量解释」（复用 VM，调试路径本来也需要它）。
3. **再** 实现列式向量化后端。

下面描述的是最终形态。

CPU 上有**两层并行**，同时生效：

1. **多线程**：数据切成批（ECS 按 chunk，数组按固定批次），每批是一个 fiber job，分布到所有 worker 核心上。
2. **SIMD**：每个 job 内部，每条列式字节码指令用 SIMD 循环一次处理整批元素。

例如 10 万个实体在 8 核上执行：约 800 个 chunk 被分给 8 个 worker，每个 chunk 的 128 个实体再由 SIMD 指令批量处理。

```csharp
// Movement.mcommon —— 写在公共子集里，shader 也能把它当普通函数调用
[Kernel, System(Phase = Physics)]
void Integrate(ref Position position, ref Velocity velocity, in Mass mass, in PhysicsSettings settings)
{
    velocity.Value += settings.Gravity * settings.FixedDelta;
    velocity.Value *= 1.0f - settings.Drag / mass.Value;
    position.Value += velocity.Value * settings.FixedDelta;
}

[Kernel] float KineticEnergy(in Velocity v, in Mass m) => 0.5f * m.Value * Dot(v.Value, v.Value);

void Report(World world)
{
    float total = world.Query<Velocity, Mass>().Reduce(KineticEnergy, ReduceOp.Sum);
    Span<float> heights = …;
    Parallel.Map(heights, (ref float h) => h = Max(h, 0.0f));   // lambda 也可以是 kernel
}
```

#### 约束（由编译器检查）

kernel 只能使用 `.mcommon` 公共子集：不能有 `class` / `new` / GC 引用、虚分派、递归、字符串、`List<T>`，也不能调用非 kernel 安全的函数。
编译器会做**副作用分析**：每个元素只能写自己的 `ref` 参数，其他的写入都必须通过下表中的并行安全类型。所以「无数据竞争」在编译期就能证明，并行化不需要用户再做任何标注。
不满足约束时，报错会指出是哪一行、因为什么不能作为 kernel。

| 参数类型 | 含义 |
| --- | --- |
| `ref T` / `in T`（T 是组件或元素类型） | 逐元素读写 / 只读 |
| 其他值参数、`in` Resource | 统一值（所有元素相同） |
| `Lookup<T>` | 只读随机访问其他实体的组件（gather） |
| `CommandBuffer.ParallelWriter` | 写其他实体、增删组件、生成或销毁实体，在同步点按确定顺序回放 |
| `Atomic<T>` / `Reducer<T, TOp>` | 跨元素的累加、计数、最值 |
| `Appender<T>` | 并行追加输出（例如收集碰撞对），每个 worker 写自己的缓冲区，再按批次顺序合并 |

#### 调用形式

| 形式 | 说明 |
| --- | --- |
| `[Kernel, System(...)]` | 从参数推导出 ECS Query 和读写集合，加入调度 DAG（05 §3），按 chunk 切成 job |
| `query.ForEach(kernel, uniforms…)` | 在普通 System 或脚本代码里对 Query 调用 |
| `Parallel.Map(spans…, kernel)` | 对任意 `Span<T>` / 缓冲区逐元素执行，可以同时传入多个等长的 span |
| `Reduce` / `Scan` / `Compact` | 归约（`ReduceOp.Sum/Min/Max/…`，或者自定义满足结合律的运算）、前缀和、按条件压缩 |

- **自动并行**：ECS 按 chunk 切分，数组按固定批次（例如 4096 个元素）切分，交给 fiber job 执行。元素少时（按「元素数 × 估算指令数」的代价模型）直接内联执行，不派发 job。
- **确定性**：分批方式只取决于数据大小，与线程数无关；归约在批内和批间都按固定的树形顺序合并，所以浮点结果在不同机器、不同核数上都完全一致（网络预测和回放需要这一点）。

#### CPU：向量化解释

- `KernelBackend` 把 kernel 编译成**列式字节码**：每个寄存器是一列（一批元素的 SoA 数组，存放在 worker 的 scratch arena 中），**每条指令处理一整批**（一个 chunk，或者最多 256 个元素），由 C++ 里用 clang 向量扩展写的 SIMD 循环实现。
  解释分派的开销被均摊到上百个元素上。
- **均匀性分析**：编译器能证明在所有元素上都相同的值（统一参数、常量及它们的运算），就保留成标量寄存器，只算一次。
- 控制流用**掩码**处理（和 GPU 的 SIMT 一样）：`if` 两边都执行，结果用掩码选择；循环一直执行到掩码全空为止。全部为真或全部为假的批次直接跳过另一边。
- `Lookup<T>` 编译成 gather 指令；`ref` 参数直接读写 chunk 中的列，不需要拷贝。
- 目标：算术密集的 kernel 性能达到原生 C++ 的 2–5 倍以内（**目标，不是承诺**，需要基准测试验证）。PC 将来如果加 JIT，列式字节码也是最容易翻译成原生代码的部分。

#### 调试

Debug 模式下，kernel 可以切换到**标量单步**：同一份 MIR 按逐元素顺序解释，这样 DAP 就能在 kernel 里下断点、看某个实体的值（可以加条件，例如 `entity == 1234`）。
正常运行时走列式路径。两条路径的结果在测试里互相对比。

### 5.5 GPU 上的数据并行：compute shader + ECS 绑定

**决策**：没有 GPU kernel。GPU 上的数据并行一律写 `.mshader` 的 compute shader（语法同样是 C#）。
引擎只补上一件 kernel 原本能提供的便利：**把 ECS 组件列自动绑定到 compute shader 上，并自动派发**。

适用的场景：数据在 GPU 上、结果也在 GPU 上被消费的**视觉模拟**，比如粒子和 VFX、植被与角色的交互、布料和流体之类的视觉效果、地形形变、GPU-driven 的大规模人群动画、生成渲染实例。
gameplay 逻辑不放到 GPU 上：回读至少晚一帧，GPU 浮点在不同厂商之间也不确定，会和网络同步冲突。

```csharp
// Grass.mshader
[Compute(64, 1, 1), ForEachEntity(Phase = PostPhysics)]
void BendGrass(ref GrassBlade blade, in WorldTransform transform, in GrassSettings settings)
{
    blade.Bend = WindBend(transform.Position, settings.Time);   // WindBend 定义在 .mcommon 中，脚本也能调用
}
```

- `[ForEachEntity]` 入口的参数规则和 kernel 一样：`ref` 写、`in` 读、其他参数是统一值。组件必须标了 `[GpuMirrored]`，GPU 副本由 RenderScene 维护（Extract 阶段上传有改动的列）。
- 引擎从参数推导出 Query：对每个匹配的 archetype，把各列在 GPU Scene 中的 `Span` 作为入口点的 `[Uniform]` 参数传入（MarlinLanguage §9.3），生成一个 RenderGraph compute pass，按 `Phase` 排序。shader 作者不需要写任何绑定代码。
- 只被 GPU 写的组件（例如 `GrassBlade.Bend`），默认只存在于 GPU 上，CPU 侧看不到。CPU 要读的话，显式使用 `GpuReadback<T>`（异步，至少晚一帧）。
- 用 `.mcommon` 的纯函数在 CPU 和 GPU 之间共享计算逻辑（例如上面的 `WindBend`）：CPU 侧由脚本或 kernel 调用，GPU 侧由 compute 调用。
- 归约、前缀和、压缩等通用并行原语，由引擎在 `Language/Shaders/` 中以 `.mshader` 库的形式提供（wave intrinsic 加多 pass 实现）。

## 6. 脚本热重载

**实现现状**：只有冷重载：模块经 `Asset.Changed` 重载后，每个 World 换一个新 domain，脚本状态从头开始（MarlinScript §12）。下面的热重载尚未实现。

- 文件保存 → 增量编译受影响的模块 → 新字节码通过 Remote 推送到所有运行中的进程。
- 只改了函数体：在下一个安全点（帧边界）替换函数表，正在运行的栈帧执行完旧版本。
- 改了类型布局：通过反射对比新旧 TypeInfo，按字段名迁移实例（包括 ECS 列中的脚本组件）；无法迁移的字段使用默认值，并输出警告。

## 7. 调试

- DAP 服务器（`MarlinDebugAdapter`）通过 Remote 层连接 VM：断点（字节码 PC ↔ 源码行表）、单步、调用栈、变量（借助 TypeInfo 显示）、条件断点、热重载后断点重新映射。
- Shader 调试：编译时保留调试信息（SPIR-V NonSemantic.Shader.DebugInfo / DXIL PDB），方便 RenderDoc、PIX 和 Razor 使用。

**实现现状**：`ShaderBackendSpirv` 输出 `NonSemantic.Shader.DebugInfo.100`，可以关闭（MarlinLanguage.md §11.3）；DAP 服务器（`MarlinDebugAdapter`）尚未实现。
