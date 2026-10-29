#include "EditorShell/EditingContextSubsystem.h"
#include "EditorShellTestTypes.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
const ToolContextType *FindTool(const EditingContextSubsystem &contexts, StringView name)
{
    for (const ToolContextType *type : contexts.GetToolTypes())
    {
        if (type->GetDisplayName() == name)
        {
            return type;
        }
    }
    return nullptr;
}

Guid AssetGuid(uint32 index)
{
    Guid guid;
    guid.A = 0x2a3c1e9f;
    guid.D = index;
    return guid;
}

/// An initialized subsystem, without menus.
struct Contexts
{
    Contexts()
    {
        SubsystemContext context;
        Subsystem.Initialize(context);
    }
    ~Contexts()
    {
        Subsystem.Deinitialize();
    }

    EditingContextSubsystem Subsystem;
};
} // namespace

MARLIN_TEST(EditorShellContexts, PinnedContextsOpenFirstAndStay)
{
    Contexts fixture;
    EditingContextSubsystem &contexts = fixture.Subsystem;
    // The test's pinned type, and any other linked one such as the Asset Manager's.
    const usize pinned = contexts.GetContexts().Count();
    MARLIN_REQUIRE(pinned >= 1);
    bool found = false;
    for (EditingContext *context : contexts.GetContexts())
    {
        MARLIN_EXPECT(context->IsPinned());
        found = found || context->Title.Peek() == "Test Pinned";
    }
    MARLIN_EXPECT(found && contexts.GetActive() == contexts.GetContexts()[0]);
    MARLIN_EXPECT(!contexts.Close(contexts.GetContexts()[0]) && contexts.GetContexts().Count() == pinned);
}

MARLIN_TEST(EditorShellContexts, OpeningTwiceActivatesTheOpenContext)
{
    Contexts fixture;
    EditingContextSubsystem &contexts = fixture.Subsystem;
    const usize pinned = contexts.GetContexts().Count();
    const ToolContextType *testTool = FindTool(contexts, "Test Tool");
    MARLIN_REQUIRE(testTool != nullptr);
    EditingContext *tool = contexts.OpenTool(*testTool);
    EditingContext *asset = contexts.OpenAsset(AssetGuid(1), {}, "Music Project");
    MARLIN_REQUIRE(tool != nullptr && asset != nullptr);
    MARLIN_EXPECT(contexts.GetActive() == asset && asset->Title.Peek() == "Music Project");
    MARLIN_EXPECT(contexts.OpenTool(*testTool) == tool && contexts.GetActive() == tool);
    MARLIN_EXPECT(contexts.OpenAsset(AssetGuid(1), {}, "Other") == asset && contexts.GetActive() == asset);
    MARLIN_EXPECT(contexts.GetContexts().Count() == pinned + 2);
}

MARLIN_TEST(EditorShellContexts, ReorderNeverPassesThePinnedContexts)
{
    Contexts fixture;
    EditingContextSubsystem &contexts = fixture.Subsystem;
    const usize pinned = contexts.GetContexts().Count();
    EditingContext *a = contexts.OpenAsset(AssetGuid(1), {}, "A");
    EditingContext *b = contexts.OpenAsset(AssetGuid(2), {}, "B");
    EditingContext *c = contexts.OpenAsset(AssetGuid(3), {}, "C");
    contexts.Reorder(c, 0);
    MARLIN_EXPECT(contexts.GetContexts()[0]->IsPinned() && contexts.GetContexts()[pinned] == c);
    contexts.Reorder(c, 99);
    MARLIN_EXPECT(contexts.GetContexts()[pinned] == a && contexts.GetContexts()[pinned + 1] == b &&
                  contexts.GetContexts()[pinned + 2] == c);
    contexts.Reorder(contexts.GetContexts()[0], 3);
    MARLIN_EXPECT(contexts.GetContexts()[0]->IsPinned());
}

MARLIN_TEST(EditorShellContexts, ClosingTheActiveTabActivatesItsNeighbour)
{
    Contexts fixture;
    EditingContextSubsystem &contexts = fixture.Subsystem;
    const usize pinned = contexts.GetContexts().Count();
    EditingContext *a = contexts.OpenAsset(AssetGuid(1), {}, "A");
    EditingContext *b = contexts.OpenAsset(AssetGuid(2), {}, "B");
    EditingContext *c = contexts.OpenAsset(AssetGuid(3), {}, "C");
    contexts.Activate(b);
    MARLIN_EXPECT(contexts.Close(b) && contexts.GetActive() == c);
    MARLIN_EXPECT(contexts.Close(c) && contexts.GetActive() == a);
    contexts.Activate(contexts.GetContexts()[0]);
    MARLIN_EXPECT(contexts.Close(a) && contexts.GetActive() == contexts.GetContexts()[0]);
    MARLIN_EXPECT(contexts.GetContexts().Count() == pinned);
}
} // namespace Marlin::Tests
