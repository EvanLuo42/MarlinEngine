#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "UIPainter/UIGeometry.h"

namespace Marlin
{
/// A font face as the text system registered it (one face of a Font asset at one variation); zero is none. The
/// painter only carries it to the GPU backend, which asks the text system for outlines.
struct UIFontFace
{
    uint32 Id = 0;

    [[nodiscard]] constexpr bool IsValid() const
    {
        return Id != 0;
    }

    constexpr bool operator==(const UIFontFace &) const = default;
};

struct UIShapedGlyph
{
    uint32 GlyphId = 0;
    /// The pen position, relative to the run's origin on the baseline, in the run's units.
    Vector2 Position;
    /// The ink bounds relative to Position; empty for glyphs that draw nothing, such as spaces.
    UIRect Bounds;
};

/// Glyphs placed by the shaper (HarfBuzz, 09 §2) for one face at one size: what DrawText draws. Shaping happens
/// once per text change, not per frame; the run is plain data.
struct UIShapedRun
{
    UIFontFace Face;
    /// Painter units per em.
    float32 Size = 0;
    List<UIShapedGlyph> Glyphs;
    /// Where the next run on the line starts, relative to this one's origin.
    Vector2 Advance;
    float32 Ascent = 0;
    float32 Descent = 0;
    /// Color glyphs (COLR) paint their own layers; DrawText's brush only tints their alpha.
    bool HasColorGlyphs = false;
};
} // namespace Marlin
