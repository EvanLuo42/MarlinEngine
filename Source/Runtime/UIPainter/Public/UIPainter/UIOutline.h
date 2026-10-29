#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Vector.h"
#include "UIPainter/UICommandList.h"
#include "UIPainter/UIPath.h"

namespace Marlin
{
/// A segment of a contour as strokes and outlines see it: a line has its control point at its middle.
struct UIPathSegment
{
    Vector2 P0;
    Vector2 Control;
    Vector2 P1;
    bool Quad = false;

    [[nodiscard]] UIPAINTER_API Vector2 StartTangent() const;
    [[nodiscard]] UIPAINTER_API Vector2 EndTangent() const;
};

struct UIPathContour
{
    List<UIPathSegment> Segments;
    Vector2 Start;
    bool Closed = false;
};

/// Splits *path* into contours of non-degenerate segments, written over *contours* from the start so that its elements
/// and their segment lists are reused; returns how many it wrote. Elements past that are left from earlier calls.
UIPAINTER_API usize GetPathContours(const UIPath &path, List<UIPathContour> &contours);

/// Appends the outline record of *path*, every contour closed, to *curves* and *words* (UIGpuOutlineHeader, then
/// the band ranges and band curve lists): what an Outline command fills. Returns the record's offset in *words*, or
/// ~0u for a path without area. The glyph cache builds glyph outlines with it too.
UIPAINTER_API uint32 AppendOutline(const UIPath &path, List<UIGpuCurve> &curves, List<uint32> &words);

/// A path's outline record encoded once into its own lists (the record at offset 0), so that it is drawn again with
/// UIPainter::FillOutline, at any origin and scale, without being flattened and banded again.
struct UIPAINTER_API UIEncodedOutline
{
    List<UIGpuCurve> Curves;
    List<uint32> Words;
    /// The path's bounds, in its units.
    UIRect Bounds;

    /// Encodes *path*, replacing what was encoded; false, and empty, for a path without area.
    bool Encode(const UIPath &path);

    [[nodiscard]] bool IsEmpty() const
    {
        return Words.IsEmpty();
    }
};

/// Appends *outline*'s record to *curves* and *words*, moving its offsets there; returns the record's offset.
UIPAINTER_API uint32 AppendOutline(const UIEncodedOutline &outline, List<UIGpuCurve> &curves, List<uint32> &words);
} // namespace Marlin
