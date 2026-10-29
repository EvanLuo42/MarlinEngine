#include "Testing/Testing.h"

#include "Containers/List.h"
#include "Diagnostics/AssertFormat.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformFiber.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "String/Number.h"

namespace Marlin
{
namespace
{
/// Test bodies may run jobs inline while they wait, so they get a large stack.
constexpr usize TestStackSize = usize{4} << 20;

constexpr StringView CurrentPlatform = MARLIN_PLATFORM_WINDOWS ? "Windows"
                                       : MARLIN_PLATFORM_LINUX ? "Linux"
                                       : MARLIN_PLATFORM_MACOS ? "macOS"
                                       : MARLIN_PLATFORM_PS5   ? "PS5"
                                       : MARLIN_PLATFORM_XBOX  ? "Xbox"
                                                               : "Switch2";

/// Tags that keep a test out of a default run.
constexpr StringView OptInTags[] = {"Slow", "Gpu", "Benchmark"};

struct TestFailure
{
    String Location;
    String Message;
};

struct TestResult
{
    const TestInfo *Test = nullptr;
    List<TestFailure> Failures;
    float64 Seconds = 0;
};

struct RunnerState
{
    List<TestInfo> Tests;
    List<void (*)()> Sources;
    TestResult *Current = nullptr;
    const TestInfo *Running = nullptr;
    FiberHandle Runner;
    bool Finished = false;

