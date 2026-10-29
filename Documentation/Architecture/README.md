# Marlin Engine 架构总览

本目录记录 Marlin Engine 的架构决策。每个文件只讲一个主题，标注 **决策** 的条目是已经定下来的，
标注 **待定** 的条目还需要再讨论。

| 文件 | 主题 |
| --- | --- |
| [01-Targets-and-Lifecycle.md](01-Targets-and-Lifecycle.md) | Game / Editor / Server / Program，启动与关闭流程，Subsystem，主循环 |
| [02-Paths-and-VFS.md](02-Paths-and-VFS.md) | 源码 / 安装 / 打包三种形态下的目录，引擎绑定，产品身份，VFS 挂载 |
| [03-Core.md](03-Core.md) | 禁用 std、内存与分配器、容器、字符串、Job 系统、数学、日志 / Profiler / CVar |
| [04-Reflection.md](04-Reflection.md) | 反射标记、MarlinHeaderTool、`.gen.h/.gen.cpp`、Asset 对象模型、注册 |
| [05-World-and-ECS.md](05-World-and-ECS.md) | Archetype ECS、System 调度、Prefab、层级、世界分区、网络复制 |
| [06-Assets-and-Pipeline.md](06-Assets-and-Pipeline.md) | MarlinText、`.masset`、Asset Indexer、Converter、GlobalStore、打包、热重载 |
| [07-Rendering.md](07-Rendering.md) | RHI、RenderGraph、Renderer（帧阶段、RenderSurface）、全局 shader、Visibility Buffer、材质 / BSDF、SceneRenderer 与视口 |
| [08-Languages.md](08-Languages.md) | 公共前端、`.mcommon/.mshader/.mscript`、公共 IR、Shader 后端、脚本 VM |
| [09-UI-and-Editor.md](09-UI-and-Editor.md) | UIPainter（tile 渲染与窗口合成）、UI 文字、DebugUI、编辑器 UI 框架（Signal）、资产编辑上下文、PIE |
| [10-Remote.md](10-Remote.md) | 对外运行时接口层（MCP / LSP / DAP / 工具），内建方法组 |
| [11-Testing.md](11-Testing.md) | 单元 / 功能 / 脚本 / 图像对比测试，toolkit 集成 |
| [12-Platforms-and-ThirdParty.md](12-Platforms-and-ThirdParty.md) | 平台层栈、源码叠加、平台 HAL（Core 与 Application）、ThirdParty 目录格式、扩展模块 |
| [13-Extension-Points.md](13-Extension-Points.md) | 模块扩展引擎的方式：Provider、Observer 接口、调度工作、数据贡献；资产导入等场景 |
| [14-Net.md](14-Net.md) | 传输层：完成端口、NetLoop 与 IO 线程、流式连接和监听、数据报 |
| [15-Input.md](15-Input.md) | 输入设备与控件、InputAction / InputContext 资产、Fixed / Variable 下的读取、改键 |
| [16-Documentation.md](16-Documentation.md) | `///` 文档注释、手册的书、MarlinDoc（libclang + LanguageFrontend）、JSON 数据、静态站点、`marlin doc`、文档测试 |

## 设计准则

所有决策都用一个问题检验：**这个设计放到一个 3A 项目里还成立吗？** 也就是要撑得住几百万个资产、几百人同时
开发，要能跑在主机上，也要在 Shipping 构建里做到零开销。

## 不变量

这几条是全局约束，任何模块都不能破坏：

1. **全系 clang**。PC 上用 LLVM clang，Windows 用 msvc triple 加 lld-link；不做交叉编译。C++20，不用 modules，也不用 coroutines。
2. **no-exceptions、no-RTTI**。引擎模块一律如此；只有 Program 专属模块可以单独开放（见 01）。
3. **Runtime 禁止 std 头文件**。只允许语言机制必需的头：`<initializer_list>`、`<new>`（placement new）、
   `<type_traits>` 中编译器内建的那一部分（优先用 `__is_*` / `__builtin_*` 内建）、`<cstddef>`、`<cstdint>`、`<compare>`。
   容器、字符串、分配器、原子、线程全部自研，能用 clang intrinsic 的都用 intrinsic。
4. **Runtime 里没有 `WITH_EDITOR` 一类的宏**。同一个模块在 Editor / Game / Server / Program 之间共享同一份编译产物，
   差异只通过「链接哪些模块」来体现。
5. **游戏是纯数据加脚本**。所有游戏共用同一个引擎 fork 编出来的 Game 可执行文件，游戏工程里不写 C++。
6. **命名遵循 C# 规范**：类型、方法、属性、公有字段、枚举值、命名空间和文件名都用 `PascalCase`；参数和局部变量用 `camelCase`；
   私有字段用 `_camelCase`；接口加 `I` 前缀；模板参数用 `T` 前缀（`TAllocator`）。宏用 `MARLIN_UPPER_CASE`，反射宏除外（`MCLASS` 等）。
   根命名空间是 `Marlin`。
