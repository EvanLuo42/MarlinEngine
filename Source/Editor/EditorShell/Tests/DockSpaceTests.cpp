#include "EditorShell/DockSpace.h"
#include "EditorShell/DockingSubsystem.h"
#include "EditorShellTestTypes.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
/// A test context whose DockSpace starts with the toolbar on top and Alpha beside Beta, which has three times the
/// room. No editor UI, so nothing floats and nothing is saved.
struct Docks
{
    Docks()
    {
        SubsystemContext subsystems;
        Docking.Initialize(subsystems);
        Owner = Reactive::CreateOwner(nullptr);
        Reactive::RunInOwner(Owner, [this] {
            DockLayoutState layout;
            DockLayout::AddStack(layout.Main.Top, -1, {DockLayout::Toolbar});
            const int32 row = DockLayout::AddSplit(layout.Main.Body, -1, DockAxis::Row);
            DockLayout::AddStack(layout.Main.Body, row, {"TestAlphaDock"}, 1);
            DockLayout::AddStack(layout.Main.Body, row, {"TestBetaDock"}, 3);
            Context = MakeUnique<TestEditingContext>();
            Space = MakeUnique<DockSpace>(*Context, StringView(), layout, true, &Docking);
        });
    }

    ~Docks()
    {
        Space.Reset();
        Context.Reset();
        Reactive::DisposeOwner(Owner);
        Docking.Deinitialize();
    }

    DockingSubsystem Docking;
    ReactiveOwner *Owner = nullptr;
    UniquePtr<TestEditingContext> Context;
    UniquePtr<DockSpace> Space;
};

bool StackIs(const DockNodeState &node, int32 parent, std::initializer_list<StringView> docks)
{
    if (node.Split || node.Parent != parent || node.Docks.Count() != docks.size())
    {
        return false;
    }
    usize index = 0;
    for (const StringView dock : docks)
    {
        if (node.Docks[index++] != dock)
        {
            return false;
        }
    }
    return true;
}

bool SplitIs(const DockNodeState &node, int32 parent, DockAxis axis)
{
    return node.Split && node.Parent == parent && node.Axis == axis && node.Docks.IsEmpty();
}

bool DockNear(float32 a, float32 b)
{
    return a - b < 0.001f && b - a < 0.001f;
}
} // namespace

MARLIN_TEST(EditorShellDocks, DefaultLayoutOpensItsDocks)
{
    Docks fixture;
    DockSpace &space = *fixture.Space;
    MARLIN_EXPECT(space.IsOpen("Toolbar") && space.IsOpen("TestAlphaDock") && space.IsOpen("TestBetaDock"));
    MARLIN_EXPECT(!space.IsOpen("TestGammaDock"));
    const DockLayoutState layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Top.Count() == 1 && layout.Main.Body.Count() == 3);
    MARLIN_EXPECT(StackIs(layout.Main.Top[0], -1, {"Toolbar"}));
    MARLIN_EXPECT(SplitIs(layout.Main.Body[0], -1, DockAxis::Row));
    MARLIN_EXPECT(StackIs(layout.Main.Body[1], 0, {"TestAlphaDock"}) && DockNear(layout.Main.Body[1].Size, 1));
    MARLIN_EXPECT(StackIs(layout.Main.Body[2], 0, {"TestBetaDock"}) && DockNear(layout.Main.Body[2].Size, 3));
}

MARLIN_TEST(EditorShellDocks, TheToolbarStaysOnTopAndTakesDocksToItsRight)
{
    Docks fixture;
    DockSpace &space = *fixture.Space;
    MARLIN_EXPECT(!space.Close("Toolbar") && !space.MoveDock("Toolbar", "TestAlphaDock", DockZone::Left));
    // Only its right side takes docks.
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", "Toolbar", DockZone::Left));
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", "Toolbar", DockZone::Bottom));
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", "Toolbar", DockZone::Center));
    MARLIN_REQUIRE(space.MoveDock("TestAlphaDock", "Toolbar", DockZone::Right));
    DockLayoutState layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Top.Count() == 3);
    MARLIN_EXPECT(SplitIs(layout.Main.Top[0], -1, DockAxis::Row));
    MARLIN_EXPECT(StackIs(layout.Main.Top[1], 0, {"Toolbar"}) && StackIs(layout.Main.Top[2], 0, {"TestAlphaDock"}));
    // The split Alpha left had only Beta: Beta takes its place.
    MARLIN_REQUIRE(layout.Main.Body.Count() == 1);
    MARLIN_EXPECT(StackIs(layout.Main.Body[0], -1, {"TestBetaDock"}));

    // The top row grows sideways only.
    MARLIN_EXPECT(!space.MoveDock("TestBetaDock", "TestAlphaDock", DockZone::Bottom));
    MARLIN_REQUIRE(space.MoveDock("TestBetaDock", "TestAlphaDock", DockZone::Center));
    layout = space.Capture();
    MARLIN_EXPECT(layout.Main.Top.Count() == 3 && StackIs(layout.Main.Top[2], 0, {"TestAlphaDock", "TestBetaDock"}));
    MARLIN_EXPECT(layout.Main.Body.IsEmpty());

    // Moving them all out of the row leaves the toolbar alone again.
    MARLIN_REQUIRE(space.MoveDock("TestAlphaDock", StringView(), DockZone::Center));
    MARLIN_REQUIRE(space.MoveDock("TestBetaDock", StringView(), DockZone::Bottom));
    layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Top.Count() == 1 && layout.Main.Body.Count() == 3);
    MARLIN_EXPECT(StackIs(layout.Main.Top[0], -1, {"Toolbar"}));
    MARLIN_EXPECT(SplitIs(layout.Main.Body[0], -1, DockAxis::Column));
    MARLIN_EXPECT(StackIs(layout.Main.Body[1], 0, {"TestAlphaDock"}) &&
                  StackIs(layout.Main.Body[2], 0, {"TestBetaDock"}));
}

