# Marlin 语言规范（Script 方言与 VM）

本文细化 [08 · Shader 语言与脚本语言](../Architecture/08-Languages.md) §3、§5、§6，是 `LanguageFrontend` 的 Script 部分、`LanguageIR`
的脚本操作、`ScriptBackend`（MIR → 字节码）和 `ScriptVM`（加载器、解释器、GC、`ScriptDomain`）的实现依据。公共子集的语法和语义见
[MarlinLanguage](MarlinLanguage.md)，本文只写 Script 方言在它之上增加或改变的部分；两边冲突时以本文为准（只对 `.mscript`）。

约定同 MarlinLanguage：「错误」指编译期诊断；「故障」（fault）指运行时结束一次 VM 入口的错误（§8）。

> **恢复说明**：rev 99 的原文在 10/28 的数据丢失中没有留下。本文按幸存的代码（前端、ScriptBackend、ScriptVM 的大纲、导出符号、诊断文本、
> 测试与脚本库源码）和会话记录重写，章节号与代码注释里引用的 `MarlinScript §x.y` 保持一致。没有直接证据、按架构文档和常规做法补写的段落
> 用「**恢复补写**」标出，供审阅（lang-files-D7）。

## 1. 源文件、包与库

### 1.1 文件与方言

- `.mscript` 是 Script 方言，UTF-8，规则同 MarlinLanguage §1.1。Script 编译看到 `.mscript` 和所有 `.mcommon` 文件（§1.3）。
- 公共子集的全部内容在 Script 中可用。下面这些只存在于 Script（在 `.mcommon` / `.mshader` 中使用时报 `` `{0}` exists only in scripts `` 一类的错误）：
  `class`、委托与事件、lambda、可空类型 `T?`、运行时长度的数组 `T[]`、`is` / `as` / `typeof`、`??` / `?.`、`switch` 表达式与 `case` 模式、
  rune 字面量 `'a'`、`async` / `await`、`yield`（§4.4）。
- Shader 专有的内容（纹理、缓冲区、wave、`half` 以外的 shader 内建类型、入口点属性）在 Script 中不可见：它们声明在
  `Language/Shaders/Core/Marlin.mshader`，不在 Script 的源集里。

### 1.2 包与模块

- **包** = 一个挂载根：`Engine`、`Project`。可见性规则同 MarlinLanguage §1.4（`internal` = 同一个包）。
- 一个包的全部 `.mscript` 编译成**一个脚本模块**（MIR 模块 → 字节码模块），模块名就是包名（`MirModule::Name`）。
  `LanguageCompilation::CheckScriptPackage(package)` 只做检查，`CompileScriptPackage(package, module)` 检查并降低到 MIR；
  包名不存在时报 `the source set has no package`。Shader 编译不能编译脚本包（`script packages are compiled by a Script compilation`）。
- 模块之间按名字链接（§6.2）：引用别的包里的函数和类型时，模块记录一个导入项，加载时在已加载的模块里找同名导出。

### 1.3 源集、目录约定、核心库与库

- 每个挂载根的语言代码在 `Language/` 下按方言分目录（MarlinLanguage §1.3）：`Language/Common/`（`.mcommon`）、`Language/Shaders/`、
  `Language/Scripts/`（`.mscript`）。文件放错目录是错误（`` `{0}` files go in Language/{1}/ of their mount root ``）。
- Script 编译的源集 = 所有挂载根的 `Language/Scripts/` 加上 `Language/Common/`。所以 `.mcommon` 中的数学、struct、enum 两边共享，
  `Marlin` 命名空间由三个核心库文件合并而成。
- 引擎的 `Language/Scripts/` 按下面的约定组织：

  ```
  <EngineRoot>/Language/Scripts/
    Core/                Marlin            核心库：Marlin.mscript（object、string、StringBuilder、数组成员、Task、Frame、委托、
                                           比较与哈希）、Random.mscript、Span.mscript。始终可见，不需要 using
    Library/<Area>/      Marlin.<Area>     普通库：Collections（List、Dictionary、HashSet、Queue、Stack、排序）、Tasks（Wait）……
                                           需要 `using Marlin.Collections;`
    Tests/               Marlin.Tests      脚本测试（§15）
  ```

- **核心库与库的分工**：只有 `/Engine/Language/Scripts/Core/` 中的文件可以声明 `[Native]` 函数和 `[Builtin]` 类型（`[Intrinsic]`、
  `[SystemValue]`、`[CompileTime]`、`[Native]` 只允许出现在核心库中）。所以需要 native 的东西（文本、`Random` 的熵、`WeakRef`、`Debug`）
  放在核心库，其余尽量用脚本本身写在 `Library/`。`Library/` 的命名空间不能和其中的类型同名（`Marlin.Random` 命名空间里的 `Random`
  类型会和自己冲突），这也是 `Random` 放在核心库的原因之一。
- 编译需要核心库：缺少 `/Engine/Language/Common/Core/Marlin.mcommon` 或 `/Engine/Language/Scripts/Core/Marlin.mscript` 时编译直接报错
  （`the compiler needs /Engine/Language/{0}/Core/`）。

## 2. 词法与语法

### 2.1 新增的关键字和语法

- 上下文关键字：`class`、`delegate`、`virtual`、`override`、`abstract`、`sealed`、`protected`、`event`、`async`、`await`、`yield`、`var`、
  `base`、`init`、`checked`。`object` 和 `string` 是核心库中的类名，不是关键字。
- 类成员：字段、方法、构造函数（`: base(…)` / `: this(…)`）、属性（`{ get; set; }`、`{ get; init; }`、`{ get; protected set; }`、表达式体）、
  索引器 `this[int index]`、事件 `event Action<float> Changed;`、运算符重载。脚本没有只写属性（`scripts have no write-only properties`）。
- 语句：公共子集的全部语句，加上 `foreach` 遍历任意可枚举类型（§3.9）、`yield return` / `yield break`、`await`（表达式）。
- 没有异常：没有 `try` / `catch` / `throw`。运行时错误是故障（§8），由 `Panic` 和 `Assert` 主动触发。

### 2.2 字面量

