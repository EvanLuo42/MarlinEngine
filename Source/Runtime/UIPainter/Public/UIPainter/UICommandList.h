#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "UIPainter/UIGeometry.h"

namespace Marlin
{
/// The command stream a UIPainter records and the GPU backend bins and rasterizes (09 §2). The GPU structs mirror
/// Shaders/Passes/Ui/UiTypes.mshader (scalar layout, 07 §1.
enum class UICommandKind : uint8
{
    /// x0 y0 x1 y1, radii TL TR BR BL, stroke width (0 fills).
    RoundedRect,
    /// cx cy rx ry, stroke width (0 fills).
    Ellipse,
    /// x0 y0 x1 y1, radii TL TR BR BL, blur sigma: a Gaussian-blurred rounded rectangle.
    BoxShadow,
    /// x0 y0 x1 y1, half width, joined ends: a stroke piece with butt ends. Joined ends (bit 0 start, bit 1 end) meet
    /// another piece, so they do not fade there and pieces leave no seam.
    StrokeLine,
    /// x0 y0 x1 y1 x2 y2, half width, joined ends: a quadratic stroke piece, the same.
    StrokeQuad,
    /// cx cy radius: round joins and caps.
    Disk,
    /// Four corners of a convex quadrilateral (repeat one for a triangle): miter and bevel joins, square caps.
    Polygon,
    /// Outline record offset, origin x y, scale (painter units per outline unit): a filled outline of quadratic
    /// curves, from the frame's paths or, with GlyphCache, the cached glyph outlines.
    Outline,
    /// Face, glyph, origin x y, size, glyph bounds x0 y0 x1 y1 relative to the origin. As recorded: the GPU backend
    /// replaces it with an Outline or an AtlasGlyph.
    Glyph,
    /// x0 y0 x1 y1, atlas texels u0 v0 u1 v1: a cached small glyph's coverage.
    AtlasGlyph,
    /// x0 y0 x1 y1, uv u0 v0 u1 v1, corner radius, image, image range.
    Image,
    /// Device x0 y0 x1 y1, child canvas: a layer's result composited with its opacity.
    Canvas,
    /// Device x0 y0 x1 y1, child canvas: the layer's blurred backdrop, drawn under the layer wherever its content
    /// has any alpha.
    Backdrop,
};

enum class UICommandFlags : uint8
{
    None = 0,
    EvenOdd = 1 << 0,
    /// The outline lives in the glyph cache, not the frame's paths.
    GlyphCache = 1 << 1,
    PointSampling = 1 << 2,
    /// The image is a frame slot the backend resolves to a descriptor.
    FrameSlot = 1 << 3,
};
MARLIN_ENUM_FLAGS(UICommandFlags)

/// One primitive: 80 bytes.
struct UIGpuCommand
{
    static constexpr uint32 DataCount = 11;

    /// Kind in the low byte, flags in the next.
    uint32 Header = 0;
    uint32 Brush = 0;
    /// Index into the clips; 0 is none.
    uint32 Clip = 0;
    /// Index into the transforms; 0 is the canvas's identity.
    uint32 Transform = 0;
    /// The index of the first command of its group within the canvas: consecutive commands of one group (the pieces
    /// of a stroke) unite their coverage and blend once.
    uint32 Group = 0;
    /// Device-space bounds, with the antialiasing margin: what binning reads.
    float32 Bounds[4] = {};
    float32 Data[DataCount] = {};

    [[nodiscard]] UICommandKind GetKind() const
    {
        return static_cast<UICommandKind>(Header & 0xFF);
    }