MARLIN_TEST(EditorShellDocks, SplittingJoinsSplitsThatRunTheSameWay)
{
    Docks fixture;
    DockSpace &space = *fixture.Space;
    MARLIN_REQUIRE(space.MoveDock("TestAlphaDock", "TestBetaDock", DockZone::Bottom));
    DockLayoutState layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Body.Count() == 3);
    MARLIN_EXPECT(SplitIs(layout.Main.Body[0], -1, DockAxis::Column));
    MARLIN_EXPECT(StackIs(layout.Main.Body[1], 0, {"TestBetaDock"}) &&
                  StackIs(layout.Main.Body[2], 0, {"TestAlphaDock"}));

    // A dock at the left edge of a column's stack makes a row inside the column.
    const DockType *gamma = fixture.Docking.FindType("TestGammaDock");
    MARLIN_REQUIRE(gamma != nullptr && space.Open(*gamma));
    MARLIN_REQUIRE(space.MoveDock("TestGammaDock", "TestBetaDock", DockZone::Left));
    layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Body.Count() == 5);
    MARLIN_EXPECT(SplitIs(layout.Main.Body[0], -1, DockAxis::Column));
    MARLIN_EXPECT(SplitIs(layout.Main.Body[1], 0, DockAxis::Row));
    MARLIN_EXPECT(StackIs(layout.Main.Body[2], 1, {"TestGammaDock"}) &&
                  StackIs(layout.Main.Body[3], 1, {"TestBetaDock"}));
    MARLIN_EXPECT(StackIs(layout.Main.Body[4], 0, {"TestAlphaDock"}));

    // Closing Alpha leaves that row alone in the column, so the row takes the column's place.
    MARLIN_REQUIRE(space.Close("TestAlphaDock"));
    layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Body.Count() == 3);
    MARLIN_EXPECT(SplitIs(layout.Main.Body[0], -1, DockAxis::Row));
    MARLIN_EXPECT(StackIs(layout.Main.Body[1], 0, {"TestGammaDock"}) &&
                  StackIs(layout.Main.Body[2], 0, {"TestBetaDock"}));
    MARLIN_EXPECT(!space.IsOpen("TestAlphaDock"));
}

MARLIN_TEST(EditorShellDocks, MovesThatChangeNothingAreRefused)
{
    Docks fixture;
    DockSpace &space = *fixture.Space;
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", "TestAlphaDock", DockZone::Center));
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", "TestAlphaDock", DockZone::Left));
    MARLIN_EXPECT(!space.MoveDock("TestAlphaDock", StringView(), DockZone::Center));
    MARLIN_EXPECT(!space.MoveDock("TestGammaDock", "TestAlphaDock", DockZone::Center));
    MARLIN_REQUIRE(space.MoveDock("TestBetaDock", "TestAlphaDock", DockZone::Center));
    // With two docks in its stack, a dock can be split off its own stack.
    MARLIN_EXPECT(space.MoveDock("TestBetaDock", "TestAlphaDock", DockZone::Right));
    const DockLayoutState layout = space.Capture();
    MARLIN_REQUIRE(layout.Main.Body.Count() == 3);
    MARLIN_EXPECT(StackIs(layout.Main.Body[1], 0, {"TestAlphaDock"}) &&
                  StackIs(layout.Main.Body[2], 0, {"TestBetaDock"}));
}

MARLIN_TEST(EditorShellDocks, CapturedLayoutsRestore)
{
    Docks fixture;
    DockSpace &space = *fixture.Space;
    MARLIN_REQUIRE(space.MoveDock("TestBetaDock", "Toolbar", DockZone::Right));
    const DockLayoutState saved = space.Capture();
    space.ResetLayout();
    MARLIN_EXPECT(space.Capture().Main.Top.Count() == 1);
    MARLIN_REQUIRE(space.Restore(saved));
    const DockLayoutState restored = space.Capture();
    MARLIN_REQUIRE(restored.Main.Top.Count() == 3 && restored.Main.Body.Count() == 1);
    MARLIN_EXPECT(StackIs(restored.Main.Top[2], 0, {"TestBetaDock"}));
    MARLIN_EXPECT(StackIs(restored.Main.Body[0], -1, {"TestAlphaDock"}));

    // Unknown docks are left out; a layout without the toolbar on top is refused for the default.
    DockLayoutState unknown;
    DockLayout::AddStack(unknown.Main.Top, -1, {DockLayout::Toolbar});
    DockLayout::AddStack(unknown.Main.Body, -1, {"NoSuchDock", "TestGammaDock"});
    MARLIN_REQUIRE(space.Restore(unknown));
    MARLIN_EXPECT(space.IsOpen("TestGammaDock") && !space.IsOpen("TestAlphaDock"));
    DockLayoutState headless;
    DockLayout::AddStack(headless.Main.Body, -1, {"TestGammaDock"});
    MARLIN_EXPECT(!space.Restore(headless));
    MARLIN_EXPECT(space.IsOpen("TestAlphaDock") && space.IsOpen("TestBetaDock") && !space.IsOpen("TestGammaDock"));
}
} // namespace Marlin::Tests
