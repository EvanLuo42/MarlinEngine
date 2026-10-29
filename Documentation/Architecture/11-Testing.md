# 11 · 测试

## 1. 框架

`Source/Runtime/Testing`：自研框架，不依赖 std，可以在主机上运行。

```cpp
MARLIN_TEST(Containers, ListGrowsGeometrically)
{
    List<int32> list;
    for (int32 i = 0; i < 100; ++i) list.Add(i);
    MARLIN_EXPECT(list.Count() == 100);
    MARLIN_REQUIRE(list.Capacity() >= 100);   // 失败时立即结束当前测试
}

MARLIN_TEST_FIXTURE(JobsFixture) { /* Setup / Teardown */ };
MARLIN_TEST_F(JobsFixture, Jobs, WaitDoesNotBlockThread) { … }
```

- 测试通过 HeaderTool 生成的表注册，不用静态构造器，这和反射注册的做法一致（04 §2）。
- 测试名是 `Module.Suite.Name`，可以按 glob 过滤，也可以加 `[Slow]`、`[Gpu]`、`[Platform(PS5)]` 等标签。
- 没有异常，所以 `REQUIRE` 失败后用 fiber 的 longjmp 风格跳出（整个测试运行在一个独立的 fiber 中，失败时切回 runner 的 fiber，再丢弃该 fiber 的栈）。

**实现现状**（`Testing/Testing.h`）：

- 标签写在名字后面，不带方括号：`MARLIN_TEST(Jobs, Stress, Slow)`、`MARLIN_TEST(Render, Shadows, Gpu, Platform(PS5))`。
  `Slow`、`Gpu`、`Benchmark` 默认不跑，用 `--tags=Slow` 或 `--all` 打开；`Platform(X)` 只在平台 X 上跑。其他标签只是标记。
- 夹具：`MARLIN_TEST_FIXTURE(Name) { … };` 派生自 `TestFixture`，重写 `SetUp`/`TearDown`；每个测试一个新对象。`REQUIRE` 失败时跳过 `TearDown`，栈上对象也不析构。
- 断言：`MARLIN_EXPECT`、`MARLIN_REQUIRE` 和带 Format 消息的 `MARLIN_EXPECTF`/`MARLIN_REQUIREF`；失败按 `file(line): error: …` 打印，连着调试器时会断下。
  `Testing::Print` 给 benchmark 输出数字。
- HeaderTool 扫描源文件里的 `MARLIN_TEST`/`MARLIN_TEST_F`，在模块的 `Register_<Module>` 里调用 `Testing::RegisterTest`；
  `<X>Tests` 模块里的测试名为 `X.Suite.Name`，同一模块内重名报错。测试必须在具名命名空间里。
- 每个测试在 4 MiB 栈的 fiber 里运行，可以等 Job（主线程等待时就地执行 Job）。
- runner 参数：过滤器（`*`/`?` 通配，不带通配的过滤器也匹配其下所有测试，`-` 开头表示排除）、`--list`、`--all`、`--tags=A,B`、`--junit=<path>`。
- **Visual Studio Test Explorer**：runner 也接受 Google Test 的命令行约定（`--gtest_list_tests`、`--gtest_filter`、`--gtest_output=xml:`），这时测试名是 `Suite.Name`（不带模块，所以 suite 名在所有模块间必须唯一，重复时 runner 报错）；
  测试函数体是 `Suite_Name_Test::TestBody`，适配器靠它找到源码行。`marlin generate` 在 sln 旁写 `Marlin.gta.runsettings`，告诉 VS 自带的 Google Test 适配器哪个 exe 有测试。
  不依赖 gtest；要支持 doctest/Catch2 风格的适配器，只需再识别它们的命令行参数。
- 测试代码的 `.clang-tidy`（`Tests/` 目录）放宽了内部链接、const 和刻意拷贝相关的检查。
- **文件系统**：测试只在自己建的唯一目录里创建和删除文件，放在 `/Temp` 或工作区的 `Saved/` 下，基路径必须非空并且检查过；
  递归删除受 `PlatformFile::IsSafeTreeRoot` 保护（02 §5.6），拒绝空路径、根目录、exe 所在目录及其上级。
