# 16 · 文档：手册与 API 参考

`marlin doc` 生成引擎的文档站点，包括两部分：

- **手册**（guide / manual）：`Documentation/` 下手写的 Markdown 书。
- **API 参考**：从 C++、`.mscript`、`.mshader`、`.mcommon` 的 `///` 文档注释中抽取。四种语言用同一种写法，写法参照 Rust：正文是 Markdown，链接写符号名。

两部分共用一套链接：手册可以直接链接到符号（`` [`World::Spawn`] ``），API 文档也能链接回手册的章节。
默认输出静态网站；`--data-only` 只输出中间数据（JSON），留给其他消费者使用，比如编辑器内帮助、外部站点、IDE 插件。

**实现现状**：`Source/Programs/MarlinDoc` 和 `marlin doc` 已能输出 §4 的 JSON 数据：
- 已实现：C++ 抽取（§3.1）和 Language/ 库抽取（§3.2），手册的书（§2），链接解析和 lint（§3.4、§6）。
- Tooltip 只取第一段（§1.1）已实现。
- 整个引擎（68 个 C++ 模块、7 个库、约 1.3 万个条目）一次大约 7 秒。

与设计的偏差和未实现的部分：
- **静态站点（§5）推迟**。`marlin doc` 不论加不加 `--data-only` 都只写数据。`Source/ThirdParty/Md4c` 已经放好，但还没有模块使用它。
- 抽取 Language/ 库时只解析语法，不做类型检查。脚本的链接和 C++ 一样，在符号表上按作用域近似查找，没有用前端的名字解析。
- Language/ 下的库按 `<方言目录>.<库目录>` 命名为模块（`Scripts.Core`、`Shaders.Passes`），`Tests/` 不算。
  脚本文件开头写在 file-scoped `namespace` 前的 `///` 当作文件文档。
- C++ 注释用 §3.1 的退路实现：从声明往上扫 token，没有用 `clang_Cursor_getRawCommentText`。宏写出来的声明（`MARLIN_NONCOPYABLE` 等）不进文档；
  带 `///` 的宏会作为 `macro` 条目收录。
- `missing-docs` 默认只按模块打印数量，`--missing-docs` 才逐条列出。
- MarlinDoc 有警告时退出码是 2。`marlin doc` 只在 `--deny-warnings` 时把它当作失败。
- 未实现：`projections`（§3.3，要等 `.gen.mscript` 做出来）、增量（§6）、文档测试（§7）、`doc-summary-too-long`。

## 1. 注释写法

**决策**：

- `///` 写在声明之前，描述这个声明；`//!` 写在文件开头（第一个声明之前），描述整个文件。普通的 `//` 和 `/* */` 不进文档。
  在 `.mscript` / `.mshader` / `.mcommon` 里，`///` 本来就是文档注释（MarlinLanguage §2.1）；C++ 里 clang 也把 `///` 当文档注释。
- 正文是 CommonMark（加表格），和手册的 Markdown 是同一种方言。**第一段是摘要**，用于列表、`Tooltip`、LSP hover 和搜索结果。
  摘要写成一句话，不重复声明名。
- 参数、字段等名字写成 `*name*`，直接放在句子里；不用 `@param`、`\brief` 这类 Doxygen 命令，工具也不解析它们。
- 需要分节时用普通的 Markdown 标题：`# Examples`、`# Threading`、`# Errors`、`# Performance`……
  **标题集合是开放的**，没有枚举。站点主题认识哪个就特殊处理哪个，其余照常渲染。
- 链接沿用 rustdoc 的写法，链接目标是符号名，不是 URL。手册页面里也可以这样写：

  | 写法 | 含义 |
  | --- | --- |
  | `` [`CompressionCodecSet`] `` | 链接文字就是符号名 |
  | `` [codecs](`CompressionCodecSet::Find`) `` | 自定义链接文字 |
  | `` [`cpp@Engine::GetCompressionCodecs`] `` / `` [`mscript@Marlin.Gameplay.Damage`] `` | 指定语言（跨语言链接） |
  | `` [`Find(StringView)`] `` | 用参数类型区分重载 |
  | `[材质](manual:Architecture/07-Rendering#材质)` | 手册页面（`manual:<书>/<页>#<标题>`） |

  不带 `@` 时，先在同一种语言里按声明所在的作用域查找，查不到再查另一种语言的投影（§3.3）。
  手册页面没有作用域，符号要写全名，或者写成足够唯一的后缀。显示时 `@` 前缀会被去掉。普通的 `[text](https://…)` 链接原样保留。

