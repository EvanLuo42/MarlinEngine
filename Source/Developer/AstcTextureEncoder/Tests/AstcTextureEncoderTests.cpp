#include "Testing/Testing.h"

#include "AstcTextureEncoder/AstcTextureEncoder.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
constexpr uint32 Width = 52;
constexpr uint32 Height = 36;

/// Red ramps along x, green along y and blue against it, alpha opaque: smooth enough for every block size, and sized
/// so 6x6 and 8x8 blocks overhang the right and bottom edges.
List<uint8> Gradient()
{
    List<uint8> texels;
    for (uint32 y = 0; y < Height; ++y)
    {
        for (uint32 x = 0; x < Width; ++x)
        {
            texels.Add(static_cast<uint8>(x * 255 / (Width - 1)));
            texels.Add(static_cast<uint8>(y * 255 / (Height - 1)));
            texels.Add(static_cast<uint8>(255 - y * 255 / (Height - 1)));
            texels.Add(255);
        }
    }
    return texels;
}

/// Encodes the gradient to *format*, decodes it back and returns the mean absolute error per channel, in 8-bit steps.
float32 RoundTripError(const AstcTextureEncoder &encoder, TextureFormat format, bool srgb)
{
    const List<uint8> texels = Gradient();
    uint32 rowPitch = 0;
    uint64 size = 0;
    TextureFormats::GetMipLayout(format, Width, Height, rowPitch, size);
    List<uint8> blocks;
    blocks.Resize(static_cast<usize>(size));

    TextureEncodeInput input;
    input.Format = format;
    input.Width = Width;
    input.Height = Height;
    input.Texels = texels.AsSpan();
    input.Srgb = srgb;
    StringBuilder<> error;
    MARLIN_REQUIRE(encoder.Encode(input, blocks.AsSpan(), error));

    List<uint8> decoded;
    decoded.Resize(texels.Count());
    MARLIN_REQUIRE(AstcTextureEncoder::Decode(format, blocks.AsSpan(), Width, Height, srgb, decoded.AsSpan()));
    float32 total = 0;
    for (usize i = 0; i < texels.Count(); ++i)
    {
        total += static_cast<float32>(texels[i] > decoded[i] ? texels[i] - decoded[i] : decoded[i] - texels[i]);
    }
    return total / static_cast<float32>(texels.Count());
}
} // namespace

MARLIN_TEST(AstcTextureEncoder, RoundTrip)
{
    const AstcTextureEncoder encoder;
    MARLIN_EXPECT(RoundTripError(encoder, TextureFormat::ASTC4x4, true) < 1.5f);
    MARLIN_EXPECT(RoundTripError(encoder, TextureFormat::ASTC4x4, false) < 1.5f);
    MARLIN_EXPECT(RoundTripError(encoder, TextureFormat::ASTC6x6, false) < 3.0f);
    MARLIN_EXPECT(RoundTripError(encoder, TextureFormat::ASTC8x8, false) < 4.0f);
    // A second encode with the same settings reuses the pooled context.
    MARLIN_EXPECT(RoundTripError(encoder, TextureFormat::ASTC4x4, false) < 1.5f);

    const List<uint8> texels = Gradient();
    List<uint8> blocks;
    blocks.Resize(4096);
    TextureEncodeInput input;
    input.Format = TextureFormat::BC7;
    input.Width = Width;
    input.Height = Height;
    input.Texels = texels.AsSpan();
    StringBuilder<> error;
    MARLIN_EXPECT(!encoder.Encode(input, blocks.AsSpan(), error));
    MARLIN_EXPECT(error.AsView() == "astcenc takes RGBA8 texels of ASTC");
}
} // namespace Marlin::Tests
