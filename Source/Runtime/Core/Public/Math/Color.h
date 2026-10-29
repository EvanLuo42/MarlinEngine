//! Linear floating-point and 8-bit sRGB color types.
//!
//! Lighting and blending work in `LinearColor`; `Color32` is the storage and texture format. The two convert through
//! the sRGB transfer function.

#pragma once

#include "CoreTypes.h"
#include "Math/MathUtility.h"
#include "Math/Vector.h"
#include "Math/Color.gen.h"

namespace Marlin
{
struct Color32;

/// A linear RGB color with alpha, stored as `float32` channels.
///
/// The channels are not clamped. Use `ToSrgb` to encode the color for storage or presentation, and `FromSrgb` to
/// decode an sRGB-encoded `Color32`. Lighting and blending operate in this space.
MSTRUCT(Tuple)
struct LinearColor
{
    MGENERATED_BODY()

    /// The linear red channel; not clamped; defaults to 0.
    MPROPERTY()
    float32 R = 0;
    /// The linear green channel; not clamped; defaults to 0.
    MPROPERTY()
    float32 G = 0;
    /// The linear blue channel; not clamped; defaults to 0.
    MPROPERTY()
    float32 B = 0;
    /// The alpha channel, from 0 (transparent) to 1 (opaque); defaults to 1.
    MPROPERTY()
    float32 A = 1;

    /// Creates opaque black.
    constexpr LinearColor() = default;

    /// Creates a color from `r`, `g`, `b` and `a`; `a` defaults to 1.
    constexpr LinearColor(float32 r, float32 g, float32 b, float32 a = 1) : R(r), G(g), B(b), A(a)
    {
    }

    /// Opaque black.
    static const LinearColor Black;
    /// Opaque white.
    static const LinearColor White;
    /// Fully transparent black.
    static const LinearColor Transparent;

    /// Decodes an sRGB-encoded 8-bit color into linear channels.
    ///
    /// The RGB channels use the sRGB transfer function; alpha is scaled to [0, 1] without gamma encoding.
    [[nodiscard]] static LinearColor FromSrgb(Color32 color);

    /// Creates a color from hue, saturation and value.
    ///
    /// `hue` is in degrees in [0, 360); `saturation` and `value` are expected in [0, 1]. `alpha` defaults to 1. The
    /// values are not checked.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// const LinearColor red = LinearColor::FromHsv(0.0f, 1.0f, 1.0f); // -> (1.0, 0.0, 0.0, 1.0)
    /// ```
    [[nodiscard]] static LinearColor FromHsv(float32 hue, float32 saturation, float32 value, float32 alpha = 1);

    /// Returns the componentwise sum of the two colors.
    constexpr LinearColor operator+(LinearColor other) const
    {
        return {R + other.R, G + other.G, B + other.B, A + other.A};
    }

    /// Returns the componentwise product of the two colors.
    constexpr LinearColor operator*(LinearColor other) const
    {
        return {R * other.R, G * other.G, B * other.B, A * other.A};
    }

    /// Returns all channels multiplied by `scale`.
    constexpr LinearColor operator*(float32 scale) const
    {
        return {R * scale, G * scale, B * scale, A * scale};
    }

    /// Compares all four channels exactly.
    constexpr bool operator==(const LinearColor &) const = default;

    /// Returns the linear interpolation from `a` to `b` by `t`.
    ///
    /// Each channel is interpolated independently; `t` is not clamped.
    [[nodiscard]] static constexpr LinearColor Lerp(LinearColor a, LinearColor b, float32 t)
    {
        return {a.R + (b.R - a.R) * t, a.G + (b.G - a.G) * t, a.B + (b.B - a.B) * t, a.A + (b.A - a.A) * t};
    }

    /// Returns the Rec. 709 luminance of the RGB channels, ignoring alpha.
    [[nodiscard]] constexpr float32 Luminance() const
    {
        return 0.2126f * R + 0.7152f * G + 0.0722f * B;
    }

    /// Returns the channels as a `Vector4` in RGBA order.
    [[nodiscard]] constexpr Vector4 ToVector4() const
    {
        return {R, G, B, A};
    }

    /// Encodes the color as 8-bit sRGB.
    ///
    /// Each channel is clamped to [0, 1] first. The RGB channels use the sRGB transfer function and alpha is scaled
    /// directly; the bytes round to nearest even.
    [[nodiscard]] Color32 ToSrgb() const;

    /// Converts one sRGB-encoded value to linear.
    ///
    /// Values at or below 0.04045 use the linear segment; larger values use the 2.4 power curve.
    [[nodiscard]] static float32 SrgbToLinear(float32 value)
    {
        return value <= 0.04045f ? value / 12.92f : Math::Pow((value + 0.055f) / 1.055f, 2.4f);
    }