- 字符串字面量 `"…"`、逐字字符串 `@"…"`，类型是 `string`（§10.1，UTF-8，不可变）。
- rune 字面量 `'a'`、`'é'`，类型是 `Rune`（一个 Unicode 标量值）。
- 插值字符串 `$"…{expr}…{expr:format}…"`：编译成 `StringBuilder` 的 `Append` 调用序列（§10.2）；`{{` / `}}` 是花括号本身。
  格式说明只对数字有效（`a format applies to numbers`），格式见 §10.2。

### 2.3 运算符

- 空值运算：`a?.b`、`a ?? b`、`a ??= b`、`x!`（断言非空，§3.6）。`??` 的左边必须可能为空。
- 类型运算：`x is T`、`x is T t`、`x is { … }` 模式（§3.9）、`x as T`（失败得到 `null`）、`(T)x`（失败是 `InvalidCast` 故障）、`typeof(T)`（§3.10）。
- `checked(expr)`：整数运算溢出时产生 `Overflow` 故障；默认不检查（按位回绕）。
- 字符串 `+` 调用 `string.Concat`；非字符串操作数先调用 `ToString`。委托的 `+` / `-` 合并和移除调用列表（§3.7）。

### 2.4 对象与集合初始化器

- 对象初始化器 `new T(args) { Field = value, Property = value }`：对 class 和 struct 都可用；`init` 属性只能在这里或构造函数中赋值。
- 集合初始化器 `new List<int> { 1, 2, 3 }`：对每个元素调用一次新集合的 `Add(element)`，元素类型从最后一个 `Add` 的参数推导。
- 数组创建：`new T[length]`、`new T[] { a, b }`。省略 `new` 的 `T[] x = { … }` 不支持（`write it: new T[] { … }`）。

## 3. 类型与声明

### 3.1 值类型与引用类型

| 类别 | 例子 | 存放 |
| --- | --- | --- |
| 值类型 | 标量、向量、矩阵、`enum`、`struct`、`Rune`、`Span<T>`、`int?`（可空值类型） | 内联在变量、字段、数组元素中 |
| 引用类型 | `class`（包括 `string`、`StringBuilder`、`Task`）、委托、数组 `T[]`、interface 类型的值 | 脚本堆上的对象（§9），变量里存引用 |

- 公共子集里的定长数组 `T[N]` 仍是值类型；`T[]` 是运行时长度的 GC 数组。
- 值类型在需要 `object` 或 interface 的地方自动**装箱**（§3.8）。

### 3.2 class 与 object

- `class` 是引用类型，用 `new` 分配，单继承（`a class has one base class, written first`），可以实现任意多个 interface。
  所有 class 都直接或间接派生自 `object`（核心库中的 `[Builtin("Object")] class object`）。只有 class 能派生自 class。
- `virtual` / `override` / `abstract` / `sealed` 的规则同 C#：`override` 必须有可覆盖的同名同签名成员（`` `{0}` overrides nothing ``），
  `sealed` 的类不能被继承、`sealed` 的成员不能再被覆盖；有 `abstract` 成员的类必须标 `abstract`，派生类必须实现所有抽象成员。
- `object` 的虚成员：`Equals(object?)`（默认比较引用）、`GetHashCode()`（默认按对象身份）、`ToString()`（默认是类型名），以及非虚的
  `GetType()`（§3.10）。`string` 按内容覆盖 `Equals` 和 `GetHashCode`。
- `base.Member` 调用基类的实现，只能在 class 的实例成员中使用。`protected` 成员对派生类可见。

### 3.3 构造函数、字段与属性

- 没有声明构造函数的 class 由编译器生成一个无参构造函数；派生类的构造函数先调用基类构造函数（默认调用无参的那个，基类没有时报错）。
- 字段初始值在构造函数体之前按声明顺序执行。
- **非空引用字段必须初始化**：每个非可空引用类型的字段要么有初始值，要么在每个构造函数中赋值，否则报错
  （`assign it in every constructor, give it an initial value, or make it nullable`）。
- 自动属性（`{ get; set; }`）由编译器生成一个隐藏字段和访问器（§5.4）；`init` 访问器只能在构造期间赋值。
- `static` 字段属于模块，模块加载时按声明顺序运行初始值（§6.3）。

### 3.4 interface、虚分派与泛型

- 在 Script 中 interface 既是泛型约束（静态分派），也是普通的类型：可以作为变量、参数、数组元素的类型，调用通过接口表在运行时分派
  （VM 用内联缓存加速，§14.2）。struct 赋给 interface 类型时装箱。
- 泛型在 Script 中同样**单态化**：每组类型实参生成一份代码。所以虚方法和 interface 方法不能有自己的泛型参数
  （`a virtual method cannot have generic parameters`：虚表的一个槽放不下所有实例）。
- 约束：公共子集的约束，加上 `where T : class`（引用类型）。约束必须是 interface 或 class。
- 泛型代码里对 `T` 调用的成员（`Equals`、`GetHashCode`、`CompareTo` …）在实例化时按实际类型解析：class 用虚成员，struct 用自己声明的成员，
  标量用核心库的扩展方法（§10.1）。实例化时找不到成员是错误，报在使用泛型的地方。

### 3.5 数组与 Span

- `T[]` 是 GC 对象：长度在创建时确定，越界访问是 `IndexOutOfRange` 故障。所有数组都有核心库 `Array<T>` 的成员：`Length`、
  `CopyTo(sourceIndex, destination, destinationIndex, count)`（允许重叠，如同经过临时缓冲区）、`Clear(start, count)`。
- `Span<T>` 是**普通 struct**，保存数组本身、起始偏移和长度，不保存指向数组内部的指针。所以它可以存放在任何地方（字段、数组、闭包），
  GC 移动数组时也一直有效，VM 不需要处理内部指针。写入直接写到数组里。`ReadOnlySpan<T>` 不能写。
- 语言里没有用户定义的转换，所以 span 用 `array.AsSpan()`、`array.AsSpan(start, length)`、`span.Slice(…)`、`span.AsReadOnly()` 得到。
  越界的范围是 `Panic` 故障。成员见 `Language/Scripts/Core/Span.mscript`：`Length`、`IsEmpty`、索引器、`Slice`、`CopyTo`、`Fill`、
  `Clear`、`ToArray`、不分配内存的 `foreach`。
- ECS chunk 列上的 span 见 §11.2（待定）。

