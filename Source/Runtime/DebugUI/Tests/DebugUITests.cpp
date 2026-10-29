#include "Testing/Testing.h"

#include "DebugUI/DebugOverlay.h"
#include "Reflection/ExtensionSet.h"

// RECOVERY-DECISION D5: the outline indexes no DebugUI tests; these were added during recovery.
namespace Marlin::Tests
{
MARLIN_TEST(DebugUI, OverlaySectionsInPriorityOrder)
{
    ExtensionSet<DebugOverlaySection> sections;
    sections.Build();
    List<StringView> titles;
    for (DebugOverlaySection *section : sections)
    {
        titles.Add(section->GetTitle());
    }
    MARLIN_REQUIRE(titles.Count() >= 3);
    MARLIN_EXPECT(titles[0] == "FRAME" && titles[1] == "GPU" && titles[2] == "UI");
}
} // namespace Marlin::Tests
