#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "UIPainter/UICommandList.h"
#include "UIPainter/UIShapedRun.h"
#include "UIPainterRenderer/UIPainterShaders.h"

namespace Marlin
{
class UITextSubsystem;

/// Where a small glyph sits in the atlas, at one size and subpixel phase.
struct UIAtlasEntry
{
    uint16 X = 0;
    uint16 Y = 0;
    uint16 Width = 0;
    uint16 Height = 0;
    /// The device pixel of the slot's top-left, relative to the glyph origin snapped as Resolve snaps it.
    int16 Left = 0;
    int16 Top = 0;
};

/// The CPU side of UIPainter's glyph cache (09 §2): glyph outlines, in ems with Y down, as outline records the tile
/// rasterizer reads, and the glyph atlas for small axis-aligned text, packed in shelves and emptied whole when full.
/// Resolve rewrites a frame's Glyph commands into Outline or AtlasGlyph commands and queues what is new; the GPU
/// backend uploads the new outline words and rasterizes the queued atlas glyphs. Main thread only.
class UIPAINTERRENDERER_API UIGlyphCache
{
public:
    static constexpr uint32 AtlasSize = 2048;
    /// Glyphs at most this many device pixels per em come from the atlas.
    static constexpr float32 MaxAtlasSize = 24;
    static constexpr uint32 SubpixelPhases = 4;

    /// Rewrites every Glyph command of *commands*; drops glyphs whose face is not ready or that draw nothing.
    void Resolve(UICommandList &commands, const UITextSubsystem &text);

    [[nodiscard]] Span<const UIGpuCurve> GetCurves() const
    {
        return _curves.AsSpan();
    }

    [[nodiscard]] Span<const uint32> GetWords() const
    {
        return _words.AsSpan();
    }

    /// How much of the atlas the shelves have taken, 0 to 1.
    [[nodiscard]] float32 GetAtlasUsage() const
    {
        return static_cast<float32>(_shelfY + _shelfHeight) / static_cast<float32>(AtlasSize);
    }

    [[nodiscard]] uint32 GetAtlasGlyphCount() const
    {
        return static_cast<uint32>(_atlas.Count());
    }

    [[nodiscard]] uint32 GetOutlineCount() const
    {
        return static_cast<uint32>(_outlines.Count());
    }

    /// The atlas glyphs queued since the last call, and whether the atlas was emptied first.
    void TakeUploads(List<UiAtlasGlyph> &atlasGlyphs, bool &clearAtlas);

private:
    struct OutlineKey
    {
        uint32 Face = 0;
        uint32 Glyph = 0;

        [[nodiscard]] uint64 GetHash() const
        {
            return (static_cast<uint64>(Face) << 32) | Glyph;
        }

        bool operator==(const OutlineKey &) const = default;
    };

    struct AtlasKey
    {
        uint32 Face = 0;
        uint32 Glyph = 0;
        /// Pixels per em in eighths.
        uint32 Size = 0;
        uint32 Phase = 0;

        [[nodiscard]] uint64 GetHash() const
        {
            return (static_cast<uint64>(Face) << 40) ^ (static_cast<uint64>(Glyph) << 16) ^ (Size << 2) ^ Phase;
        }

        bool operator==(const AtlasKey &) const = default;
    };

    /// What ResolveAll decided for one Glyph command, applied once every glyph has its slot.
    struct ResolvedGlyph
    {
        UIGpuCommand *Command = nullptr;
        /// ~0u drops the glyph.
        uint32 Record = ~0u;
        bool Atlas = false;
        UIAtlasEntry Entry;
        float32 SnappedX = 0;
        float32 SnappedY = 0;
    };

    /// A command transform's forward matrix and how glyphs under it scale, worked out once per transform.
    struct TransformInfo
    {
        UITransform Forward;
        float32 DeviceScale = 1;
        bool AxisAligned = false;
        bool Known = false;
    };

    /// The outline record of a glyph, ~0u if it has no outline.
    uint32 FindOutline(const UITextSubsystem &text, UIFontFace face, uint32 glyph);
    bool Allocate(uint32 width, uint32 height, uint16 &x, uint16 &y);
    void ResetAtlas();
    /// False, rewriting nothing, when the atlas fills up.
    bool ResolveAll(UICommandList &commands, const UITextSubsystem &text);
    const TransformInfo &GetTransformInfo(const UICommandList &commands, uint32 index);

    List<UIGpuCurve> _curves;
    List<uint32> _words;
    HashMap<OutlineKey, uint32> _outlines;
    HashMap<AtlasKey, UIAtlasEntry> _atlas;
    uint32 _shelfX = 0;
    uint32 _shelfY = 0;
    uint32 _shelfHeight = 0;
    List<UiAtlasGlyph> _pendingGlyphs;
    bool _clearAtlas = true;
    /// Scratch for ResolveAll, kept between frames.
    List<ResolvedGlyph> _resolved;
    List<TransformInfo> _transformInfos;
};
} // namespace Marlin