    /// Converts one linear value to sRGB encoding.
    ///
    /// Values at or below 0.0031308 use the linear segment; larger values use the 1/2.4 power curve.
    [[nodiscard]] static float32 LinearToSrgb(float32 value)
    {
        return value <= 0.0031308f ? value * 12.92f : 1.055f * Math::Pow(value, 1 / 2.4f) - 0.055f;
    }

    /// Writes `(r, g, b, a)` to `output`, applying `spec` to each channel.
    void FormatTo(StringWriter &output, StringView spec) const
    {
        FormatComponents(output, &R, 4, spec);
    }
};

inline constexpr LinearColor LinearColor::Black{0, 0, 0, 1};
inline constexpr LinearColor LinearColor::White{1, 1, 1, 1};
inline constexpr LinearColor LinearColor::Transparent{0, 0, 0, 0};

/// An 8-bit RGBA color, usually sRGB-encoded.
///
/// The bytes are in the order of an R8G8B8A8 texture. `LinearColor::FromSrgb` and `LinearColor::ToSrgb` convert
/// between the two types.
///
/// # Examples
///
/// ```cpp
/// const Color32 color = Color32::FromHex(0x11223344); // -> (0x11, 0x22, 0x33, 0x44)
/// const uint32 hex = color.ToHex(); // -> 0x11223344
/// ```
MSTRUCT(Tuple)
struct Color32
{
    MGENERATED_BODY()

    /// The red channel, from 0 to 255; defaults to 0.
    MPROPERTY()
    uint8 R = 0;
    /// The green channel, from 0 to 255; defaults to 0.
    MPROPERTY()
    uint8 G = 0;
    /// The blue channel, from 0 to 255; defaults to 0.
    MPROPERTY()
    uint8 B = 0;
    /// The alpha channel, from 0 (transparent) to 255 (opaque); defaults to 255.
    MPROPERTY()
    uint8 A = 255;

    /// Creates opaque black.
    constexpr Color32() = default;

    /// Creates a color from `r`, `g`, `b` and `a`; `a` defaults to 255.
    constexpr Color32(uint8 r, uint8 g, uint8 b, uint8 a = 255) : R(r), G(g), B(b), A(a)
    {
    }

    /// Creates a color from a `0xRRGGBBAA` value.
    [[nodiscard]] static constexpr Color32 FromHex(uint32 rgba)
    {
        return {static_cast<uint8>(rgba >> 24), static_cast<uint8>(rgba >> 16), static_cast<uint8>(rgba >> 8),
                static_cast<uint8>(rgba)};
    }

    /// Returns the color as a `0xRRGGBBAA` value.
    [[nodiscard]] constexpr uint32 ToHex() const
    {
        return (static_cast<uint32>(R) << 24) | (static_cast<uint32>(G) << 16) | (static_cast<uint32>(B) << 8) | A;
    }

    /// Compares all four channels exactly.
    constexpr bool operator==(const Color32 &) const = default;
};

inline LinearColor LinearColor::FromSrgb(Color32 color)
{
    constexpr float32 Scale = 1 / 255.0f;
    return {SrgbToLinear(static_cast<float32>(color.R) * Scale), SrgbToLinear(static_cast<float32>(color.G) * Scale),
            SrgbToLinear(static_cast<float32>(color.B) * Scale), static_cast<float32>(color.A) * Scale};
}

inline Color32 LinearColor::ToSrgb() const
{
    const auto encode = [](float32 value) {
        return static_cast<uint8>(Math::Round(Math::Saturate(LinearToSrgb(Math::Saturate(value))) * 255.0f));
    };
    return {encode(R), encode(G), encode(B), static_cast<uint8>(Math::Round(Math::Saturate(A) * 255.0f))};
}

inline LinearColor LinearColor::FromHsv(float32 hue, float32 saturation, float32 value, float32 alpha)
{
    const float32 sector = Math::Mod(hue < 0 ? hue + 360 : hue, 360.0f) / 60.0f;
    const float32 chroma = value * saturation;
    const float32 x = chroma * (1 - Math::Abs(Math::Mod(sector, 2.0f) - 1));
    const float32 m = value - chroma;
    float32 r = 0;
    float32 g = 0;
    float32 b = 0;
    switch (static_cast<int32>(sector))
    {
    case 0:
        r = chroma, g = x;
        break;
    case 1:
        r = x, g = chroma;
        break;
    case 2:
        g = chroma, b = x;
        break;
    case 3:
        g = x, b = chroma;
        break;
    case 4:
        r = x, b = chroma;
        break;
    default:
        r = chroma, b = x;
        break;
    }
    return {r + m, g + m, b + m, alpha};
}
} // namespace Marlin