    [[nodiscard]] UICommandFlags GetFlags() const
    {
        return static_cast<UICommandFlags>((Header >> 8) & 0xFF);
    }
};
static_assert(sizeof(UIGpuCommand) == 80);

enum class UIGpuBrushKind : uint32
{
    Solid,
    Linear,
    Radial,
    Sweep,
};

/// 64 bytes. Color is premultiplied linear RGBA; gradient geometry is in the command's local units: Linear start xy
/// and end xy, Radial center xy and radius, Sweep center xy and start angle.
struct UIGpuBrush
{
    uint32 Kind = 0;
    uint32 StopOffset = 0;
    uint32 StopCount = 0;
    uint32 Pad = 0;
    float32 Color[4] = {};
    float32 Geometry[8] = {};
};
static_assert(sizeof(UIGpuBrush) == 64);

/// 32 bytes: premultiplied linear RGBA.
struct UIGpuGradientStop
{
    float32 Offset = 0;
    float32 Pad[3] = {};
    float32 Color[4] = {};
};
static_assert(sizeof(UIGpuGradientStop) == 32);

/// A device-to-local transform: local = Inverse * device. 32 bytes.
struct UIGpuTransform
{
    float32 M11 = 1;
    float32 M12 = 0;
    float32 M21 = 0;
    float32 M22 = 1;
    float32 Dx = 0;
    float32 Dy = 0;
    /// Local units per device pixel, for the antialiasing width.
    float32 PixelSize = 1;
    float32 Pad = 0;
};
static_assert(sizeof(UIGpuTransform) == 32);

/// A clip: the device rectangle it and its ancestors leave, and, when it is Shaped (rounded or not axis-aligned), its
/// rounded rectangle in its transform's local units. 64 bytes.
struct UIGpuClip
{
    float32 DeviceRect[4] = {};
    float32 Rect[4] = {};
    float32 Radii[4] = {};
    uint32 Transform = 0;
    /// The nearest Shaped ancestor, 0 for none: a pixel tests the chain of shaped clips only.
    uint32 Parent = 0;
    uint32 Shaped = 0;
    uint32 Pad = 0;
};
static_assert(sizeof(UIGpuClip) == 64);

/// A quadratic curve p0 p1 p2; lines have p1 at their middle. 24 bytes.
struct UIGpuCurve
{
    float32 Points[6] = {};
};
static_assert(sizeof(UIGpuCurve) == 24);

/// How an outline record starts in the outline words; bands and their curve lists follow it. The outline is cut into
/// BandsY horizontal and BandsX vertical bands over its bounds; each band lists the curves crossing it, so a pixel
/// tests only those for its horizontal and vertical ray (09 §2).
struct UIGpuOutlineHeader
{
    uint32 CurveOffset = 0;
    uint32 CurveCount = 0;
    uint32 BandsY = 0;
    uint32 BandsX = 0;
    float32 Bounds[4] = {};
};
static_assert(sizeof(UIGpuOutlineHeader) == 32);

/// A canvas: the root (index 0) is the target itself; every other one is a layer, painted into its own texture.
struct UICanvas
{
    List<UIGpuCommand> Commands;
    /// The canvas a Canvas command composites this layer into.
    uint32 Parent = 0;
    float32 Opacity = 1;
    /// Gaussian standard deviation applied to the layer's result, in device pixels.
    float32 Blur = 0;
    /// Gaussian standard deviation applied to what lies behind the layer, in device pixels.
    float32 BackdropBlur = 0;
    /// Device-space bounds of the layer's content, whole pixels; the root covers the target.
    UIRect Bounds;
};

/// What a painter records for one target in a frame: canvases of commands and the tables their indices point into.
/// The GPU backend draws it after the scene views (01 §4).
struct UIPAINTER_API UICommandList
{
    List<UICanvas> Canvases;
    List<UIGpuBrush> Brushes;
    List<UIGpuGradientStop> Stops;
    /// Index 0 is the identity.
    List<UIGpuTransform> Transforms;
    /// Index 0 is no clip.
    List<UIGpuClip> Clips;
    List<UIGpuCurve> Curves;
    /// Outline headers, band ranges (offset, count pairs) and band curve indices, as 32-bit words.
    List<uint32> OutlineWords;

    /// Empties the list for the next recording; the canvases' command lists keep their memory for the canvases added
    /// after it.
    void Reset();
    /// Adds a canvas, reusing a command list a Reset kept.
    UICanvas &AddCanvas();
    [[nodiscard]] usize GetCommandCount() const;
    /// Paints *other* over this list: its root commands follow this root's, its layers become layers of this list,
    /// and every index they hold is moved to the merged tables. Both lists must share a target size.
    void Append(const UICommandList &other);

private:
    /// The command lists of the canvases a Reset removed, empty but with their capacity.
    List<List<UIGpuCommand>> _spareCommands;
};
} // namespace Marlin
