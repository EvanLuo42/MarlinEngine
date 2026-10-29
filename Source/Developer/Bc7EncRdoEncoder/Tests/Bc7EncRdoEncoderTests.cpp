#include "Testing/Testing.h"

#include "Bc7EncRdoEncoder/Bc7EncRdoEncoder.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
constexpr uint32 Width = 64;
constexpr uint32 Height = 44;

/// Red ramps along x, green along y, blue against green and alpha along the diagonal.
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
            texels.Add(static_cast<uint8>((x + y) * 255 / (Width + Height - 2)));
        }
    }
    return texels;
}

/// The blocks of a Width by Height *format* image of *texels*.
List<uint8> Encode(TextureFormat format, Span<const uint8> texels, float32 quality, float32 lambda)
{
    uint32 rowPitch = 0;
    uint64 size = 0;
    TextureFormats::GetMipLayout(format, Width, Height, rowPitch, size);
    List<uint8> blocks;
    blocks.Resize(static_cast<usize>(size));
    TextureEncodeInput input;
    input.Format = format;
    input.Width = Width;
    input.Height = Height;
    input.Texels = texels;
    input.Quality = quality;
    input.RdoLambda = lambda;
    StringBuilder<> error;
    MARLIN_REQUIRE(Bc7EncRdoEncoder().Encode(input, blocks.AsSpan(), error));
    return blocks;
}

/// The mean absolute error of the first *channels* channels of *blocks* decoded against *texels*, in 8-bit steps.
float32 MeanError(TextureFormat format, Span<const uint8> blocks, Span<const uint8> texels, uint32 channels)
{
    List<uint8> decoded;
    decoded.Resize(texels.Count());
    MARLIN_REQUIRE(Bc7EncRdoEncoder::Decode(format, blocks, Width, Height, decoded.AsSpan()));
    float32 total = 0;
    for (usize texel = 0; texel < texels.Count() / 4; ++texel)
    {
        for (uint32 c = 0; c < channels; ++c)
        {
            const uint8 a = texels[texel * 4 + c];
            const uint8 b = decoded[texel * 4 + c];
            total += static_cast<float32>(a > b ? a - b : b - a);
        }
    }
    return total / static_cast<float32>(texels.Count() / 4 * channels);
}
} // namespace

// RECOVERY-DECISION ENC-D6: the error bounds; only the shape of the test survived.
MARLIN_TEST(Bc7EncRdoEncoder, Formats)
{
    const List<uint8> texels = Gradient();
    MARLIN_EXPECT(MeanError(TextureFormat::BC1, Encode(TextureFormat::BC1, texels.AsSpan(), 0.5f, 0).AsSpan(),
                            texels.AsSpan(), 3) < 6.0f);
    MARLIN_EXPECT(MeanError(TextureFormat::BC3, Encode(TextureFormat::BC3, texels.AsSpan(), 0.5f, 0).AsSpan(),
                            texels.AsSpan(), 4) < 6.0f);
    MARLIN_EXPECT(MeanError(TextureFormat::BC4, Encode(TextureFormat::BC4, texels.AsSpan(), 0.5f, 0).AsSpan(),
                            texels.AsSpan(), 1) < 2.0f);
    MARLIN_EXPECT(MeanError(TextureFormat::BC5, Encode(TextureFormat::BC5, texels.AsSpan(), 0.5f, 0).AsSpan(),
                            texels.AsSpan(), 2) < 2.0f);
    MARLIN_EXPECT(MeanError(TextureFormat::BC7, Encode(TextureFormat::BC7, texels.AsSpan(), 1.0f, 0).AsSpan(),
                            texels.AsSpan(), 4) < 3.0f);

    // A mip smaller than a block takes one whole block; a buffer of the wrong size is refused.
    TextureEncodeInput input;
    input.Format = TextureFormat::BC7;
    input.Width = 3;
    input.Height = 3;
    input.Texels = texels.AsSpan().Slice(0, 3 * 3 * 4);
    List<uint8> small;
    small.Resize(16);
    StringBuilder<> error;
    MARLIN_EXPECT(Bc7EncRdoEncoder().Encode(input, small.AsSpan(), error));
    small.Resize(8);
    MARLIN_EXPECT(!Bc7EncRdoEncoder().Encode(input, small.AsSpan(), error) && error.AsView() == "bc7enc_rdo failed");
}

MARLIN_TEST(Bc7EncRdoEncoder, RateDistortion)
{
    List<uint8> texels = Gradient();
    uint32 state = 12345;
    for (uint8 &value : texels)
    {
        state = state * 1664525u + 1013904223u;
        value = static_cast<uint8>(value / 2 + (state >> 26));
    }
    const List<uint8> plain = Encode(TextureFormat::BC7, texels.AsSpan(), 0.5f, 0);
    const List<uint8> rdo = Encode(TextureFormat::BC7, texels.AsSpan(), 0.5f, 2.0f);
    MARLIN_EXPECT(MemCompare(plain.Data(), rdo.Data(), plain.Count()) != 0);

    // ert copies bytes from the blocks just before, so more bytes repeat at the same place in a recent block.
    const auto repeats = [](const List<uint8> &blocks) {
        usize count = 0;
        for (usize i = 16; i < blocks.Count(); ++i)
        {
            bool found = false;
            for (usize back = 16; back <= 8 * 16 && back <= i && !found; back += 16)
            {
                found = blocks[i] == blocks[i - back];
            }
            count += found ? 1 : 0;
        }
        return count;
    };
    MARLIN_EXPECT(repeats(rdo) > repeats(plain));
    MARLIN_EXPECT(MeanError(TextureFormat::BC7, rdo.AsSpan(), texels.AsSpan(), 4) < 12.0f);
}
} // namespace Marlin::Tests