```cpp
//! Block compression codecs and the per-process set of them.

/// The codecs linked into the process, by name. Immutable once built.
///
/// [`Engine`] owns the process's set: see [`Engine::GetCompressionCodecs`].
///
/// # Threading
/// Safe to read from any thread.
class CORE_API CompressionCodecSet
{
public:
    /// The codec named *name*, case-insensitively, or null.
    [[nodiscard]] const CompressionCodec *Find(StringView name) const;
};
```

```csharp
/// Bends a grass blade away from the wind at *position*.
///
/// Shared by the CPU kernel and the GPU compute pass (08 §5.5), so both see the same shape.
///
/// # Examples
/// ```mscript
/// float bend = WindBend(float3(0, 0, 0), 1.0f);
/// Assert(bend >= 0.0f);
/// ```
public float WindBend(float3 position, float time) { … }
```

### 1.1 Tooltip

**决策**：反射元数据 `Tooltip` 只取**第一段**，并去掉 Markdown 标记（`*name*` 变成 `name`，链接只留文字）。
完整的文档进 `marlin doc` 的数据，不进运行时的 TypeInfo。显式写了 `Tooltip=` 的，仍以显式的为准（04 §1）。

**实现现状**：已实现（HeaderTool 的 `StripMarkdown`）。Remote 方法的描述和 JSON Schema 的 `description` 也来自 `Tooltip`，所以现在同样只有第一段。

## 2. 手册

**决策**：`Documentation/` 下每个含有 `Summary.md` 的目录是一本**书**。书的列表由目录决定，不在任何地方登记。

- `Summary.md` 是目录：一个 Markdown 列表，每一项链接一个页面，缩进表示层级（和 mdBook 的 `SUMMARY.md` 一样）。
  没有列进目录的 `.md` 文件不会发布，`marlin doc` 会给出警告。
- 第一行的 `# 标题` 是书名。书在站点导航中的顺序由 `Summary.md` 开头的一行 `<!-- order: 10 -->` 决定，没有这一行的书按名字排序。
- 现有的 `Architecture/`、`Languages/` 加上 `Summary.md` 就成为书；面向引擎使用者的手册以后放在新的书里（比如 `Documentation/Manual/`），
  加书时再定目录名。
- 页面之间的相对链接（`[04](04-Reflection.md)`）照常可用，生成时改写成站点里的地址；指向不存在的页面会报 `broken-doc-link`。
- 正文里像「04 §1」这样的引用只是文字，不会自动变成链接。

## 3. API 抽取

**决策**：一个 C++ Program `MarlinDoc`（`Source/Programs/MarlinDoc`，可以使用 std），按模块运行。两个前端产出同一个数据模型。

### 3.1 C++：libclang

- 用 libclang 的 C API 解析 C++，**不**扩展 HeaderTool。HeaderTool 只懂带标记的声明，看不懂就报错（04 §2），
  而文档要覆盖所有公开 API，包括模板、自由函数和没有反射的类型。
- libclang 来自 toolkit 已经找到的那份 LLVM：查找顺序是 `toolchain.llvm_root`、`$LLVM_ROOT`、默认安装位置，见 toolkit 的 `find_llvm`。
  Windows 上是 `bin/libclang.dll`，Linux 上是 `lib/libclang.so`，macOS 上是 `lib/libclang.dylib`。LLVM 官方安装包都带这个库，
  所以不往仓库里放二进制；它和编译用的 clang 是同一个版本，解析结果和真实编译一致。
  - `Source/ThirdParty/LibClang` 只放 `clang-c/*.h` 头文件（Apache-2.0 WITH LLVM-exception），`version` 写最低支持的 LLVM 版本。
    `MarlinDoc` 在**运行时**按 toolkit 传入的路径（`--libclang <path>`）加载这个库，链接时不依赖它。
    libclang 的 C ABI 只增不改，所以旧版本的头文件可以搭配更新的库。
  - Linux 发行版常把 libclang 拆成单独的包。找不到时，`marlin doc` 报错，并提示安装 `libclang-<N>` 或者设置 `toolchain.llvm_root`。
- 每个模块生成一个只 include 全部 `Public/` 头文件的临时源文件，用该模块的编译参数（和 compile database 相同）解析一次。
  这样还能顺带检查公开头文件能否单独编译。
- 注释取原文（`clang_Cursor_getRawCommentText`），由 MarlinDoc 自己去掉 `///` 前缀，不使用 clang 的 Doxygen 解析结果。
  反射宏（`MCLASS(...)`、`MPROPERTY(...)`）的参数，从 detailed preprocessing record 的宏展开游标中原样取出。
  - **风险**：注释和声明之间隔着 `MCLASS(...)` 时，clang 能否把注释挂到声明上，需要在原型里验证。
    如果挂不上，就退回到从声明起点往上扫 token，扫法和 HeaderTool 相同。