    /// Built on first use and never destroyed.
    static RunnerState &Get()
    {
        alignas(RunnerState) static uint8 storage[sizeof(RunnerState)];
        static RunnerState *state = ConstructAt(reinterpret_cast<RunnerState *>(storage));
        return *state;
    }
};

using Testing::Print;

struct Options
{
    List<StringView> Include;
    List<StringView> Exclude;
    List<StringView> Tags;
    StringView JUnit;
    bool List = false;
    bool All = false;
    /// A --gtest_ option was given: names are Suite.Name, the convention IDE test adapters expect.
    bool GoogleTest = false;
};

void AppendName(StringWriter &output, const TestInfo &test, const Options &options)
{
    if (!options.GoogleTest)
    {
        output.Append(test.Module).Append('.');
    }
    output.Append(test.Suite).Append('.').Append(test.Name);
}

/// `--gtest_filter=A.*:B.C-D.E`: patterns separated by ':', those after the first '-' excluding.
void ParseGoogleTestFilter(StringView filter, Options &options)
{
    StringView include = filter;
    StringView exclude;
    if (!filter.SplitFirst('-', include, exclude))
    {
        include = filter;
    }
    for (const StringView pattern : include.Split(':'))
    {
        if (!pattern.IsEmpty() && pattern != "*")
        {
            options.Include.Add(pattern);
        }
    }
    for (const StringView pattern : exclude.Split(':'))
    {
        if (!pattern.IsEmpty())
        {
            options.Exclude.Add(pattern);
        }
    }
}

bool ParseOptions(Span<const StringView> arguments, Options &options)
{
    for (const StringView argument : arguments)
    {
        if (argument == "--list")
        {
            options.List = true;
        }
        else if (argument == "--all")
        {
            options.All = true;
        }
        else if (argument.StartsWith("--tags="))
        {
            for (const StringView tag : argument.RemoveStart(7).Split(','))
            {
                options.Tags.Add(tag.Trim());
            }
        }
        else if (argument.StartsWith("--junit="))
        {
            options.JUnit = argument.RemoveStart(8);
        }
        else if (argument.StartsWith("--gtest_"))
        {
            options.GoogleTest = true;
            if (argument == "--gtest_list_tests")
            {
                options.List = true;
            }
            else if (argument.StartsWith("--gtest_filter="))
            {
                ParseGoogleTestFilter(argument.RemoveStart(15), options);
            }
            else if (argument.StartsWith("--gtest_output=xml"))
            {
                const StringView path = argument.RemoveStart(18);
                options.JUnit = path.StartsWith(':') ? path.RemoveStart(1) : StringView("test_detail.xml");
            }
        }
        else if (argument.StartsWith("--"))
        {
            StringBuilder<256> message("unknown option ");
            message.Append(argument).Append('\n');
            Print(message.AsView());
            return false;
        }
        else if (argument.StartsWith('-'))
        {
            options.Exclude.Add(argument.RemoveStart(1));
        }
        else
        {
            options.Include.Add(argument);
        }
    }
    return true;
}

bool Matches(StringView name, StringView filter)
{
    if (name.MatchesWildcard(filter))
    {
        return true;
    }
    StringBuilder<256> below(filter);
    below.Append(".*");
    return name.MatchesWildcard(below.AsView());
}

/// Whether *test* runs: it passes the filters, its platform tag names this platform, and it was named exactly or an
/// opt-in tag of it was asked for.
bool IsSelected(const TestInfo &test, const Options &options)
{
    StringBuilder<256> name;
    AppendName(name, test, options);
    const auto matches = [&](const List<StringView> &filters) {
        for (const StringView filter : filters)
        {
            if (Matches(name.AsView(), filter))
            {
                return true;
            }
        }
        return false;
    };
    if ((!options.Include.IsEmpty() && !matches(options.Include)) || matches(options.Exclude))
    {
        return false;
    }
    for (const StringView tag : test.Tags.Split(','))
    {
        const StringView trimmed = tag.Trim();
        if (trimmed.StartsWith("Platform(") && trimmed.EndsWith(')') &&
            trimmed.Substring(9, trimmed.Length() - 10).Trim() != CurrentPlatform)
        {
            return false;
        }
    }
    if (options.All || options.Include.Contains(name.AsView()))
    {
        return true;
    }
    for (const StringView tag : OptInTags)
    {
        if (test.HasTag(tag) && !options.Tags.Contains(tag))
        {
            return false;
        }
    }
    return true;
}

void TestEntry(void * /*argument*/)
{
    RunnerState &state = RunnerState::Get();
    if (state.Running->Run != nullptr)
    {
        state.Running->Run();
    }
    else
    {
        state.Running->RunWith(state.Running->Context);
    }
    state.Finished = true;
    PlatformFiber::SwitchTo(state.Runner);
    MARLIN_UNREACHABLE();
}

void RunOne(const TestInfo &test, TestResult &result)
{
    RunnerState &state = RunnerState::Get();
    result.Test = &test;
    state.Current = &result;
    state.Finished = false;
    state.Running = &test;
    const float64 start = PlatformTime::Seconds();
    const FiberHandle fiber = PlatformFiber::Create(TestStackSize, &TestEntry, nullptr);
    if (!fiber.IsValid())
    {
        MARLIN_FATAL("Cannot create a fiber for a test");
    }
    PlatformFiber::SwitchTo(fiber);
    PlatformFiber::Delete(fiber);
    result.Seconds = PlatformTime::Seconds() - start;
    state.Current = nullptr;
}

void AppendEscaped(StringWriter &output, StringView text)
{
    for (const char c : text)
    {
        switch (c)
        {
        case '<':
            output.Append("&lt;");
            break;
        case '>':
            output.Append("&gt;");
            break;
        case '&':
            output.Append("&amp;");
            break;
        case '"':
            output.Append("&quot;");
            break;
        default:
            output.Append(c);
            break;
        }
    }
}

void AppendSeconds(StringWriter &output, float64 seconds)
{
    AppendFloat(output, seconds, FloatStyle::Fixed, 6);
}

void AppendSuite(StringWriter &output, const TestInfo &test, const Options &options)
{
    if (!options.GoogleTest)
    {
        output.Append(test.Module).Append('.');
    }
    output.Append(test.Suite);
}

/// JUnit XML in the shape Google Test writes, one testsuite per suite; *results* are sorted by suite.
bool WriteJUnit(StringView path, Span<const TestResult> results, float64 seconds, const Options &options)
{
    usize failed = 0;
    for (const TestResult &result : results)
    {
        failed += result.Failures.IsEmpty() ? 0 : 1;
    }
    StringBuilder<4096> xml("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites name=\"MarlinTests\" tests=\"");
    char digits[24];
    xml.Append(StringView(digits, FormatUnsigned(results.Count(), digits))).Append("\" failures=\"");
    xml.Append(StringView(digits, FormatUnsigned(failed, digits))).Append("\" time=\"");
    AppendSeconds(xml, seconds);
    xml.Append("\">\n");
    for (usize i = 0; i < results.Count(); ++i)
    {
        const TestResult &result = results[i];
        const TestInfo &test = *result.Test;
        StringBuilder<128> suite;
        AppendSuite(suite, test, options);
        StringBuilder<128> previous;
        if (i > 0)
        {
            AppendSuite(previous, *results[i - 1].Test, options);
        }
        if (i == 0 || previous.AsView() != suite.AsView())
        {
            xml.Append(i == 0 ? "" : "  </testsuite>\n").Append("  <testsuite name=\"").Append(suite.AsView());
            xml.Append("\">\n");
        }
        xml.Append("    <testcase classname=\"").Append(suite.AsView());
        xml.Append("\" name=\"").Append(test.Name).Append(R"(" status="run" time=")");
        AppendSeconds(xml, result.Seconds);
        xml.Append(result.Failures.IsEmpty() ? "\"/>\n" : "\">\n");
        for (const TestFailure &failure : result.Failures)
        {
            xml.Append("      <failure message=\"");
            AppendEscaped(xml, failure.Message.AsView());
            xml.Append("\">");
            AppendEscaped(xml, failure.Location.AsView());
            xml.Append("</failure>\n");
        }
        if (!result.Failures.IsEmpty())
        {
            xml.Append("    </testcase>\n");
        }
    }
    xml.Append(results.IsEmpty() ? "" : "  </testsuite>\n").Append("</testsuites>\n");

    const String terminated(path);
    const FileHandle file = PlatformFile::Open(terminated.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const bool written = PlatformFile::Write(file, xml.Data(), xml.Length());
    PlatformFile::Close(file);
    return written;
}

void Record(const char *expression, const char *file, int32 line, StringView message)
{
    const RunnerState &state = RunnerState::Get();
    if (state.Current == nullptr)
    {
        MARLIN_FATALF("Test check failed outside a test: {0}", expression);
    }
    StringBuilder<512> location(file);
    char digits[16];
    location.Append('(').Append(StringView(digits, FormatSigned(line, digits))).Append("): error: ");
    location.Append(expression);
    StringBuilder<512> text(location.AsView());
    if (!message.IsEmpty())
    {
        text.Append(": ").Append(message);
    }
    text.Append('\n');
    Print(text.AsView());
    state.Current->Failures.Add({location.ToString(), message.IsEmpty() ? String(expression) : String(message)});
    if (PlatformDebug::IsDebuggerAttached())
    {
        MARLIN_DEBUG_BREAK();
    }
}
} // namespace

bool TestInfo::HasTag(StringView tag) const
{
    for (const StringView part : Tags.Split(','))
    {
        if (part.Trim() == tag)
        {
            return true;
        }
    }
    return false;
}

namespace Testing
{
void RegisterTest(const TestInfo &test)
{
    RunnerState::Get().Tests.Add(test);
}

void RegisterTestSource(void (*source)())
{
    RunnerState::Get().Sources.Add(source);
}

void Print(StringView text)
{
    PlatformProcess::WriteOutput(text.Data(), text.Length());
}

void Fail(const char *expression, const char *file, int32 line, StringView message)
{
    Record(expression, file, line, message);
}

void FailFatal(const char *expression, const char *file, int32 line, StringView message)
{
    Record(expression, file, line, message);
    PlatformFiber::SwitchTo(RunnerState::Get().Runner);
    MARLIN_UNREACHABLE();
}

int32 RunTests(Span<const StringView> arguments)
{
    RunnerState &state = RunnerState::Get();
    Options options;
    if (!ParseOptions(arguments, options))
    {
        return 2;
    }
    for (void (*source)() : state.Sources)
    {
        source();
    }
    state.Sources.Clear();
    // Google Test names leave the module out, so they sort and must be unique without it.
    const bool byModule = !options.GoogleTest;
    state.Tests.Sort([byModule](const TestInfo &a, const TestInfo &b) {
        if (byModule && a.Module != b.Module)
        {
            return a.Module.CompareTo(b.Module) < 0;
        }
        if (a.Suite != b.Suite)
        {
            return a.Suite.CompareTo(b.Suite) < 0;
        }
        return a.Name != b.Name ? a.Name.CompareTo(b.Name) < 0 : a.Module.CompareTo(b.Module) < 0;
    });
    for (usize i = 1; i < state.Tests.Count(); ++i)
    {
        const TestInfo &a = state.Tests[i - 1];
        const TestInfo &b = state.Tests[i];
        if (a.Suite == b.Suite && a.Name == b.Name && (options.GoogleTest || a.Module == b.Module))
        {
            StringBuilder<256> message("test ");
            message.Append(a.Suite).Append('.').Append(a.Name).Append(" is defined by both ").Append(a.Module);
            message.Append(" and ").Append(b.Module).Append("; suite names must be unique across modules\n");
            Print(message.AsView());
            return 2;
        }
    }

    List<const TestInfo *> selected;
    for (const TestInfo &test : state.Tests)
    {
        if (IsSelected(test, options))
        {
            selected.Add(&test);
        }
    }
    if (options.List)
    {
        for (usize i = 0; i < selected.Count(); ++i)
        {
            const TestInfo &test = *selected[i];
            StringBuilder<256> line;
            if (options.GoogleTest)
            {
                // Google Test's listing: "Suite." then each test indented under it.
                if (i == 0 || selected[i - 1]->Suite != test.Suite)
                {
                    line.Append(test.Suite).Append(".\n");
                }
                line.Append("  ").Append(test.Name).Append('\n');
            }
            else
            {
                AppendName(line, test, options);
                line.Append(test.Tags.IsEmpty() ? "" : "  [").Append(test.Tags).Append(test.Tags.IsEmpty() ? "" : "]");
                line.Append('\n');
            }
            Print(line.AsView());
        }
        return 0;
    }

    const bool converted = !PlatformFiber::Current().IsValid();
    state.Runner = converted ? PlatformFiber::ConvertCurrentThread() : PlatformFiber::Current();
    List<TestResult> results;
    results.Resize(selected.Count());
    const float64 start = PlatformTime::Seconds();
    usize failed = 0;
    for (usize i = 0; i < selected.Count(); ++i)
    {
        StringBuilder<256> line("[ RUN      ] ");
        AppendName(line, *selected[i], options);
        line.Append('\n');
        Print(line.AsView());
        RunOne(*selected[i], results[i]);
        line.Clear();
        line.Append(results[i].Failures.IsEmpty() ? "[       OK ] " : "[  FAILED  ] ");
        AppendName(line, *selected[i], options);
        line.Append(" (");
        // Google Test prints whole milliseconds, and adapters parse nothing else.
        AppendFloat(line, results[i].Seconds * 1000, FloatStyle::Fixed, options.GoogleTest ? 0 : 2);
        line.Append(" ms)\n");
        Print(line.AsView());
        failed += results[i].Failures.IsEmpty() ? 0 : 1;
    }
    const float64 seconds = PlatformTime::Seconds() - start;
    if (converted)
    {
        PlatformFiber::RevertCurrentThread();
    }

    StringBuilder<1024> summary;
    char digits[24];
    summary.Append(StringView(digits, FormatUnsigned(selected.Count(), digits))).Append(" tests, ");
    summary.Append(StringView(digits, FormatUnsigned(failed, digits))).Append(" failed, ");
    summary.Append(StringView(digits, FormatUnsigned(state.Tests.Count() - selected.Count(), digits)));
    summary.Append(" not selected (");
    AppendFloat(summary, seconds, FloatStyle::Fixed, 2);
    summary.Append(" s)\n");
    for (const TestResult &result : results)
    {
        if (!result.Failures.IsEmpty())
        {
            summary.Append("[  FAILED  ] ");
            AppendName(summary, *result.Test, options);
            summary.Append('\n');
        }
    }
    Print(summary.AsView());
    if (!options.JUnit.IsEmpty() && !WriteJUnit(options.JUnit, results.AsSpan(), seconds, options))
    {
        Print("cannot write the JUnit report\n");
        return 1;
    }
    return failed == 0 ? 0 : 1;
}
} // namespace Testing
} // namespace Marlin