### 3.6 可空性

- 引用类型默认**不可为空**：`Node node = null;` 是错误（`` `null` is not a `{0}` ``，提示 `declare the type `{0}?` to allow null`）。
  `T?` 才能保存 `null`。
- **流分析**：对 `T?` 的值，在证明非空之前不能访问成员（`` `{0}` may be null here ``）。能证明非空的写法：`if (x != null)`、`x is T t`、
  `x?.Member`、`x ?? fallback`，或用 `x!` 断言（为空时是 `NullReference` 故障）。对已知非空的值写 `?.` 或 `??` 会报诊断（`` `{0}` is never null ``）。
  进入循环时，循环中被赋值的变量的证明作废。
- 值类型的 `T?` 是可空值类型：`HasValue`、`Value`、`GetValueOrDefault()`，可以和 `null` 比较、参与 `??`。
- `default` 对引用类型是 `null`，所以不能赋给非可空引用。`default!` 给出非可空引用类型的零值，用于确实需要一个占位值的地方
  （例如 `TryGetValue` 失败时的 `out` 参数）；读取这个零值的成员是 `NullReference` 故障。泛型 `T` 的零值写 `default!`。

### 3.7 委托、事件、lambda 与闭包

- `delegate` 声明委托类型；核心库提供 `Action`、`Action<T1..T4>`、`Func<TResult>`、`Func<T1..T4, TResult>`。委托是引用类型，
  保存一个调用列表；`+` / `-` 合并和移除。签名（参数、参数模式和返回类型）必须完全一致。
- 方法组可以转换成委托（`Curve curve = Ease;`、`health.Changed += bar.Set;`）。
- `event` 字段只能在声明它的类之外 `+=` / `-=`，在类内部才能调用。调用一个没有处理函数的事件什么也不做；所以事件的委托必须返回 `void`。
- lambda：`x => expr`、`(int a, int b) => a + b`、块体、`async` lambda。lambda 只能转换成委托类型；参数类型从目标委托推导，个数必须一致。
- **闭包**：lambda 捕获的局部变量按引用共享；**每次循环迭代**有自己的一份循环体内的变量（`for` 循环体中声明的变量，见 rev 119 的修复）。
  捕获规则：不能捕获 `ref` / `out` 参数；struct 方法中的 lambda 不能使用 `this`；`static` lambda 不能捕获任何东西，也不能用 `this`；
  在赋值之前捕获的变量是错误。捕获的变量提升到闭包环境对象中（§5.5、§9.10），环境按作用域嵌套，指向外层环境。

### 3.8 装箱与类型转换

- 值类型转换成 `object` 或它实现的 interface 时装箱：分配一个 box 对象保存值的副本。`(int)boxed` 拆箱，类型不符是 `InvalidCast` 故障。
- 装箱的值按值比较和哈希（`object boxed = 41; boxed.Equals(41)` 为真）。
- 引用转换：派生类到基类、class 到它实现的 interface 是隐式的；反方向用 `(T)x`（故障）或 `as`（得到 `null`）。

### 3.9 模式、switch 与 foreach

- 模式：类型模式 `T t`、常量模式（含 enum 成员）、`null` 和弃元 `_`，用在 `is`、`switch` 语句的 `case`（可带 `when`）和 `switch` 表达式中。
  `switch` 表达式必须穷尽，否则报错并提示补上缺少的情况或 `_` 分支。**恢复补写**：属性模式、关系模式是否已实现以 `BinderPatterns.cpp` 为准。
- `foreach` 可以遍历：数组、`Range`、有 `GetEnumerator()` 方法的类型（它返回的枚举器要有 `bool MoveNext()` 和 `Current`），以及
  `IEnumerable<T>` / `IEnumerator<T>`（通过 interface 分派）。有自己的 `GetEnumerator()` 的类型直接调用它，不分配、不装箱
  ——库里的集合都返回 struct 枚举器。

### 3.10 类型信息

- `typeof(T)` 和 `x.GetType()` 得到 `TypeInfo`：引擎反射对类型的描述（04），脚本能读 `Name` 和 `IsAssignableFrom(other)`。
  脚本 struct 和 class 在模块加载时注册为动态 TypeInfo（08 §5.2）。

## 4. 执行模型

### 4.1 帧与阶段

- 脚本代码在 World 的线程上运行。每个 World 有一个 `ScriptDomain`（§9.5），帧边界时推进 domain 的时钟、恢复等待中的任务。
- 时间是 **World 时间**：随 World 暂停和缩放。`Frame.Delay(seconds)` 按它计时。
- **恢复补写**：普通脚本函数由 System、组件回调和 C++ 调用（§7.2）进入 VM；一次调用到返回或挂起为止是一次 **VM 入口**，故障只结束当前入口（§8）。

### 4.2 任务（async / await）

- `async` 函数返回 `Task` 或 `Task<T>`（`an async function returns Task or Task<T>`）。调用它就启动一个**任务**：任务在自己的 VM 栈上运行，
  运行到第一个未完成的 `await` 时挂起，调用方拿到 `Task` 继续执行。`Task<T> : Task`，所以 `Task<int>` 可以传给需要 `Task` 的参数。
- `await task`：任务已完成时直接得到结果；否则挂起当前任务，等它完成后在**帧边界**恢复。只有 `Task` 能被 await，`await` 只能出现在
  `async` 函数中（`mark the function or lambda async and make it return Task`）。被 await 的任务故障或取消时，故障传给等待者。
- 帧上的等待点（核心库 `Frame`）：`Frame.Next()`（下一帧 Gameplay 阶段开始时）、`Frame.NextFixed()`（下一个固定步长的同一位置）、
  `Frame.Delay(seconds)`、`Frame.Until(condition)`（第一个满足条件的帧边界）。
- `Task` 的成员：`IsCompleted`、`IsCanceled`、`IsFaulted`、`Cancel()`、`Task.WhenAll(tasks)`、`Task.WhenAny(tasks)`、`Task<T>.Result`
  （任务未完成时读取是故障：`the task has not completed`）。