- **公开范围**：`Public/` 下的头文件，排除 `namespace Private`、`private:` 成员和 `Internal/`。`protected` 成员保留，并标出可见性。

### 3.2 `.mscript` / `.mshader` / `.mcommon`：LanguageFrontend

- 直接链接 `LanguageFrontend`，只跑到类型检查（不需要单态化和 MIR）。
- 词法器把 `///` 和 `//!` 作为 trivia 保留，附着到下一个声明或所在文件上。`MarlinLanguageServer` 的 hover 也要用这个能力（10），两边共用一份实现。
- 公开范围按 `public` / `internal` 修饰符判断。`internal` 的条目也会带出来，但会做标记。
- 链接按前端真实的名字解析处理，包括 `using`、命名空间和泛型参数。

### 3.3 C++ 到脚本的投影

- `MFUNCTION(ScriptCallable)` 和 `MPROPERTY(ScriptVisible)` 生成的 `.gen.mscript` 声明（08 §5.3）带上 C++ 的 `///` 原文。
- 数据里**只有一个条目**：语言是 `cpp`，`projections` 列出它在脚本中的全名和签名。脚本文档引用它时，链接到同一个 id。
  站点可以按读者选择的语言，显示 C++ 签名或脚本签名。

### 3.4 链接解析

**决策**：链接在抽取时解析，生成站点的阶段和外部消费者拿到的都是符号 id，不需要懂任何一种语言的名字查找。

- 脚本：用前端的作用域查找。
- C++：在抽取出的符号表上，近似模拟 C++ 的查找顺序：所在类 → 基类 → 外层命名空间 → 全局。
  不考虑 ADL，也不考虑 `using` 引入的名字，这类链接要写全名。
- 手册：按全名或唯一后缀，在所有模块的符号表里查找。
- 查不到时报 `broken-doc-link`；有歧义时报 `ambiguous-doc-link`，列出候选，并提示加上参数类型或 `cpp@` / `mscript@`。
  这两种情况下，链接都退化成纯文本。

## 4. 数据格式

**决策**：用 JSON，结构参照 rustdoc 的 JSON 输出。

为什么不用 MarlinText：MarlinText 的长处是 Schema 版本迁移、用 UUID 做对象链接、手写和 diff 友好、由 TypeInfo 驱动（06 §1），
这些对文档数据都用不上：文档数据不是引擎对象，没人手写，每次都整份重新生成。
而它的主要消费者是浏览器里的搜索和外部工具，都能直接读 JSON。

```
<out>/Data/
  index.json               书和模块的列表；符号 id → 名字 / 种类 / 摘要 / 所在模块；反向关系
  Modules/<Module>.json    一个模块的全部条目
  Books/<Book>.json        一本书：目录、每页的 Markdown 原文和链接表
```

```json
{
  "format_version": 1,
  "module": "Core",
  "kind": "Runtime",
  "docs": "…模块级文档，来自模块目录下的 Docs.md…",
  "items": {
    "cpp:Marlin::CompressionCodecSet::Find": {
      "language": "cpp",
      "kind": "method",
      "name": "Find",
      "parent": "cpp:Marlin::CompressionCodecSet",
      "visibility": "public",
      "span": { "file": "Source/Runtime/Core/Public/Compression/CompressionCodec.h", "line": 42, "column": 5 },
      "signature": [
        { "text": "[[nodiscard]] const " },
        { "text": "CompressionCodec", "ref": "cpp:Marlin::CompressionCodec" },
        { "text": " *Find(" },
        { "text": "StringView", "ref": "cpp:Marlin::StringView" },
        { "text": " name) const" }
      ],
      "attributes": [],
      "reflection": null,
      "projections": [],
      "docs": "The codec named *name*, case-insensitively, or null.",
      "links": {}
    }
  }
}
```

- `docs` 是 Markdown **原文**；`links` 是「原文中的链接目标 → 符号 id 或手册地址」。手册页面用同样的 `markdown` 加 `links` 表示。
- `signature` 是 token 流，类型带 `ref`，所以消费者能把签名里的每个类型都做成链接，不需要懂 C++ 或脚本的语法。
  另外按种类给出结构化字段：参数（名字、类型 token、默认值原文）、返回值、模板/泛型参数和约束、基类和实现的 interface、枚举值、字段类型。
- `kind`、`language` 是字符串，**不是封闭枚举**（`class`、`struct`、`enum`、`interface`、`function`、`method`、`field`、`constant`、`namespace`、
  `typedef`、`macro`、`extension`……）。以后出现新的种类时，消费者不认识的就按通用条目显示。
- `reflection` 原样带出反射宏的参数（`{"macro": "MPROPERTY", "arguments": "Replicated, Range=(0, 10000)"}`）；
  脚本的属性放在 `attributes` 里（`[Compute(64, 1, 1)]`、`[Component]`）。
