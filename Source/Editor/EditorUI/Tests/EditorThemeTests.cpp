#include "EditorUI/EditorTheme.h"
#include "EditorUI/UIElement.h"
#include "EditorUI/UIWindowRoot.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
constexpr StringView BaseLayer = R"(!Version "1.0"
Colors
{
    Gray = "#808080"
    White = "#FFFFFF80"
    Panel =
    {
        Background = Gray
        Border = "Panel.Background"
    }
    Loop = Cycle
    Cycle = Loop
    Broken = "#12345"
}
Metrics
{
    Spacing = { Small = 4, Medium = 8 }
    Width = 1.5
}
)";

constexpr StringView OverrideLayer = R"(!Version "1.0"
Colors
{
    Gray = "#FF0000"
}
Metrics
{
    Spacing = { Medium = 10 }
}
)";

bool ThemeNear(float32 a, float32 b)
{
    return a - b < 0.001f && b - a < 0.001f;
}

bool ThemeColorIs(LinearColor color, LinearColor expected)
{
    return ThemeNear(color.R, expected.R) && ThemeNear(color.G, expected.G) && ThemeNear(color.B, expected.B) &&
           ThemeNear(color.A, expected.A);
}
} // namespace

MARLIN_TEST(EditorTheme, ReadsHexColorsReferencesAndNestedTokens)
{
    EditorTheme theme;
    const StringView layers[] = {BaseLayer};
    theme.LoadFromText(layers);
    const LinearColor gray = LinearColor::FromSrgb({128, 128, 128});
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("Gray")), gray));
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("White")), {1, 1, 1, 128.0f / 255.0f}));
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("Panel.Background")), gray));
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("Panel.Border")), gray));
    MARLIN_EXPECT(ThemeNear(theme.GetMetric(Name("Spacing.Medium")), 8) &&
                  ThemeNear(theme.GetMetric(Name("Width")), 1.5f));
}

MARLIN_TEST(EditorTheme, MissingCyclicAndMalformedTokensAreMagenta)
{
    EditorTheme theme;
    const StringView layers[] = {BaseLayer};
    theme.LoadFromText(layers);
    MARLIN_EXPECT(theme.GetColor(Name("Loop")) == EditorTheme::MissingColor);
    MARLIN_EXPECT(theme.GetColor(Name("Broken")) == EditorTheme::MissingColor);
    MARLIN_EXPECT(theme.GetColor(Name("Nowhere")) == EditorTheme::MissingColor);
    MARLIN_EXPECT(theme.GetMetric(Name("Nowhere")) == 0);
}

MARLIN_TEST(EditorTheme, LaterLayersOverrideSingleTokens)
{
    EditorTheme theme;
    const StringView layers[] = {BaseLayer, OverrideLayer};
    theme.LoadFromText(layers);
    const LinearColor red{1, 0, 0};
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("Gray")), red));
    // References resolve after merging, so roles follow an overridden palette.
    MARLIN_EXPECT(ThemeColorIs(theme.GetColor(Name("Panel.Border")), red));
    MARLIN_EXPECT(ThemeNear(theme.GetMetric(Name("Spacing.Medium")), 10) &&
                  ThemeNear(theme.GetMetric(Name("Spacing.Small")), 4));
}

MARLIN_TEST(EditorTheme, BoundPropertiesFollowReloads)
{
    EditorTheme theme;
    EditorTheme::SetCurrent(&theme);
    const StringView base[] = {BaseLayer};
    theme.LoadFromText(base);
    UIElementNode *panel = nullptr;
    {
        UIWindowRoot window(nullptr);
        window.SetContent([&] {
            UIElement box = Box().Background(ThemeColor("Panel.Background")).Padding(ThemeMetric("Spacing.Medium"));
            panel = box.GetNode();
            return box;
        });
        Reactive::Flush();
        MARLIN_EXPECT(ThemeColorIs(panel->Style.Background, LinearColor::FromSrgb({128, 128, 128})));
        MARLIN_EXPECT(ThemeNear(panel->Style.Padding.Left, 8));

        const StringView overridden[] = {BaseLayer, OverrideLayer};
        theme.LoadFromText(overridden);
        Reactive::Flush();
        MARLIN_EXPECT(ThemeColorIs(panel->Style.Background, {1, 0, 0}));
        MARLIN_EXPECT(ThemeNear(panel->Style.Padding.Left, 10));
    }
    EditorTheme::SetCurrent(nullptr);
}
} // namespace Marlin::Tests