- **所有权**：`Task.Run(function, owner)` 启动一个属于 `owner`（实体或 behaviour）的任务，owner 销毁时任务被取消（`CancelTasksOwnedBy`）。
- 取消：取消的任务在下一个挂起点结束，不再恢复；等待它的任务也被取消。没有人等待的任务故障时写日志（`A task faulted with no one awaiting it`）。
- 库 `Marlin.Tasks.Wait`（`Library/Tasks/Wait.mscript`）提供 `Frames(count)`、`Until(condition, timeout)`、`For(task, timeout)`、`Every(interval, step)`。

### 4.3 迭代器与 VM 栈

- 含 `yield` 的函数是**迭代器**，返回 `IEnumerable<T>` 或 `IEnumerator<T>`；用 `yield break` 结束，不能用 `return` 返回值。
  `yield` 只能出现在迭代器中；同一个函数里 `yield` 和 `await` 不能混用（一个函数不能既是 `async` 又是迭代器）。
- 迭代器和任务都是**协程**：各自有一个 VM 栈（domain 的主栈之外，按段分配，`StackSegmentBytes`，上限 `MaxStackBytes`，超出是
  `StackOverflow` 故障）。迭代器的 `MoveNext` 在调用方的入口里恢复它的栈运行到下一个 `yield`；`Current` 在 `MoveNext` 之前或结束之后读取是故障。

### 4.4 方言边界

- class、委托、运行时长度的数组和可空引用是**脚本类型**，只能出现在 `.mscript` 中（`classes, delegates, arrays of run-time length and nullable
  references are script types`）。`.mcommon` 中的代码也会被 shader 编译，所以不能使用它们；kernel（§13 待定）同样只用公共子集。

## 5. 降低到 MIR

MIR 是 Shader 和 Script 共用的（MarlinLanguage §11.2）；这里规定 Script 专有的部分。

### 5.1 脚本类型与操作

- MIR 类型增加 `Reference`（指向脚本堆对象的引用，带地址空间 `Managed` / `Static`）。每个模块有一张**脚本类型表**（`MirScriptType`：
  `Class`、`Interface`、`Delegate`、`Array`、`String`、`Value`），脚本指令按下标引用其中的类型。
- 脚本操作（MirOp）：`New`、`NewArray`、`FieldAddress`、`ElementAddress`、`ObjectLength`、`Box`、`Unbox`、`IsInstance`、`Cast`（可选「失败得 null」）、
  `TypeOf`、`NullCheck`、`CallVirtual`、`CallInterface`、`CallDelegate`、`MakeDelegate`、`CombineDelegates`、`Await`、`Yield`。字节码层面的指令见 §14.2。

### 5.2 类布局

- class 的对象 = 对象头（§9.1）+ 基类字段 + 本类字段；虚表按基类槽在前的顺序排列，每个 interface 有一张接口方法表（`ScriptInterfaceTable`）。

### 5.3 函数种类

- `MirFunctionKind`：`Normal`、`Async`、`Iterator`、`Kernel`（预留，08 §5.4）。字节码的函数头保留同样的字段（`ScriptFunctionKind`）。
  `async` 函数的 `Return` 给出任务的结果类型（`ResultType`）；迭代器的 `yield return Value;` 降低成 `Yield`，`yield break` 降低成不带值的 `Yield`。
- `[Native("Name")]` 函数没有函数体，降低成一个带 native 名字的导入函数（§7.1）。

### 5.4 属性与自动属性

- 属性降低成 get / set（或 init）访问器函数；自动属性的访问器读写编译器生成的字段。读写属性的表达式（含复合赋值 `+=`、`++`）
  按「读访问器 → 运算 → 写访问器」降低，`ref` 实参的属性通过临时变量回写。

### 5.5 闭包环境

- 有被捕获变量的作用域在进入时创建一个**环境对象**（§9.10），被捕获的变量是它的字段；lambda 降低成以环境为接收者的函数加 `MakeDelegate`。
  循环体的环境每次迭代重新创建，所以每次迭代捕获的是各自的变量。

## 6. 字节码与模块

`ScriptVM` 模块包含字节码格式和模块加载、解释器、GC 和 `ScriptDomain`。`ScriptBackend`（Developer）把 MIR 编译成字节码模块。

### 6.1 ScriptVM 与模块映像

- 进程里只有一个 `ScriptVM`，由 `ScriptVMSubsystem`（EngineSubsystem）持有：native 注册表（§7.1）和所有 World 的 domain 共享的模块映像
  （只读的字节码和元数据）。每个 World 的 `ScriptWorldSubsystem` 持有该 World 的 domain（§9.5）。
- 字节码模块的段：`Strings`、`Shapes`（值的布局：标量、向量、矩阵、数组、struct、引用、指针、函数）、`Types`（脚本类型表）、`Functions`、
  `Statics`、`Imports`、`Exports`、`Debug`（源文件、行表）。模块头有格式版本，与 VM 不一致时拒绝加载
  （`format version {0}, but this VM reads {1}`）。加载时校验每条指令和每个表引用（越界、跳出函数、解码失败都拒绝加载）。

### 6.2 链接

- domain 里每个类型名只有一个类型对象，所有引用它的模块共享（`ScriptDomain::FindType`）。
- 导入项（`ScriptImportKind`：`Function`、`Native`、`Type`）在加载时按名字解析：函数在已加载模块的导出中找，签名必须与导出一致；
  native 在注册表中找（`no native function is registered as "{1}"`）；类型在已加载模块声明的类型中找。找不到就拒绝加载这个模块。

### 6.3 静态初始化

- 模块加载完成后，按声明顺序运行它的静态字段初始值函数（`MirModule` 里的初始化函数列表）。初始值故障时模块加载失败
  （`a static initializer of {0} faulted`）。

## 7. 与 C++ 互操作

### 7.1 native 函数

- 核心库用 `[Native("Type.Member")]` 声明没有函数体的函数；VM 启动时 C++ 把实现注册到 `ScriptNativeRegistry`（`RegisterScriptCoreNatives`、
  `ScriptInterpreter::RegisterNatives`），加载时按名字链接。名字是契约：改名必须两边同时改。
- native 看到的调用（`ScriptCallContext`）：参数寄存器、结果寄存器（与第一个参数重叠），以及 `Fault(kind, message)`（§8）。参数和返回值由
  `ScriptMarshal` 在脚本值与 C++ 类型（`int32`、`float32`、`StringView`、`ScriptArray *`、`ScriptObject *` …）之间转换。
