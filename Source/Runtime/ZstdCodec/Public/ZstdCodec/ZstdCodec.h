#pragma once

#include "Compression/CompressionCodec.h"
#include "CoreTypes.h"
#include "ZstdCodec/ZstdCodec.gen.h"

namespace Marlin
{
/// Zstandard (the zstd library): about a third smaller than Lz4 and roughly a third of its decompression speed.
/// Level 0 is zstd's default (3); levels run from 1 to 22, the high ones slow to compress only.
MCLASS()
class ZSTDCODEC_API ZstdCodec final : public CompressionCodec
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "Zstd";
    }
    bool Compress(Span<const uint8> input, int32 level, List<uint8> &output) const override;
    bool Decompress(Span<const uint8> input, Span<uint8> output) const override;
};
} // namespace Marlin
