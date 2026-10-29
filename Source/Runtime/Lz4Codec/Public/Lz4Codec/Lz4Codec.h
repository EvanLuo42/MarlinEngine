#pragma once

#include "Compression/CompressionCodec.h"
#include "CoreTypes.h"
#include "Lz4Codec/Lz4Codec.gen.h"

namespace Marlin
{
/// LZ4 (the lz4 library): decompresses at several GB/s on one core. Level 0 is the fast compressor; levels 1 to 12
/// are LZ4HC, slower to compress for smaller packages and the same decompression speed.
MCLASS()
class LZ4CODEC_API Lz4Codec final : public CompressionCodec
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "Lz4";
    }
    bool Compress(Span<const uint8> input, int32 level, List<uint8> &output) const override;
    bool Decompress(Span<const uint8> input, Span<uint8> output) const override;
};
} // namespace Marlin
