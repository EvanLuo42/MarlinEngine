#include "Testing/Testing.h"

#include "Engine/Engine.h"
#include "Engine/FramePacer.h"
#include "EngineTestTypes.h"
#include "HAL/PlatformTime.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
List<String> TestScopeSubsystem::Events;

void TestScopeSubsystem::Initialize(SubsystemContext &context)
{
    MARLIN_EXPECT(context.Collection != nullptr && context.Collection->Find(GetType()) == this);
    Events.Add(String("+") + GetType().GetName());
}

void TestScopeSubsystem::Deinitialize()
{
    Events.Add(String("-") + GetType().GetName());
}

bool TestDisabledSubsystem::ShouldCreate(const SubsystemContext & /*context*/)
{
    return false;
}

MARLIN_TEST(Engine, SubsystemsFollowDependencies)
{
    TestScopeSubsystem::Events.Clear();
    {
        SubsystemCollection collection(TypeOf<TestScopeSubsystem>());
        collection.Initialize();
        MARLIN_REQUIRE(collection.GetAll().Count() == 3);
        MARLIN_EXPECT(collection.Get<TestStorageSubsystem>() != nullptr);
        MARLIN_EXPECT(collection.Get<TestRendererSubsystem>() != nullptr);
        MARLIN_EXPECT(collection.Get<TestAudioSubsystem>() != nullptr);
        MARLIN_EXPECT(collection.Get<TestDisabledSubsystem>() == nullptr);
        MARLIN_EXPECT(collection.Get<TestNeedsDisabledSubsystem>() == nullptr);
        MARLIN_EXPECT(collection.Get<TestScopeSubsystem>() == collection.GetAll()[0]);
    }
    const List<String> &events = TestScopeSubsystem::Events;
    MARLIN_REQUIRE(events.Count() == 6);
    MARLIN_EXPECT(events[0] == "+TestStorageSubsystem" && events[1] == "+TestRendererSubsystem" &&
                  events[2] == "+TestAudioSubsystem");
    MARLIN_EXPECT(events[3] == "-TestAudioSubsystem" && events[4] == "-TestRendererSubsystem" &&
                  events[5] == "-TestStorageSubsystem");
}

MARLIN_TEST(Engine, Environment)
{
    const EngineEnvironment &environment = Engine::GetEnvironment();
    MARLIN_EXPECT(environment.TargetName == "MarlinTests" && environment.Form == EngineForm::Source);
    MARLIN_EXPECT(!environment.EngineRoot.IsEmpty() && environment.EngineId.StartsWith("Source-"));
    MARLIN_EXPECT(environment.EngineId == Engine::ComputeSourceEngineId(environment.EngineRoot));
    MARLIN_EXPECT(Vfs::IsMounted("/Engine") && Vfs::IsMounted("/Temp") && Vfs::IsMounted("/User/Global"));
    MARLIN_EXPECT(Vfs::Exists("/Engine/Config/Engine.mtext"));
}

MARLIN_TEST(Engine, SourceEngineIdMatchesTheToolkit)
{
    // marlin generate registered this root under this Id; the algorithm must not drift.
    MARLIN_EXPECT(Engine::ComputeSourceEngineId("C:/Users/ziyun/Projects/MarlinEngine") == "Source-bad89a4e");
    MARLIN_EXPECT(Engine::ComputeSourceEngineId("C:\\Users\\ziyun\\Projects\\MarlinEngine\\") == "Source-bad89a4e");
    if (MARLIN_PLATFORM_WINDOWS)
    {
        MARLIN_EXPECT(Engine::ComputeSourceEngineId("c:/users/ZIYUN/projects/marlinengine") == "Source-bad89a4e");
    }
}

MARLIN_TEST(Engine, FramePacerKeepsAnAbsoluteGrid)
{
    // On time or a little late: the next deadline stays on the grid, so a late frame is made up by the next one.
    MARLIN_EXPECT(FramePacer::NextDeadline(1.0, 0.25, 1.1) == 1.25);
    MARLIN_EXPECT(FramePacer::NextDeadline(1.0, 0.25, 1.4) == 1.25);
    // More than a whole interval behind: the grid restarts at now instead of running frames back to back.
    MARLIN_EXPECT(FramePacer::NextDeadline(1.0, 0.25, 1.6) == 1.6);

    // Sleeping holds the frame rate within a millisecond or two over many frames, not the system tick.
    FramePacer pacer;
    pacer.SetInterval(1.0 / 250);
    const float64 start = PlatformTime::Seconds();
    pacer.Reset(start);
    for (uint32 frame = 0; frame < 50; ++frame)
    {
        pacer.Wait();
    }
    const float64 elapsed = PlatformTime::Seconds() - start;
    MARLIN_EXPECT(elapsed >= 0.2 - 1e-6 && elapsed < 0.24);
}
} // namespace Marlin::Tests