- **恢复补写**：native 不能重入 VM，也不能挂起；需要等待的功能（`Frame.*`、`Task.When*`）由 VM 自己实现（§9.7）。

### 7.2 C++ 调用脚本

- `ScriptDomain::FindFunction(name)` 得到 `ScriptFunction`，带类型的参数调用：`ScriptResult<T> result = function(args…)`。
  结果要么是返回值，要么是故障信息（§8）。脚本函数挂起时（async）返回 `Task`。
- C++ 持有脚本对象用**句柄**（`CreateHandle` / `DestroyHandle`）：句柄是 GC 根，对象被移动后句柄跟着更新。

### 7.3 生成的声明

- **恢复补写**：`MFUNCTION(ScriptCallable)` / `MPROPERTY(ScriptVisible)` 由 HeaderTool 生成直接调用的 thunk 和 `.gen.mscript` 声明（08 §5.3），
  声明中的函数以 `[Native]` 的形式出现在生成文件里，由引擎模块注册。实现随 ECS 绑定一起做（§16 步骤 7）。

## 8. 故障

- 故障结束当前 VM 入口，带种类、消息和脚本调用栈（函数、文件、行、列，最内层在前）：`ScriptFaultInfo`。种类（`ScriptFault`）：

  | 种类 | 原因 |
  | --- | --- |
  | `NullReference` | 访问空引用的成员、`x!` 为空 |
  | `IndexOutOfRange` | 数组、字符串越界 |
  | `InvalidCast` | `(T)x` 或拆箱的类型不符 |
  | `DivideByZero` | 整数除以零 |
  | `Overflow` | `checked` 中的整数溢出、转换的值超出目标类型 |
  | `StackOverflow` | 协程栈超过 `MaxStackBytes` |
  | `Timeout` | 一次 VM 入口超过看门狗时间（`WatchdogSeconds`） |
  | `AssertFailed` | `Debug.Assert`、测试的 `Assert` 失败（§15） |
  | `Panic` | 核心库或用户代码调用 `Panic(message)` |
  | `Native` | native 函数报告的其他错误 |

- 故障发生时 VM 释放这次入口的栈；对任务而言，任务进入 `Faulted` 状态，等待者收到同一个故障（§4.2）。没有恢复机制：脚本不能捕获故障。
- 故障通过日志（`LogScript`）报告；编辑器中跳到源码位置。

### 8.1 VM 入口与结果

- `ScriptInterpreter::Invoke` 把参数写进被调函数的参数寄存器并运行；结果状态（`ScriptRunStatus`）：`Returned`、`Suspended`（挂起的协程）、
  `Faulted`、`Canceled`。成功时结果从结果寄存器读出。任务的状态（`ScriptTaskStatus`）：`Pending`、`Completed`、`Faulted`、`Canceled`。
- `ScriptDomain::TryInvoke` 是不构造 `ScriptResult` 的 `Invoke`（故障在 `GetLastFault`）；`ScriptFunction<Sig>` 用它，参数和结果少于 8 / 4 个槽时
  放在 C++ 栈上，不分配。`ScriptBatchCall` 在一次入口里反复调用同一个函数（脚本 System 每个实体一次，§11.2），协程栈只准备一次；
  每次调用对看门狗和故障仍算一次入口。
- 看门狗的时钟在入口第一次补充预算（1024 次回边或调用检查）时才开始读，所以很短的入口不读时钟；超时的判定因此最多晚 1024 次检查。

## 9. 运行时

### 9.1 对象头

- 每个堆对象以对象头开始：类型指针、GC 位（年龄、已转发标记等）和哈希种子。数组和字符串在头后存长度，数据从类型给出的 `DataOffset` 开始。

### 9.2 堆、GC 与弱引用

- 每个 domain 一个堆：一段预留的地址空间（`HeapReserveBytes`），包含年轻代（`NurseryBytes`）、两个 survivor 区（`SurvivorBytes`）和老年代。
  **精确、分代、会移动对象**：年轻代用复制回收，存活够久的对象晋升到老年代；老年代用增量标记清除（阶段 `Idle`、`Marking`、`Sweeping`）。
  堆耗尽是故障（`the script heap is exhausted`）。
- 根：VM 栈（按安全点的栈图精确扫描，§9.3）、静态字段、句柄（§7.2）、等待中的任务。
- **`WeakRef<T>`**（`where T : class`）：不让目标保持存活的引用。目标只被弱引用引用时，下一次能回收它的收集把它释放，`Target` 变成 `null`。
  VM 一侧：弱引用对象创建时登记一次（`RegisterWeakRef`），收集器不追踪它的目标；年轻代收集更新被移动的目标，目标死亡时清空。
  `SetTarget` 只改目标，不重新登记，所以是 O(1)。

### 9.3 栈图与安全点

- ScriptBackend 为每个函数记录安全点（调用、分配、`await`、`yield`、循环回边）和每个安全点上存放引用的寄存器（`ScriptSafepoint`、`ScriptSlot`）。
  GC 只在安全点发生，所以寄存器里的引用都是已知的，移动对象后可以更新。

### 9.4 GC 预算

- 每帧在帧边界给收集器一个时间预算（`GcBudgetSeconds`，默认 0.5 ms 量级，08 §5.2），增量推进老年代的标记和清除；分配压力大时也会提前请求收集
  （`RequestCollection`）。domain 销毁时整块释放。

### 9.5 World 中的 ScriptDomain

- `ScriptWorldSubsystem`（WorldSubsystem）在 World 启动时创建 domain 并加载 VM 的模块；每个帧边界推进 domain 的时钟（World 时间）、恢复等待中的任务、
  给收集器预算。所以 PIE 的 World 和编辑器 World 互相隔离（08 §5.2）。

### 9.6 ScriptDomain

- `ScriptDomain` 是一个 World 的全部脚本状态：堆、类型、加载的模块、静态变量、解释器和任务调度器。主要接口：`Load`、`FindFunction`、`FindType`、
  `Invoke`、`BeginFrame`、`FixedTick`、`AdvanceTime`、`RunReady`、`CancelTasksOwnedBy`、`GetTaskStatus`、`Collect`、`CreateHandle` / `DestroyHandle`。
  选项 `ScriptDomainOptions`：堆的各区大小、栈段大小与上限、每帧 GC 预算、看门狗时间。

