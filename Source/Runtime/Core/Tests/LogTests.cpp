#include "Concurrency/Jobs.h"
#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Log.h"
#include "Diagnostics/Profiler.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
MARLIN_DECLARE_LOG_CATEGORY(, LogCheck, Info, Info);
MARLIN_DEFINE_LOG_CATEGORY(LogCheck);

namespace
{
class CaptureSink final : public ILogSink
{
public:
    void Write(const LogRecord &record) override
    {
        ++Count;
        Last = record.Message;
        LastLevel = record.Level;
        Ordered &= record.Ticks >= LastTicks || record.ThreadId != LastThread;
        LastTicks = record.Ticks;
        LastThread = record.ThreadId;
        Bytes += record.Message.Length();
    }

    uint32 Count = 0;
    String Last;
    LogLevel LastLevel = LogLevel::Off;
    bool Ordered = true;
    uint64 LastTicks = 0;
    uint32 LastThread = 0;
    uint64 Bytes = 0;
};

Atomic<uint32> _zonesBegun{0};
Atomic<uint32> _zonesEnded{0};
Atomic<uint32> _fiberSwitches{0};

void LogFromJob(void *argument)
{
    const auto index = static_cast<uint32>(reinterpret_cast<uintptr>(argument));
    MARLIN_PROFILE_SCOPE("LogFromJob");
    for (uint32 i = 0; i < 200; ++i)
    {
        MARLIN_LOG(LogCheck, Info, "job {0} line {1} with some padding text to fill the ring faster", index, i);
    }
}

uint32 _evaluated = 0;

[[maybe_unused]] uint32 CountEvaluation()
{
    return ++_evaluated;
}
} // namespace

MARLIN_TEST(Log, SinksAndJobs)
{
    CaptureSink sink;
    Log::EnableDebugOutput(false);
    // Messages logged before the sink exists, such as the startup line, are not this test's.
    Log::Flush();
    Log::AddSink(&sink);

    MARLIN_LOG(LogCheck, Info, "hello {0}", 42);
    MARLIN_LOG(LogCheck, Debug, "hidden {0}", CountEvaluation());
    Log::Flush();
    MARLIN_EXPECT(sink.Count == 1 && sink.Last == "hello 42" && sink.LastLevel == LogLevel::Info);
    MARLIN_EXPECT(_evaluated == 0);

    LogCheck.SetLevel(LogLevel::Trace);
    MARLIN_LOG(LogCheck, Debug, "shown {0}", CountEvaluation());
    Log::Flush();
#if MARLIN_BUILD_SHIPPING
    MARLIN_EXPECT(sink.Count == 1 && _evaluated == 0);
#else
    MARLIN_EXPECT(sink.Count == 2 && sink.Last == "shown 1");
#endif
    LogCheck.SetLevel(LogLevel::Info);

    MARLIN_LOG(LogCheck, Error, "flushed at once");
    MARLIN_EXPECT(sink.Last == "flushed at once");

    ProfilerHooks hooks;
    hooks.BeginZone = [](const ProfileZone *) { _zonesBegun.FetchAdd(1); };
    hooks.EndZone = [](const ProfileZone *) { _zonesEnded.FetchAdd(1); };
    hooks.FiberSwitch = [](const void *, const char *) { _fiberSwitches.FetchAdd(1); };
    Profiler::SetHooks(&hooks);

    const uint32 before = sink.Count;
    JobSystem::Start();
    JobDecl decls[64];
    for (uint32 i = 0; i < 64; ++i)
    {
        decls[i] = {LogFromJob, reinterpret_cast<void *>(static_cast<uintptr>(i))};
    }
    JobCounter counter;
    Jobs::Run(Span<const JobDecl>(decls), &counter);
    Jobs::Wait(counter);
    JobSystem::Stop();
    Log::Flush();
    MARLIN_EXPECT(sink.Count - before == 64 * 200);
    MARLIN_EXPECT(sink.Ordered);
    MARLIN_EXPECT(sink.Bytes + static_cast<usize>(sink.Count) * 56 > (usize{1} << 20));
#if MARLIN_PROFILING
    MARLIN_EXPECT(_zonesBegun.Load() == 64 && _zonesEnded.Load() == 64 && _fiberSwitches.Load() > 0);
#endif
    Profiler::SetHooks(nullptr);

    StringBuilder<128> line;
    const LogRecord record{0, 7, &LogCheck, "File.cpp", "text", 3, 10, LogLevel::Warning};
    Log::FormatRecord(line, record);
    MARLIN_EXPECT(line.AsView().EndsWith("][frame 7][thread 3] LogCheck: Warning: text"));
    MARLIN_EXPECT(line.AsView()[0] == '[' && line.AsView()[3] == ':' && line.AsView()[9] == '.');

    MARLIN_ASSERTF(sink.Count > 0, "count was {0}", sink.Count);
    Log::RemoveSink(&sink);
    Log::EnableDebugOutput(true);
}

MARLIN_TEST(Log, CategoriesAreFoundByName)
{
    // Core's own category and the one this module defines both come from their modules' sections.
    MARLIN_REQUIRE(Log::FindCategory("logcore") == &LogCore);
    MARLIN_REQUIRE(Log::FindCategory("LogCheck") == &LogCheck);
    MARLIN_EXPECT(Log::FindCategory("LogNoSuchCategory") == nullptr);

    LogLevel level = LogLevel::Info;
    MARLIN_EXPECT(Log::ParseLevel("trace", level) && level == LogLevel::Trace);
    MARLIN_EXPECT(!Log::ParseLevel("Verbose", level));

    const LogLevel before = LogCheck.GetLevel();
    StringBuilder<256> errors;
    MARLIN_EXPECT(Log::SetLevels("LogCheck:Debug", errors) && LogCheck.GetLevel() == LogLevel::Debug);
    MARLIN_EXPECT(Log::SetLevels(" logcheck : Off , ", errors) && LogCheck.GetLevel() == LogLevel::Off);
    MARLIN_EXPECT(errors.IsEmpty());

    // The valid entry still applies when another one is wrong.
    MARLIN_EXPECT(!Log::SetLevels("LogCheck:Trace,LogNoSuchCategory:Debug,LogCheck", errors));
    MARLIN_EXPECT(LogCheck.GetLevel() == LogLevel::Trace);
    MARLIN_EXPECT(errors.AsView().Contains("LogNoSuchCategory") && errors.AsView().Contains("expected Category:Level"));
    LogCheck.SetLevel(before);

    uint32 seen = 0;
    Log::ForEachCategory([&seen](LogCategory &category) { seen += &category == &LogCore || &category == &LogCheck; });
    MARLIN_EXPECT(seen == 2);
}
} // namespace Marlin::Tests