- 反向关系（派生类型、interface 的实现、extension 方法、某个扩展点有哪些实现）在所有模块抽取完之后统一算出，写进 `index.json`。
- **符号 id** 的格式是 `<语言>:<全名>`，重载时追加参数类型（`cpp:Marlin::Foo::Bar(int32,float)`）。id 在构建之间保持稳定，站点直接用它生成 URL。
- `format_version` 只在做了不兼容的修改时加一。新增字段不算不兼容，消费者要忽略不认识的字段。
- 模块级文档写在模块目录下的 `Docs.md`（可选），相当于 Rust crate 根上的 `//!`。

**待定**：平台层覆盖的头文件（12 §1）只出现在对应平台的输出里。要不要把各平台合并成一份带平台标记的数据，等主机平台接入时再定。

## 5. 静态站点

**决策**：站点也由 `MarlinDoc` 生成，输入就是 §4 的数据，和外部消费者读的是同一份。所以 `--data-only` 只是跳过这一步。

- Markdown 用 md4c 渲染（C 写的 CommonMark 实现，支持表格，MIT 许可证），放在 `Source/ThirdParty/Md4c`。
  渲染时按 `links` 表把符号链接改写成页面地址；代码块按语言做语法高亮。
  mscript/mshader/mcommon 的高亮直接用 LanguageFrontend 的词法器，C++ 的高亮用 libclang 的 token。
- **页面**：每本书的每页对应一个页面；每个模块一个页面；每个类型、命名空间、自由函数组各一个页面，成员放在所属类型的页面里，用锚点定位。
  每个条目都带「源码」链接（仓库内的相对路径和行号）。
- **主题**：HTML 模板、CSS 和 JS 放在 `Source/Programs/MarlinDoc/Theme/`，跟着工具走。模板只做简单的占位符替换，不引入模板引擎。
  页面没有 JS 也能正常阅读；搜索和语言切换（C++ 签名 / 脚本签名）需要 JS。
- **搜索**：完全在客户端完成。生成 `search-index.js`，内容是符号名、摘要、手册标题，以全局变量的形式提供；
  用 `<script>` 而不是 `fetch` 加载，这样直接打开 `file://` 也能搜索。
- 输出的站点是自包含的目录，不需要服务器，可以直接放到任何静态托管上。

**待定**（站点推迟实现，下面是要满足的需求）：

- 顶部有几个 tab，在开发者文档、用户文档、API 参考（以后可能还有别的）之间切换。书怎么归到 tab 下，等做站点时再定。
- 支持 i18n：同一本书有多种语言的版本，站点可以切换语言。
- 版本切换（按引擎版本发布多份文档）可选，如果实现起来麻烦就不做。

## 6. `marlin doc`

```
marlin doc [modules...] [--data-only] [--no-manual] [--no-reference] [--out <dir>] [--deny-warnings] [--open]
```

- 和 HeaderTool 一样，先按宿主平台构建 `MarlinDoc` 的 Development 版本，再按模块并行抽取，最后汇总、解析跨模块链接、生成站点。
- 默认输出到 `Intermediate/Docs/<Platform>/`（`Data/` 和 `Site/`）；`--out` 换成别的位置。
  抽取按「输入文件 + 编译参数 + MarlinDoc 版本」的哈希做增量，没变化的模块不重跑。
- 不指定模块时处理全部模块；指定模块时，手册里指向其他模块的链接仍能通过上次的 `index.json` 解析。
- 不放进 `marlin build`：文档不影响编译产物，单独运行。
- 检查项（lint）：`broken-doc-link`、`ambiguous-doc-link`、`missing-docs`（公开 API 没有 `///`）、`doc-summary-too-long`、
  `unlisted-manual-page`。默认都是警告，`--deny-warnings` 时让 CI 失败。**待定**：按模块单独开关的写法。

## 7. 文档测试

**决策**（第二期）：文档注释和手册页面里的代码块都可以作为测试，做法参照 Rust 的 doctest。

| 代码块 | 处理 |
| --- | --- |
| ```` ```mscript ```` / ```` ```mcommon ```` | 编译并在 VM 中运行，`Assert` 失败就是测试失败 |
| ```` ```mshader ```` | 只编译 |
| ```` ```cpp ```` | 默认不处理；`cpp,compile` 只做编译检查 |
| `…,ignore` / `…,no_run` | 不处理 / 只编译 |

- `MarlinDoc` 把可测试的代码块写进数据（带源码位置），`marlin test` 把它们作为 `<Module>.Doc.*` 和 `Manual.<Book>.*` 测试运行（11 §2），
  失败时报告代码块所在的行。
- 代码块里可以省略外围的样板（`using`、所在命名空间），按声明所在的文件补全。
