#include "Testing/Testing.h"

#include "IspcTextureEncoder/IspcTextureEncoder.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// Encodes a *width* by *height* image of RGB *value* and alpha 1 to BC6H into *blocks*.
bool EncodeSolid(uint32 width, uint32 height, float32 value, List<uint8> &blocks)
{
    List<float32> texels;
    for (uint32 i = 0; i < width * height; ++i)
    {
        texels.Add(value);
        texels.Add(value);
        texels.Add(value);
        texels.Add(1.0f);
    }
    blocks.Resize(usize{(width + 3) / 4} * ((height + 3) / 4) * 16);
    TextureEncodeInput input;
    input.Format = TextureFormat::BC6H;
    input.Width = width;
    input.Height = height;
    input.Texels = AsBytes(texels.AsSpan());
    StringBuilder<> error;
    return IspcTextureEncoder().Encode(input, blocks.AsSpan(), error);
}
} // namespace

// RECOVERY-DECISION ENC-D5: no library here decodes BC6H, so the test checks the encoder by equal and different
// blocks: solid images give identical blocks, edge padding repeats texels, and out-of-range values clamp.
MARLIN_TEST(IspcTextureEncoder, BC6H)
{
    List<uint8> one;
    MARLIN_REQUIRE(EncodeSolid(4, 4, 1.0f, one));
    List<uint8> two;
    MARLIN_REQUIRE(EncodeSolid(4, 4, 2.0f, two));
    MARLIN_EXPECT(MemCompare(one.Data(), two.Data(), 16) != 0);

    // A 6x5 image overhangs into 2x2 blocks; repeating its edges keeps every block the solid one.
    List<uint8> padded;
    MARLIN_REQUIRE(EncodeSolid(6, 5, 1.0f, padded));
    MARLIN_REQUIRE(padded.Count() == 64);
    for (usize block = 0; block < 4; ++block)
    {
        MARLIN_EXPECT(MemCompare(padded.Data() + block * 16, one.Data(), 16) == 0);
    }

    List<uint8> zero;
    MARLIN_REQUIRE(EncodeSolid(4, 4, 0.0f, zero));
    List<uint8> negative;
    MARLIN_REQUIRE(EncodeSolid(4, 4, -3.0f, negative));
    MARLIN_EXPECT(MemCompare(zero.Data(), negative.Data(), 16) == 0);
    List<uint8> largest;
    MARLIN_REQUIRE(EncodeSolid(4, 4, 65504.0f, largest));
    List<uint8> huge;
    MARLIN_REQUIRE(EncodeSolid(4, 4, 1.0e9f, huge));
    MARLIN_EXPECT(MemCompare(largest.Data(), huge.Data(), 16) == 0);

    const float32 texels[16 * 4] = {};
    TextureEncodeInput input;
    input.Format = TextureFormat::BC6H;
    input.Width = 4;
    input.Height = 4;
    input.Texels = AsBytes(Span<const float32>(texels));
    List<uint8> wrong;
    wrong.Resize(8);
    StringBuilder<> error;
    MARLIN_EXPECT(!IspcTextureEncoder().Encode(input, wrong.AsSpan(), error));
    MARLIN_EXPECT(error.AsView() == "the block buffer does not fit the mip");
}
} // namespace Marlin::Tests
