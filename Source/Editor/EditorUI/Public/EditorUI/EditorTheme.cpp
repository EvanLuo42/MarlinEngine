#include "EditorUI/EditorTheme.h"

#include "Asset/Assets.h"
#include "Config/Config.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextValue.h"
#include "String/Number.h"
#include "String/String.h"
#include "UIIcon/Icon.h"

MARLIN_DEFINE_LOG_CATEGORY(LogEditorTheme);

namespace Marlin
{
namespace
{
EditorTheme *CurrentTheme = nullptr;

/// Longer reference chains than this are taken for cycles.
constexpr uint32 MaxReferenceDepth = 16;

/// "#RRGGBB" or "#RRGGBBAA", sRGB with linear alpha.
bool ParseHexColor(StringView text, LinearColor &color)
{
    if (!text.StartsWith('#') || (text.Length() != 7 && text.Length() != 9))
    {
        return false;
    }
    uint32 value = 0;
    if (!TryParse(text.Substring(1), value, 16))
    {
        return false;
    }
    if (text.Length() == 7)
    {
        value = value << 8 | 0xFF;
    }
    color = LinearColor::FromSrgb(Color32::FromHex(value));
    return true;
}
} // namespace

/// Merges the layers' tokens, later layers replacing single tokens, then resolves the color references.
struct EditorTheme::Builder
{
    struct RawColor
    {
        String Text;
        String Source;
        uint32 Line = 0;
    };

    HashMap<Name, RawColor> RawColors;
    HashMap<Name, float32> Metrics;
    HashMap<Name, Guid> Icons;
    HashMap<Name, LinearColor> Colors;

    enum class Section : uint8
    {
        Colors,
        Metrics,
        Icons,
    };

    void ReadLayer(const MarlinTextObject &object, const MarlinTextConditions &conditions, StringView source)
    {
        const StringView name = object.GetTypeName();
        if (name == "Colors")
        {
            ReadBlock(object.GetFields(), conditions, Section::Colors, {}, source);
        }
        else if (name == "Metrics")
        {
            ReadBlock(object.GetFields(), conditions, Section::Metrics, {}, source);
        }
        else if (name == "Icons")
        {
            ReadBlock(object.GetFields(), conditions, Section::Icons, {}, source);
        }
    }

    void ReadBlock(Span<const MarlinTextField> fields, const MarlinTextConditions &conditions, Section section,
                   StringView prefix, StringView source)
    {
        for (const MarlinTextField &field : fields)
        {
            if (field.IsDirective || !conditions.Holds(field.Condition, field.InElse))
            {
                continue;
            }
            String token(prefix);
            if (!prefix.IsEmpty())
            {
                token.Append('.');
            }
            token.Append(field.Key);
            const MarlinTextValue &value = field.Value;
            if (field.Operation != MarlinTextOperation::Assign)
            {
                MARLIN_LOG(LogEditorTheme, Warning, "{0}({1}): theme token {2} takes only =", source, field.Line,
                           token);
            }
            else if (value.Kind == MarlinTextValueKind::Block)
            {
                ReadBlock(value.Fields, conditions, section, token.AsView(), source);
            }
            else if (section == Section::Colors &&
                     (value.Kind == MarlinTextValueKind::String || value.Kind == MarlinTextValueKind::Identifier))
            {
                RawColors.Set(Name(token.AsView()), {String(value.Text), String(source), field.Line});
            }
            else if (section == Section::Metrics && value.IsNumber())
            {
                Metrics.Set(Name(token.AsView()), static_cast<float32>(value.AsFloat()));
            }
            else if (section == Section::Icons && value.Kind == MarlinTextValueKind::ObjectLink)
            {
                Icons.Set(Name(token.AsView()), value.LinkGuid);
            }
            else
            {
                const StringView expected = section == Section::Colors    ? "\"#RRGGBB[AA]\" or another color token"
                                            : section == Section::Metrics ? "a number"
                                                                          : "a link to an Icon, <name#UUID>";
                MARLIN_LOG(LogEditorTheme, Warning, "{0}({1}): theme token {2} must be {3}", source, field.Line, token,
                           expected);
            }
        }
    }

    void Resolve()
    {
        for (const auto &entry : RawColors)
        {
            LinearColor color = MissingColor;
            if (ResolveColor(entry.Key, 0, color))
            {
                Colors.Set(entry.Key, color);
            }
        }
    }