### 9.7 VM 实现的函数

- 有些核心库函数由 VM 自己实现，因为它们要操作调度器或协程：`Frame.*`、`Task.*`、迭代器的 `MoveNext` / `Current` / `GetEnumerator`
  （`Iterator.*`）。它们和 native 一样按名字链接，但能挂起当前协程。

### 9.8 字符串、数组与内建对象

- `string` 是不可变的 UTF-8 字节数组对象；`Length` 和所有偏移按字节计。`Substring` 切开一个 UTF-8 字符时是故障（`the substring splits a UTF-8 character`）。
- VM 认识的内建对象（`ScriptBuiltin`）：`Object`、`String`、`Array`、`Delegate`、`Box`、`StringBuilder`、`TypeInfo`、`Task`、`Iterator`、`WeakRef`；
  它们的 VM 状态放在对象字段之后（例如任务对象的状态，前几个成员是引用，GC 追踪它们）。

### 9.9 任务对象

- **恢复补写**：任务对象持有协程、结果（`Task<T>` 的结果装箱存放）、等待者列表和 owner；完成时唤醒等待者，在下一个 `RunReady` 中恢复它们。

### 9.10 闭包对象

- 闭包环境（§5.5）是编译器生成的 class：被捕获的变量是字段，另有一个指向外层环境的字段。委托对象保存目标函数和接收者（环境或 `this`）。

## 10. 库

### 10.1 核心库与 Library

- **核心库**（`Language/Scripts/Core/`，命名空间 `Marlin`，始终可见）：
  - `object`、`string`（比较、哈希、`Substring`、`Contains`、`StartsWith` / `EndsWith`、`IndexOf` / `LastIndexOf`、`Replace`、`Trim*`、
    `ToUpperAscii` / `ToLowerAscii`、`Split` / `Join`、`Runes()`）、`Rune`、`StringBuilder`、`Number.TryParse`（整数、浮点、bool，与区域设置无关）。
  - `Array<T>` 成员（§3.5）、`Span<T>` / `ReadOnlySpan<T>`、`TypeInfo`、`IEquatable<T>`、`IEnumerable<T>` / `IEnumerator<T>`。
  - `Task`、`Task<T>`、`Frame`（§4.2）、`WeakRef<T>`（§9.2）、`Debug`（§10.3）、`Panic`、`AssertFailed`、`Action` / `Func` 委托。
  - `Random`：xoshiro256\*\*，同一个种子在所有平台上给出同一序列；`new Random()` 和 `Random.Shared` 从时钟取种子。提供整数与浮点范围、
    单位圆和单位球上的点、`Shuffle`。不用于密码学。
  - 数学、向量、`IComparable<T>`、`AttributeTargets` 等来自公共核心库 `Language/Common/Core/Marlin.mcommon`。
- **比较与哈希**：泛型代码直接调用 `Equals`、`GetHashCode`、`CompareTo`。class 用 `object` 的虚成员（字符串按内容），标量用核心库的扩展方法
  （`float` / `double` 把 NaN 当作等于自身、+0 等于 -0，NaN 排在最前），struct 自己声明 `Equals(T)` 和 `GetHashCode()`（以及排序需要的
  `CompareTo(T)`）；struct 没有声明时，在使用集合的地方报错。
- **Library**（`Language/Scripts/Library/`，需要 `using`）：
  - `Marlin.Collections`：`List<T>`、`Dictionary<TKey, TValue>`、`HashSet<T>`、`Queue<T>`、`Stack<T>`、`KeyValuePair`，以及数组和 `List<T>`
    的稳定排序 `Sort`（归并排序，相等元素保持原顺序）。它们建立在 `T[]` 上，所以每种元素类型有自己的字节码，值类型不装箱；`foreach` 不分配。
    字典的枚举顺序在没有删除时是插入顺序，删除后新元素填进空位——**不保证**任何顺序。
  - `Marlin.Tasks.Wait`（§4.2）。
- 核心库尽量用脚本写成；native 只做批量内存操作（`Array.CopyTo` / `Clear`）和字符串处理。

### 10.2 格式化

- 插值字符串和 `StringBuilder.Append(value, format)` 的格式说明：`D<n>`（十进制，至少 n 位）、`X<n>`（十六进制，大小写跟随说明符）、
  `F<n>`（定点，默认 2 位小数）、`E<n>`（指数，默认 6 位）、`G`（最短往返表示，默认）。其他说明符是 `Panic` 故障
  （`the format specifier is not D, X, F, E or G`）。`bool` 输出 `True` / `False`。所有格式都与区域设置无关。

### 10.3 Debug

- `Debug.Print(message)` 写日志（`LogScript`）；`Debug.Assert(condition, message)` 条件为假时以 `AssertFailed` 故障结束当前入口。
- 两者都标了 `[DebugOnly]`：只有编译选项打开调试输出时（编辑器和 Development 构建，`LanguageCompileOptions.DebugOutput`）调用才存在；
  关闭时整条调用连同参数一起删除，参数不求值。`[DebugOnly]` 是编译器认识的属性，可以用在任何返回 `void` 的函数上。

## 11. 引擎集成

### 11.1 编译流程

- Script 编译（`LanguageCompileOptions.Language = Script`）的源集是 `.mscript` 文件加上所有 `.mcommon` 文件（§1.3）。编辑器、Converter 和测试按包编译：
  检查 → 降低到 MIR → 通用 MIR pass → ScriptBackend 生成字节码模块 → 存进 GlobalStore（以包的源文件内容哈希为 key）。打包时字节码模块放进 mpak，
  Shipping 只带 VM，不带编译器（08 §4）。
- **实现现状**：`Developer/ScriptPipeline` 的 `CompileScriptPackage` 走上面的流程；`ScriptModuleConversion` 把结果作为 `ScriptModuleAsset` 存进 GlobalStore，
  开发期由 Indexer 的 `Script.Locate` 和 `GlobalStoreScriptStore` 定位，打包时由 `MarlinPackager` 打进包，运行时由 `ScriptModuleLoaderSubsystem` 加载进
  `ScriptVM`（06 §3.8）。脚本测试（§15）仍在测试程序里直接编译。

