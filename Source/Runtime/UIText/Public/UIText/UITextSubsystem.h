#pragma once

#include "Asset/AssetRef.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Engine/Subsystem.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "String/StringView.h"
#include "UIPainter/UIPainter.h"
#include "UIPainter/UIPath.h"
#include "UIPainter/UIShapedRun.h"
#include "UIText/Font.h"
#include "UIText/UITextSubsystem.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(UITEXT_API, LogUIText, Info, Warning);

struct hb_draw_funcs_t;

namespace Marlin
{
/// What to shape with: the faces to try in order, the first that has a character drawing it, and the size.
struct UITextStyle
{
    Span<const UIFontFace> Faces;
    /// Painter units per em.
    float32 Size = 14;
};

/// A line of text shaped into runs, one per stretch drawn by one face, laid end to end from the origin.
struct UIShapedText
{
    List<UIShapedRun> Runs;
    float32 Width = 0;
    /// Above and below the baseline, from the first face: the line's box.
    float32 Ascent = 0;
    float32 Descent = 0;
    float32 LineGap = 0;
};

/// Draws every run of *text* with its origin on the baseline at *origin*.
UITEXT_API void DrawText(UIPainter &painter, const UIShapedText &text, Vector2 origin, const UIBrush &brush);

/// The text system (09 §2): registers Font assets as UIFontFaces, shapes UTF-8 with HarfBuzz (scripts, ligatures,
/// kerning, marks) falling back through the style's faces per character, and hands the GPU backend glyph outlines.
/// The engine's default faces (Engine.UIText) load at startup. Shaping and outlines are thread-safe; a face whose
/// font is still loading draws nothing.
MCLASS(Config = "Engine.UIText", Dependencies = "AssetSubsystem")
class UITEXT_API UITextSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// The UI fonts, in fallback order (Inter, then Noto Sans).
    MPROPERTY()
    static List<SoftAssetRef<Font>> DefaultFonts;
    /// The monospace font for tables, logs and stats (JetBrains Mono).
    MPROPERTY()
    static SoftAssetRef<Font> MonospaceFont;

    UITextSubsystem();
    ~UITextSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// The face of *font*, registered on first use; it draws once the font has loaded. Main thread.
    UIFontFace RegisterFont(const AssetRef<Font> &font);

    /// The default faces in fallback order, then the monospace face's fallbacks.
    [[nodiscard]] Span<const UIFontFace> GetDefaultFaces() const
    {
        return _defaultFaces.AsSpan();
    }

    /// The monospace face followed by the default faces as fallbacks.
    [[nodiscard]] Span<const UIFontFace> GetMonospaceFaces() const
    {
        return _monospaceFaces.AsSpan();
    }

    /// True once *face*'s font has loaded.
    [[nodiscard]] bool IsReady(UIFontFace face) const;

    /// Shapes one line of UTF-8; characters no face has show the first face's missing glyph.
    void Shape(StringView text, const UITextStyle &style, UIShapedText &shaped) const;

    /// The outline of *glyph* in ems, Y down from the baseline origin, cubic curves converted to quadratics; false if
    /// the face is not ready or the glyph has no outline.
    bool GetGlyphOutline(UIFontFace face, uint32 glyph, UIPath &outline) const;

private:
    struct Face;

    /// Shared with the completion callbacks of the fonts, which the asset table may still run after Deinitialize,
    /// as it does for loads it finishes at shutdown.
    struct Lifetime final : RefCounted
    {
        bool Alive = true;
    };

    [[nodiscard]] const Face *FindReady(UIFontFace face) const;
    void Load(Face &face);
    void Unload(Face &face);

    mutable ReadWriteLock _lock;
    hb_draw_funcs_t *_drawFuncs = nullptr;
    List<UniquePtr<Face>> _faces;
    List<UIFontFace> _defaultFaces;
    List<UIFontFace> _monospaceFaces;
    SharedPtr<Lifetime> _lifetime{new Lifetime()};
};
} // namespace Marlin
