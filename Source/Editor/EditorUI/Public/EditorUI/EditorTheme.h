#pragma once

#include "Asset/AssetRef.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "EditorUI/Signal.h"
#include "Math/Color.h"
#include "Misc/Guid.h"
#include "String/Name.h"
#include "String/StringView.h"

MARLIN_DECLARE_LOG_CATEGORY(EDITORUI_API, LogEditorTheme, Info, Warning);

namespace Marlin
{
class Icon;

/// The editor's colors, metrics and icons (Documentation/Architecture/09 §4.5), read from Editor/Theme.mtext in every
/// config layer, so a project or user overrides single tokens.
/// - Section `Colors`: token = "#RRGGBB" or "#RRGGBBAA" in sRGB, or the name of another color token.
/// - Section `Metrics`: token = a number in painter units.
/// - Section `Icons`: token = a link to an Icon asset, `<folder-open#UUID>`.
/// A nested block names its tokens with dots: `Text = { Primary = "#E6E6E8" }` is Text.Primary. Reading a token
/// follows the theme, so what is bound to one updates when the theme reloads.
class EDITORUI_API EditorTheme
{
public:
    EditorTheme();
    ~EditorTheme();
    MARLIN_NONCOPYABLE(EditorTheme);

    /// Reads Editor/Theme from the config layers.
    void LoadFromConfig();
    /// Reads *layers*, lowest first, as the layers of Editor/Theme.mtext; `@if` blocks never hold. For tests.
    void LoadFromText(Span<const StringView> layers);

    /// The color *token*; magenta, reported once, if the theme has none.
    [[nodiscard]] LinearColor GetColor(Name token) const;
    /// The metric *token*; zero, reported once, if the theme has none.
    [[nodiscard]] float32 GetMetric(Name token) const;
    /// The icon *token*, loading it on first use; null while it loads or, reported once, if the theme has none.
    [[nodiscard]] const Icon *GetIcon(Name token) const;

    /// The theme ThemeColor and ThemeMetric read, or null; EditorThemeSubsystem sets it.
    [[nodiscard]] static EditorTheme *GetCurrent();
    static void SetCurrent(EditorTheme *theme);

    static constexpr LinearColor MissingColor{1, 0, 1};

    struct Builder;

private:
    void Commit(Builder &builder);

    ReactiveOwner *_owner = nullptr;
    /// Bumped by every load; reading a token follows it.
    Signal<uint32> _version;
    HashMap<Name, LinearColor> _colors;
    HashMap<Name, float32> _metrics;
    HashMap<Name, Guid> _icons;
    mutable HashMap<Name, AssetRef<Icon>> _loadedIcons;
    mutable HashSet<Name> _reported;
};

/// A color token of the current theme as a property value that follows it:
/// `Box().Background(ThemeColor("Panel.Background"))`.
struct EDITORUI_API ThemeColor
{
    explicit ThemeColor(StringView token) : Token(token)
    {
    }

    [[nodiscard]] LinearColor operator()() const;

    Name Token;
};

/// A metric token of the current theme as a property value that follows it: `.Padding(ThemeMetric("Spacing.Medium"))`.
struct EDITORUI_API ThemeMetric
{
    explicit ThemeMetric(StringView token) : Token(token)
    {
    }

    [[nodiscard]] float32 operator()() const;

    Name Token;
};
} // namespace Marlin
