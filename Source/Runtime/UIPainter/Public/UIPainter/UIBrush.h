#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "UIPainter/UIGeometry.h"

namespace Marlin
{
/// What the values of an image mean, so the backend knows how to bring them to the display.
enum class UIImageRange : uint8
{
    /// Display-referred, 1 is the UI white: textures, thumbnails, SDR video, tonemapped SDR views. sRGB textures are
    /// read through sRGB views, so the shader sees linear values.
    Ui,
    /// Linear Rec. 709 in nits: views tonemapped for an HDR display.
    DisplayNits,
};

/// An image for DrawImage, referenced by bindless index: either the descriptor index of a texture view, or a frame
/// slot, standing for a texture of this frame's render graph that the backend binds when it draws (07 §5), which
/// RendererSubsystem::AllocateImageSlot hands out. Zero is no image.
struct UIImage
{
    uint32 Value = 0;
    UIImageRange Range = UIImageRange::Ui;
    bool IsFrameSlot = false;

    [[nodiscard]] static constexpr UIImage FromDescriptor(uint32 descriptorIndex, UIImageRange range = UIImageRange::Ui)
    {
        return {descriptorIndex, range, false};
    }

    /// The range of a frame slot comes from whoever binds it.
    [[nodiscard]] static constexpr UIImage FromFrameSlot(uint32 slot)
    {
        return {slot, UIImageRange::Ui, true};
    }

    [[nodiscard]] constexpr bool IsValid() const
    {
        return Value != 0 || (!IsFrameSlot && Range == UIImageRange::DisplayNits);
    }

    constexpr bool operator==(const UIImage &) const = default;
};

enum class UISampling : uint8
{
    Linear,
    /// Nearest texel, for pixel art and 1:1 previews.
    Point,
};

struct UIGradientStop
{
    /// Along the gradient, 0 to 1.
    float32 Offset = 0;
    /// Straight (not premultiplied) linear color.
    LinearColor Color;
};

enum class UIBrushKind : uint8
{
    Solid,
    /// Between Start and End, constant across.
    Linear,
    /// From Center outwards to Radius.
    Radial,
    /// Around Center, clockwise from StartAngle.
    Sweep,
};

/// How a shape is colored. Colors are straight linear RGBA; gradients interpolate premultiplied linear colors and
/// their geometry is in the painter's units under the current transform, like the shape's. A gradient holds up to
/// MaxStops stops; outside 0 to 1 it pads with the end colors.
struct UIBrush
{
    static constexpr uint32 MaxStops = 16;

    UIBrushKind Kind = UIBrushKind::Solid;
    LinearColor Color = LinearColor::White;
    Vector2 Start;
    Vector2 End;
    float32 Radius = 0;
    float32 StartAngle = 0;
    Span<const UIGradientStop> Stops;

    constexpr UIBrush() = default;

    constexpr UIBrush(const LinearColor &color) : Color(color)
    {
    }

    [[nodiscard]] static constexpr UIBrush Solid(const LinearColor &color)
    {
        return UIBrush(color);
    }

    /// *stops* must stay valid until the call it is passed to returns.
    [[nodiscard]] static constexpr UIBrush LinearGradient(Vector2 start, Vector2 end, Span<const UIGradientStop> stops)
    {
        UIBrush brush;
        brush.Kind = UIBrushKind::Linear;
        brush.Start = start;
        brush.End = end;
        brush.Stops = stops;
        return brush;
    }

    [[nodiscard]] static constexpr UIBrush RadialGradient(Vector2 center, float32 radius,
                                                          Span<const UIGradientStop> stops)
    {
        UIBrush brush;
        brush.Kind = UIBrushKind::Radial;
        brush.Start = center;
        brush.Radius = radius;
        brush.Stops = stops;
        return brush;
    }

    /// *startAngle* in radians, clockwise from +X on screen.
    [[nodiscard]] static constexpr UIBrush SweepGradient(Vector2 center, float32 startAngle,
                                                         Span<const UIGradientStop> stops)
    {
        UIBrush brush;
        brush.Kind = UIBrushKind::Sweep;
        brush.Start = center;
        brush.StartAngle = startAngle;
        brush.Stops = stops;
        return brush;
    }
};

/// How DrawImage maps an image into its rectangle.
struct UIImageDraw
{
    /// The part of the image drawn, in 0 to 1 texture coordinates.
    UIRect Uv = {0, 0, 1, 1};
    /// Multiplies the image; straight linear RGBA.
    LinearColor Tint = LinearColor::White;
    float32 CornerRadius = 0;
    UISampling Sampling = UISampling::Linear;
};
} // namespace Marlin