- 还没有：benchmark 的统计和趋势记录（目前只打印），按测试设超时，主机 devkit 上运行。

## 2. toolkit 集成

**决策**：每个模块下有一个 `Tests/` 目录，再用一个合并的 runner 统一运行。

- 模块目录下如果有 `Tests/`，`marlin generate` 就把它当作测试模块 `<Module>Tests`（已实现）：
  - 它依赖被测模块、被测模块的全部依赖，以及框架模块 `Testing`，可以看到被测模块的 `Public/`、`Internal/` 和 `Private/` 头文件（测试模块是唯一的例外）。
  - 被测模块的源码扫描会排除 `Tests/`。
  - 需要额外依赖时，在 `Tests/` 里放一个 `<Module>Tests.module.py`。
  - 任何模块都不能依赖测试模块。
- 只有设置了 `test_modules = True` 的 target 才会把测试模块编进去，通常是 `MarlinTests`（Program 类型）。target 容纳不了的测试会被跳过：比如 Editor 模块的测试不能进 Program，Developer 模块的测试不进 Shipping，不支持当前平台的测试也会跳过。
  Editor 模块的测试放在 `MarlinEditorTests`（Editor 类型的测试 target，launch 模块同样是 `MarlinTests`）。两个 target 在 Windows 和 Linux 上都链接 `RHIVulkan`，
  这样 `Gpu` 标签的 RHI 测试有后端可用（`-Rhi=` 选后端）。
- modular 构建下，测试模块链接的是被测模块的 DLL，所以只能调用导出了 `*_API` 的符号；要测未导出的内部实现，就用 monolithic 配置（Profile/Shipping）或者给它加上导出。
  测试模块本身不做成 DLL，而是直接链接进测试 exe（没有模块依赖它），这样测试函数的符号在 exe 的 PDB 里，IDE 的测试适配器才能找到源码位置。
- `marlin test [filters...] [-c Development] [-t MarlinTests] [--no-build] [--list] [-- 其他参数]`：
  先构建本机平台和架构上的测试 target，再运行；filter 和 `--` 之后的参数原样传给测试可执行文件（例如 `-- --junit out.xml`）。
  测试 target 的列表由 generate 写进 `Intermediate/FASTBuild/Generated.json` 的 `tests` 字段。
  主机的 devkit 部署后运行、结果通过 Remote 层传回，**待定**。
- `marlin coverage`（已有命令）用 clang 的 source-based coverage 运行 `MarlinTests`。

## 3. 测试种类

| 种类 | 位置 | 运行方式 |
| --- | --- | --- |
| 单元测试 | `<Module>/Tests/` | `MarlinTests` |
| 功能测试 | 项目或引擎 `Content/Tests/*.masset`（测试关卡）+ 脚本 | `Game -Test=Name` 或者在编辑器中：加载关卡，跑 N 帧或等脚本发出完成信号，期间检查断言 |
| 脚本测试 | `.mscript` 中的 `[Test]` 函数 | 在 VM 中运行，结果并入同一份报告 |
| 编译器测试 | `Developer/LanguageFrontend/Tests/Cases/*.mshader`，带期望输出注释 | 类似 lit/FileCheck；每个 shader 后端还会和参考路径（HLSL+DXC）在 GPU 上对比结果 |
| 图像对比 | `Content/Tests/Rendering/` | §4 |
| 性能测试 | 标了 `[Benchmark]` 的测试 | 输出统计，CI 记录趋势，超过阈值就报警 |

## 4. 图像对比

**决策**：使用带容差的参考图。

- 测试场景加固定相机和固定时间 → 截图 → 和 `Content/Tests/Rendering/Reference/<Platform>/<GpuVendor>/<Name>.exr` 用 **FLIP** 对比。
- 每个测试可以配置容差（平均值和最大值），超出容差时输出差异热力图，并作为 CI artifact 上传。
- 参考图的更新必须手动审批（`marlin test --update-references`，提交前检查 diff）。
- 确定性：固定随机种子，关闭 TAA 的抖动或固定抖动序列，跳过时域累积帧，或者固定渲染 N 帧后再截图。
