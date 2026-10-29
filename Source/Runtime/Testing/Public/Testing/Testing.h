#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "String/StringView.h"

namespace Marlin
{
/// A test as MarlinHeaderTool registers it from MARLIN_TEST or MARLIN_TEST_F.
struct TestInfo
{
    StringView Module;
    StringView Suite;
    StringView Name;
    /// Comma-separated, as written after the name: Slow, Gpu, Benchmark, Platform(PS5) or any other word.
    StringView Tags;
    void (*Run)() = nullptr;
    /// In place of Run, for a test a source registers at run time: called with Context.
    void (*RunWith)(const void *context) = nullptr;
    const void *Context = nullptr;
    StringView File;
    uint32 Line = 0;

    [[nodiscard]] TESTING_API bool HasTag(StringView tag) const;
};

/// The base of MARLIN_TEST_FIXTURE types: SetUp runs before the test body and TearDown after it, on the same object.
class TestFixture
{
public:
    TestFixture() = default;
    virtual ~TestFixture() = default;
    MARLIN_NONCOPYABLE(TestFixture);

    virtual void SetUp()
    {
    }

    virtual void TearDown()
    {
    }
};

namespace Testing
{
/// Called by the generated Register_<Module> of test modules.
TESTING_API void RegisterTest(const TestInfo &test);

/// Registers a MARLIN_TEST_SOURCE: a function RunTests calls once before it selects tests, which registers the tests it
/// finds, such as the `[Test]` functions of scripts (Documentation/Architecture/11 §3). Their names must outlive the
/// run.
TESTING_API void RegisterTestSource(void (*source)());

/// Runs the registered tests that *arguments* select and returns the exit code, 0 if every test that ran passed.
/// Arguments: name filters matched against Module.Suite.Name, where `*` and `?` are wildcards, a filter without
/// wildcards also matches everything below it and a leading `-` excludes; `--list`; `--all`, which also runs tests
/// tagged Slow, Gpu or Benchmark; `--tags=A,B`, which adds tests with those tags; `--junit=<path>`. A test named
/// exactly runs whatever its tags. The Google Test options `--gtest_list_tests`, `--gtest_filter` and
/// `--gtest_output=xml:<path>` switch to its Suite.Name names, so IDE test adapters can list and run the tests.
TESTING_API int32 RunTests(Span<const StringView> arguments);

/// Writes *text* to the test output, such as a benchmark's figures.
TESTING_API void Print(StringView text);

/// Records a failed check of the running test; a fatal one ends the test at once, without unwinding its stack.
TESTING_API void Fail(const char *expression, const char *file, int32 line, StringView message);
[[noreturn]] TESTING_API void FailFatal(const char *expression, const char *file, int32 line, StringView message);
} // namespace Testing

namespace Private
{
template <typename... TArgs>
MARLIN_NOINLINE void TestFailFormatted(bool fatal, const char *expression, const char *file, int32 line,
                                       FormatString<::Marlin::TypeIdentity<TArgs>...> format, const TArgs &...args)
{
    StringBuilder<512> message;
    FormatTo(message, format, args...);
    if (fatal)
    {
        Testing::FailFatal(expression, file, line, message.AsView());
    }
    Testing::Fail(expression, file, line, message.AsView());
}
} // namespace Private
} // namespace Marlin

/// Defines a test: `MARLIN_TEST(Containers, ListGrows) { ... }`, optionally with tags after the name. MarlinHeaderTool
/// finds it and registers it as Module.Suite.Name; nothing runs at static initialization. The body is
/// Suite_Name_Test::TestBody, the symbol Google Test adapters look up to find a test's source line.
#define MARLIN_TEST(Suite, Name, ...)                                                                                  \
    struct Suite##_##Name##_Test                                                                                       \
    {                                                                                                                  \
        static void TestBody();                                                                                        \
    };                                                                                                                 \
    void MarlinTestRun_##Suite##_##Name()                                                                              \
    {                                                                                                                  \
        Suite##_##Name##_Test::TestBody();                                                                             \
    }                                                                                                                  \
    void Suite##_##Name##_Test::TestBody()

/// Declares a fixture, a TestFixture whose members the tests that use it can reach.
#define MARLIN_TEST_FIXTURE(Fixture) struct Fixture : ::Marlin::TestFixture

/// Defines a test source, which MarlinHeaderTool registers with RegisterTestSource: `MARLIN_TEST_SOURCE(ScriptTests)
/// { ... Testing::RegisterTest(...) ... }`.
#define MARLIN_TEST_SOURCE(Name) void MarlinTestSource_##Name()

/// Defines a test with a fresh *Fixture*: SetUp, the body, then TearDown. A failed MARLIN_REQUIRE skips TearDown.
#define MARLIN_TEST_F(Fixture, Suite, Name, ...)                                                                       \
    struct Suite##_##Name##_Test final : Fixture                                                                       \
    {                                                                                                                  \
        void TestBody();                                                                                               \
    };                                                                                                                 \
    void MarlinTestRun_##Suite##_##Name()                                                                              \
    {                                                                                                                  \
        Suite##_##Name##_Test test;                                                                                    \
        test.SetUp();                                                                                                  \
        test.TestBody();                                                                                               \
        test.TearDown();                                                                                               \
    }                                                                                                                  \
    void Suite##_##Name##_Test::TestBody()

/// Checks *expression* and goes on if it fails.
#define MARLIN_EXPECT(expression)                                                                                      \
    (MARLIN_LIKELY(expression) ? (void)0 : ::Marlin::Testing::Fail(#expression, __FILE__, __LINE__, {}))
/// Checks *expression* and ends the test if it fails.
#define MARLIN_REQUIRE(expression)                                                                                     \
    (MARLIN_LIKELY(expression) ? (void)0 : ::Marlin::Testing::FailFatal(#expression, __FILE__, __LINE__, {}))
/// MARLIN_EXPECT with a Format message: `MARLIN_EXPECTF(a == b, "{0} != {1}", a, b)`.
#define MARLIN_EXPECTF(expression, ...)                                                                                \
    (MARLIN_LIKELY(expression)                                                                                         \
         ? (void)0                                                                                                     \
         : ::Marlin::Private::TestFailFormatted(false, #expression, __FILE__, __LINE__, __VA_ARGS__))
/// MARLIN_REQUIRE with a Format message.
#define MARLIN_REQUIREF(expression, ...)                                                                               \
    (MARLIN_LIKELY(expression)                                                                                         \
         ? (void)0                                                                                                     \
         : ::Marlin::Private::TestFailFormatted(true, #expression, __FILE__, __LINE__, __VA_ARGS__))