### 11.2 ECS

- 脚本 `struct` 可以作为组件（`[Component] struct Health { public float Value; }`），布局由 VM 计算后注册为动态 TypeInfo；`[System]` 函数从参数推导
  Query 和读写集合，加入调度 DAG（05 §3）。
- 普通脚本 System 遍历 chunk 列（08 §5.1）。**待定**：`Span<T>` 现在只能指向 GC 数组（§3.5），chunk 列不是 GC 对象。步骤 7 决定 chunk 列的视图：
  另一个 `ColumnSpan<T>`（句柄 + 列偏移 + 长度，每次访问校验 chunk 版本），或让 VM 的栈图认识内部指针。
- 引擎对象以句柄形式出现：`EntityRef`（值类型），`AssetRef<T>`（GC 对象持有引用计数，被回收时释放；步骤 7）。

**实现现状**（第一版，步骤 7 的一部分；声明在 `Language/Scripts/Core/World.mscript`，C++ 在 `ScriptVM/ScriptWorld.h`）：

- **属性进模块**：编译器把用户声明的属性 struct（`[Attribute]`，MarlinLanguage §3.7）在包里的 struct 和函数上的应用记进 MIR（`MirAttribute`）和
  字节码模块的 `Attributes` 段（`ScriptAttribute`：目标是 struct 形状或函数、属性 struct 的全限定名、每个字段的值的文本），引擎按名字读。
  函数记录另有 `ReadOnlyParameters`（`in` 参数的位）。脚本模块里的 struct 形状用带命名空间的全名（`Game.Health`）。字节码格式版本因此升到 2。
- **组件**：`[Component]`（`Marlin.ComponentAttribute`）的 struct 在 `ScriptVM::AddModule` 时注册：由形状建一个动态 TypeInfo（`Reflection::RegisterDynamicType`，
  名字是全名，`Module` 是包名，元数据 `Component`），再 `Components::RegisterDynamic`。字段只能是数字、bool、enum、`Entity`（映射到 C++ 的 `Entity`，
  所以 CommandBuffer 和 EntityTemplate 会修补它）、`float2..4`（`Vector2..4`）、`int2..3`（`IntVector2..3`）和由它们组成的 struct（嵌套的 struct 也注册成
  动态类型）；脚本布局必须和 C++ 类型同大小，否则不注册并报错。新值全零（字段初始值尚不支持）。
- **进程级、按名字**：组件和 System 类型是进程级的，所有 World 的 domain 共用；同一个名字只注册一次，之后再声明必须布局相同（否则报错、保留旧的，
  重启才生效）。临时决定：一个进程最多 1024 个脚本 struct（`TypeOperations` 没有上下文，每个 struct 占一个操作槽）。
- **System**：`[System(Phase = "…", Sets = "…", UpdateAfter = "…", UpdateBefore = "…")]`（`Marlin.SystemAttribute`，字段是字符串，写法同 C++ 的
  `MCLASS(...)` 元数据，多个名字用逗号分隔）的函数注册为一个动态的 System 类型（名字是去掉参数表的全名，如 `Game.Move`，父类 `ISystem`），
  调度器用 `Systems::RegisterFactory` 登记的工厂创建它（05 §3）。参数只能是 `ref` 组件（写）、`in` 组件（读；小 struct 按值传入）和一个 `Entity`；
  返回 `void`，不能是 async 或迭代器。名字指向的 Phase、Set 和 System 在注册前检查（调度器遇到错的名字会终止进程），不对的 System 不注册并报错，
  指向它的 System 也一样。
- **运行**：脚本 System 对 Query 选中的每个实体调用一次函数（`ref` / `in` 传 chunk 列里该行的地址，GC 不追踪 heap 之外的地址），没有组件参数时每次运行
  调用一次。每个 World 的脚本 System 都写同一个资源 `ScriptDomainAccess`，所以它们互相串行（domain 一次只跑一个 VM 入口），和 C++ System 照常并行。
  故障只结束那一次调用；一次运行里的故障计数并记录第一个。还没有 chunk 列视图（上面的待定问题），循环在 C++ 里，每个实体调用一次函数，
  一次运行的调用共用一个 `ScriptBatchCall`（§8.1）。
- **实体 API**：`Entity`（`Index`、`Generation`、`IsNull`、`IsDeferred`、`IsAlive()`、`Destroy()`、`Has<T>()`、`Get<T>()`、`Set<T>(value)`（添加或替换）、
  `Remove<T>()`）、`Entities.Create()`、`Time.Delta`（当前 System 的步长，帧边界是帧间隔）。都作用于 domain 所属 World（`ScriptDomain::GetWorld`），
  和 `EntityRef` 一样：System 运行期间的结构变更进 World 的延迟 CommandBuffer，`Entities.Create()` 返回延迟实体，在同一阶段结束前可以对它 `Set`。
  组件类型由 native 从泛型实例化的形状名找到（`Has` / `Remove` 用一个不读的同类型参数带出类型）。不是组件的类型是 `Panic` 故障。
- 尚未实现：chunk 列视图和 VM 内循环、C++ 组件在脚本里可见（`.gen.mscript`，§7.3）、`AssetRef<T>`、`ScriptBehaviour`（05 §4）、
  `[Kernel, System]`、Shared / Tag / Enableable 等存储方式、编译期检查 System 的参数（现在在注册时检查）。

## 12. 热重载

- **恢复补写**（按 08 §6）：文件保存 → 增量编译受影响的包 → 新模块推送给运行中的进程。只改了函数体时，在帧边界替换函数表，正在运行的栈帧执行完旧版本；
  改了类型布局时，按字段名迁移实例（含 ECS 列中的脚本组件），无法迁移的字段取默认值并警告。挂起中的任务在旧版本的代码里继续运行到结束。
- **实现现状**：只有「冷」重载。脚本文件改动后 Indexer 让模块失效并推送 `Asset.Changed`，`ScriptModuleLoaderSubsystem` 把新模块换进 `ScriptVM`；
  每个 `ScriptWorldSubsystem` 在下一帧边界丢掉旧 domain、建一个装了所有模块的新 domain，并让 World 重建 System（新的脚本 System 加入）。
  脚本的静态变量和任务从头开始，不迁移；组件布局的改动被拒绝（§11.2），要重启进程。模块在 World 启动之后才到达时走同一条路。

