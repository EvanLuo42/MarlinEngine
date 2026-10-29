#pragma once

#include "TexturePipeline/TextureEncoder.h"
#include "TextureTestTypes.gen.h"

namespace Marlin::Tests
{
/// Claims every compressed format ahead of any real encoder and writes each block as the first texel of its strip
/// repeated, so a test can see what the converter handed over and where the blocks went.
// RECOVERY-DECISION TM-D7: priority 1000, far above any real encoder's.
MCLASS(Priority = 1000)
class TestTextureEncoder final : public TextureEncoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const TextureFormat> GetFormats() const override
    {
        static constexpr TextureFormat Formats[] = {
            TextureFormat::BC1,     TextureFormat::BC3,     TextureFormat::BC4,
            TextureFormat::BC5,     TextureFormat::BC7,     TextureFormat::BC6H,
            TextureFormat::ASTC4x4, TextureFormat::ASTC6x6, TextureFormat::ASTC8x8};
        return Formats;
    }

    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }

    bool Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter & /*error*/) const override
    {
        const usize texel = TextureFormats::IsFloat(input.Format) ? 16 : 4;
        for (usize i = 0; i < blocks.Count(); ++i)
        {
            blocks[i] = input.Texels[i % texel];
        }
        return true;
    }
};
} // namespace Marlin::Tests