    bool ResolveColor(Name token, uint32 depth, LinearColor &color) const
    {
        const RawColor *raw = RawColors.Find(token);
        if (raw == nullptr)
        {
            return false;
        }
        if (ParseHexColor(raw->Text.AsView(), color))
        {
            return true;
        }
        if (raw->Text.AsView().StartsWith('#'))
        {
            MARLIN_LOG(LogEditorTheme, Warning, "{0}({1}): {2} is not #RRGGBB or #RRGGBBAA", raw->Source, raw->Line,
                       raw->Text);
            return false;
        }
        const Name target = Name::Find(raw->Text.AsView());
        if (depth >= MaxReferenceDepth || target.IsNone() || !RawColors.Contains(target))
        {
            MARLIN_LOG(LogEditorTheme, Warning, "{0}({1}): color token {2} names {3}, which {4}", raw->Source,
                       raw->Line, token, raw->Text,
                       depth >= MaxReferenceDepth ? "refers back to itself" : "is not a color token");
            return false;
        }
        return ResolveColor(target, depth + 1, color);
    }
};

EditorTheme::EditorTheme()
{
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] { _version = CreateSignal<uint32>(0); });
}

EditorTheme::~EditorTheme()
{
    if (CurrentTheme == this)
    {
        CurrentTheme = nullptr;
    }
    Reactive::DisposeOwner(_owner);
}

void EditorTheme::LoadFromConfig()
{
    Builder builder;
    const auto read = [&](const MarlinTextObject &object, const MarlinTextConditions &conditions, StringView source) {
        builder.ReadLayer(object, conditions, source);
    };
    Config::ForEachSectionWithConditions("Editor/Theme.Colors", read);
    Config::ForEachSectionWithConditions("Editor/Theme.Metrics", read);
    Config::ForEachSectionWithConditions("Editor/Theme.Icons", read);
    Commit(builder);
}

void EditorTheme::LoadFromText(Span<const StringView> layers)
{
    Builder builder;
    const MarlinTextConditions conditions;
    MarlinTextParseOptions options;
    options.AllowConditions = true;
    for (const StringView text : layers)
    {
        MarlinTextDocument document;
        MarlinTextDiagnostics diagnostics;
        if (document.Parse(text, diagnostics, options))
        {
            List<usize> active;
            document.GetActiveObjects(conditions, active);
            for (const usize index : active)
            {
                if (const MarlinTextObject *object = document.GetObject(index, diagnostics))
                {
                    builder.ReadLayer(*object, conditions, "Editor/Theme.mtext");
                }
            }
        }
        for (const MarlinTextDiagnostic &diagnostic : diagnostics.GetAll())
        {
            MARLIN_LOG(LogEditorTheme, Warning, "Editor/Theme.mtext({0}): {1}", diagnostic.Line, diagnostic.Message);
        }
    }
    Commit(builder);
}

void EditorTheme::Commit(Builder &builder)
{
    builder.Resolve();
    _colors = Move(builder.Colors);
    _metrics = Move(builder.Metrics);
    _icons = Move(builder.Icons);
    _loadedIcons.Clear();
    _reported.Clear();
    _version.Set(_version.Peek() + 1);
}

LinearColor EditorTheme::GetColor(Name token) const
{
    (void)_version.Get();
    if (const LinearColor *color = _colors.Find(token))
    {
        return *color;
    }
    if (_reported.Add(token))
    {
        MARLIN_LOG(LogEditorTheme, Warning, "The theme has no color {0}", token);
    }
    return MissingColor;
}

float32 EditorTheme::GetMetric(Name token) const
{
    (void)_version.Get();
    if (const float32 *metric = _metrics.Find(token))
    {
        return *metric;
    }
    if (_reported.Add(token))
    {
        MARLIN_LOG(LogEditorTheme, Warning, "The theme has no metric {0}", token);
    }
    return 0;
}

const Icon *EditorTheme::GetIcon(Name token) const
{
    if (const AssetRef<Icon> *loaded = _loadedIcons.Find(token))
    {
        return loaded->Get();
    }
    const Guid *guid = _icons.Find(token);
    if (guid == nullptr)
    {
        if (_reported.Add(token))
        {
            MARLIN_LOG(LogEditorTheme, Warning, "The theme has no icon {0}", token);
        }
        return nullptr;
    }
    // Critical: the UI needs it this frame, and a splash screen waits for it (Documentation/Architecture/09 §4.6).
    return _loadedIcons.Set(token, Assets::Load<Icon>(*guid, IoPriority::Critical)).Get();
}

EditorTheme *EditorTheme::GetCurrent()
{
    return CurrentTheme;
}

void EditorTheme::SetCurrent(EditorTheme *theme)
{
    CurrentTheme = theme;
}

LinearColor ThemeColor::operator()() const
{
    const EditorTheme *theme = EditorTheme::GetCurrent();
    return theme != nullptr ? theme->GetColor(Token) : EditorTheme::MissingColor;
}

float32 ThemeMetric::operator()() const
{
    const EditorTheme *theme = EditorTheme::GetCurrent();
    return theme != nullptr ? theme->GetMetric(Token) : 0;
}
} // namespace Marlin
