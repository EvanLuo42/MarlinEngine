#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "UIPainter/UIBrush.h"
#include "UIPainter/UICommandList.h"
#include "UIPainter/UIGeometry.h"
#include "UIPainter/UIOutline.h"
#include "UIPainter/UIPath.h"
#include "UIPainter/UIShapedRun.h"

MARLIN_DECLARE_LOG_CATEGORY(UIPAINTER_API, LogUIPainter, Info, Warning);

namespace Marlin
{
/// What PushLayer paints into: an offscreen canvas composited into its parent when popped.
struct UILayer
{
    /// Multiplies the layer's result as it is composited, 0 to 1; a default layer composites unchanged.
    float32 Opacity = 1;
    /// Gaussian standard deviation blurring the layer's content, in painter units.
    float32 Blur = 0;
    /// Gaussian standard deviation blurring what lies behind the layer (frosted glass), in painter units; the blurred
    /// backdrop shows wherever the layer's content has any alpha.
    float32 BackdropBlur = 0;
};

/// Immediate-mode drawing (09 §2): between Begin and End, every call appends primitives to a UICommandList, which
/// the GPU backend bins into tiles and rasterizes with analytic coverage. Coordinates are painter units (device
/// pixels divided by the scale), Y down, under the current transform; colors are straight linear RGBA.
///
/// Recording keeps no GPU state and no references to its inputs: paths, runs and gradient stops only need to live
/// until the call returns. Clips, transforms and layers are stacks; End pops what is left. One painter records one
/// target at a time; it may be reused for the next target or frame.
/// Calls outside Begin and End are ignored.
class UIPAINTER_API UIPainter
{
public:
    UIPainter();
    ~UIPainter();
    MARLIN_NONCOPYABLE(UIPainter);

    /// Starts recording into *commands*, which it resets, for a target of *width* × *height* device pixels with
    /// *scale* device pixels per painter unit.
    void Begin(UICommandList &commands, uint32 width, uint32 height, float32 scale);
    /// Ends recording; unbalanced pushes are popped and reported.
    void End();

    [[nodiscard]] bool IsRecording() const
    {
        return _commands != nullptr;
    }

    /// The target's size in painter units.
    [[nodiscard]] Vector2 GetSize() const;
    [[nodiscard]] float32 GetScale() const
    {
        return _scale;
    }

    void FillRect(const UIRect &rect, const UIBrush &brush);
    void FillRoundedRect(const UIRect &rect, const UICornerRadii &radii, const UIBrush &brush);
    /// A border of *width* inside *rect*'s edge.
    void StrokeRoundedRect(const UIRect &rect, const UICornerRadii &radii, float32 width, const UIBrush &brush);
    void FillEllipse(Vector2 center, Vector2 radii, const UIBrush &brush);
    /// A ring of *width* centered on the ellipse's edge.
    void StrokeEllipse(Vector2 center, Vector2 radii, float32 width, const UIBrush &brush);
    /// A soft shadow of a rounded rectangle: *blur* is the Gaussian standard deviation, *spread* grows the shape.
    void DrawBoxShadow(const UIRect &rect, const UICornerRadii &radii, float32 blur, float32 spread,
                       const LinearColor &color);

    void FillPath(const UIPath &path, const UIBrush &brush, UIFillRule rule = UIFillRule::NonZero);
    /// Fills an outline encoded once, its units placed at *origin* and scaled by *scale*: FillPath without flattening
    /// and banding the path again.
    void FillOutline(const UIEncodedOutline &outline, Vector2 origin, float32 scale, const UIBrush &brush,
                     UIFillRule rule = UIFillRule::NonZero);
    void StrokePath(const UIPath &path, const UIStrokeStyle &style, const UIBrush &brush);
    /// Strokes contours split from a path once (GetPathContours), offset by *origin*: StrokePath without splitting it
    /// again.
    void StrokeContours(Span<const UIPathContour> contours, Vector2 origin, const UIStrokeStyle &style,
                        const UIBrush &brush);
    void DrawLine(Vector2 from, Vector2 to, const UIStrokeStyle &style, const UIBrush &brush);

    /// Draws *run* with its origin on the baseline at *origin*. Color glyphs paint their own colors, with the brush's
    /// alpha. Small axis-aligned text comes from the glyph atlas, the rest from the outlines.
    void DrawText(const UIShapedRun &run, Vector2 origin, const UIBrush &brush);

    void DrawImage(UIImage image, const UIRect &rect, const UIImageDraw &draw = {});

    /// Limits painting to *rect* with rounded corners, inside the current clip, under the current transform.
    void PushClip(const UIRect &rect, const UICornerRadii &radii = {});
    void PopClip();

    /// Applies *transform* before the current one: later coordinates are in its space.
    void PushTransform(const UITransform &transform);
    void PopTransform();

    void PushLayer(float32 opacity, float32 blur = 0);
    void PushLayer(const UILayer &layer);
    void PopLayer();

    /// Device-space bounds of the current clip, for callers that skip invisible content.
    [[nodiscard]] UIRect GetClipBounds() const;
    /// Whether *rect*, under the current transform, lies wholly outside the current clip, so that what it bounds would
    /// paint nothing.
    [[nodiscard]] bool IsClippedOut(const UIRect &rect) const;

private:
    struct ClipState
    {
        uint32 Index = 0;
        UIRect DeviceBounds;
    };

    struct LayerState
    {
        uint32 Canvas = 0;
        /// The Canvas command in the parent that composites this layer, patched on pop.
        uint32 ParentCommand = 0;
        uint32 BackdropCommand = ~0u;
    };

    [[nodiscard]] UICanvas &GetCanvas();
    [[nodiscard]] const UITransform &GetTransform() const
    {
        return _transforms.Last();
    }

    uint32 AddBrush(const UIBrush &brush);
    uint32 AddTransform();
    /// Appends a command with *localBounds* (in the current transform's units) grown by *margin* device pixels;
    /// null when the clip hides it.
    UIGpuCommand *AddCommand(UICommandKind kind, const UIRect &localBounds, float32 margin, uint32 brush,
                             UICommandFlags flags = UICommandFlags::None);
    UIGpuCommand *AddDeviceCommand(UICommandKind kind, const UIRect &deviceBounds, uint32 brush);
    void AddStrokePieces(Span<const UIPathContour> contours, Vector2 origin, const UIStrokeStyle &style, uint32 brush);
    void BeginGroup();
    void EndGroup();

    UICommandList *_commands = nullptr;
    uint32 _width = 0;
    uint32 _height = 0;
    float32 _scale = 1;
    List<UITransform> _transforms;
    /// The transform index of each entry of _transforms, made on first use.
    List<uint32> _transformIndices;
    List<ClipState> _clips;
    List<LayerState> _layers;
    uint32 _group = ~0u;
    /// Last brush added, reused while it repeats.
    UIGpuBrush _lastBrush;
    uint32 _lastBrushIndex = ~0u;
    /// Scratch for StrokePath and DrawLine, kept with their segment lists so a stroke allocates nothing.
    List<UIPathContour> _contours;
};
} // namespace Marlin