## 13. Kernel

- **恢复补写**：`[Kernel]` 按 08 §5.4 推迟实现。现在预留的位置：`MirFunctionKind::Kernel` 和字节码的函数种类字段；kernel 只能使用公共子集（§4.4）。

## 14. 指令集

### 14.1 指令格式

- 寄存器式字节码，指令定长 32 位，操作数形式（`ScriptOperands`）：`A`、`AB`、`ABC`、`ABx`、`AsBx`、`sAx`；需要时跟扩展字。
  寄存器槽 128 位，向量和数学类型以值存放在寄存器中。指令按类型特化（`ScriptNumber`：`I8` … `U64`、`F16`、`F32`、`F64`、`Bool`），
  内存访问按宽度和种类（`ScriptAccess`：`I8`、`U8`、`I16`、`U16`、`B32`、`B64`、`Bytes`、`Reference`、`Typed`），写引用时经过写屏障。
- 分派用 computed goto；`switch` 用跳转表（`ScriptSwitchTable`）。
- 循环的安全点是 `Loop`（`sAx`）：循环头一条偏移为 0 的，每条无条件回边用一条跳回头部 `Loop` 之后的 `Loop` 代替 `Jump`，所以每次迭代只多一次分派。
  格式版本 4 起如此。
- 和常数做的 32 位整数掩码与移位、F32 加减乘有 K 形式（`AndI32K`、`ShlI32K`、`ShrI32K`、`ShrU32K`、`AddF32K`、`MulF32K`）；`Cross`、`Normalize`
  对 `float3` 有专用指令。Script 的 MIR 流水线用 `sroa-keepvectors`：只被整体读写的向量变量不拆成分量，留在一个 128 位寄存器里。
- 调用窗口（被调函数的帧从这里开始）不再固定在所有值之上：ScriptBackend 的寄存器分配先给跨调用存活的值分配，每个调用的窗口从跨它存活的值和帧变量之上开始，
  其余值（没有调用跨过它们）优先住进它们作为实参或结果所在的窗口寄存器，所以大多数实参和结果不用搬运。实参进窗口按并行移动发出；
  调用处的栈图不含窗口里的寄存器，它们由被调函数的帧（或 native 的根窗口）覆盖。
- 加载时把「只搬寄存器、只调用空函数、不返回值」的函数标为空（`ScriptFunctionInfo::Empty`，如没有字段初始值的构造函数），对它的调用直接跳过。
  **注意**：将来的调试器若要在这类函数里下断点，需要关掉这个优化。

### 14.2 脚本操作

- 对象：分配对象和数组、字段与元素地址、长度、装箱与拆箱、类型测试与转换、`typeof`、空检查。脚本类型按它在模块类型表中的下标命名。
- 调用：直接调用、虚调用（虚表槽）、interface 调用（接口方法表加每个调用点的内联缓存，`ScriptInterfaceSite`）、委托调用（多播时依次调用列表中的每一项）、
  native 调用。
- 协程：`Await`（操作数是任务；已完成时直接取结果，否则挂起当前协程并登记为等待者）、`Yield`（迭代器交出当前值或结束）。
- **恢复补写**：完整的操作码表由 `ScriptBytecode.h` 的 `ScriptOpInfo` 表给出，以代码为准，本文不重复。

## 15. 测试

- `Language/Scripts/Tests/` 中带 `[Test]` 的函数是脚本测试：测试程序为每次运行编译一次脚本包（`ScriptTestCatalog`），每个 `[Test]` 函数注册为一个测试，
  名字是 `Scripts.<包>.<函数>`（例如 `Scripts.Engine.ArraysCopyOverlappingRanges`）。`async Task` 的测试一直运行到任务结束；测试中每帧推进 1/60 秒的
  World 时间，所以可以测试延迟和超时。
- `Marlin.Tests.Assert`（`Tests/Assert.mscript`）：`True`、`False`、`Equal`（int、long、float、double、bool、string）、`Fail`；失败时调用核心库的
  `AssertFailed`，以 `AssertFailed` 故障结束测试。任何故障都让测试失败，故障消息和脚本调用栈写进测试输出。

## 16. 实现步骤与状态

> **恢复补写**：原文的步骤表没有留下；下面按提交记录（LOST-WORK.md）和会话记录重建，步骤号以会话中引用的为准（步骤 5、7），其余为推断。

| 步骤 | 内容 | 状态（10/28 01:10） |
| --- | --- | --- |
| 1 | 本规范 | 完成（rev 99） |
| 2 | Script 方言的解析和类型检查：class、interface、委托与事件、lambda、可空性与流分析、模式、async / 迭代器 | 完成（rev 106；`BinderPatterns.cpp` 等后续改动未提交） |
| 3 | 降低到 MIR：脚本类型与操作、闭包环境、async / 迭代器的函数种类 | 完成（rev 119 修复了每次迭代的闭包） |
| 4 | ScriptBackend（MIR → 字节码）与 ScriptVM（加载、解释器、GC、任务、ScriptDomain） | 完成（rev 120、127） |
| 5 | 核心库与 Library：集合、文本、`Random`、`Wait`、`Span<T>`、`WeakRef<T>`、`Debug` | 集合、文本、`Random`、`Wait` 完成（rev 141、143、146）；`Span`、`WeakRef`、`Debug` 已实现，未提交 |
| 6 | 热重载与调试（DAP） | 未开始 |
| 7 | ECS 集成（组件、System、chunk 列视图，§11.2）、`AssetRef<T>`、`.gen.mscript` | 组件、System、实体 API 和模块交付（§11.1）完成（10/28）；chunk 列视图、`AssetRef<T>`、`.gen.mscript` 未开始 |
| 8 | Kernel（§13） | 推迟 |

## 17. 待定问题

1. **chunk 列的视图**（§11.2）：`ColumnSpan<T>` 还是让 GC 认识内部指针。倾向前者，不需要改 VM。
2. **共享泛型代码**：现在全部单态化（08 §2）；引用类型实参共享一份代码是以后的优化。
3. **JIT**：PC 上将来可能加，字节码保留类型信息和基本块边界（08 §5.1）。
