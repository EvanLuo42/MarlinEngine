#include "ZstdCodec/ZstdCodec.h"

#include <zstd.h>

#include "Concurrency/ThreadLocal.h"

namespace Marlin
{
namespace
{
/// A thread's zstd contexts, made on its first call and reused, so a call does not build zstd's tables again.
struct ZstdContexts
{
    ZSTD_CCtx *Compress = nullptr;
    ZSTD_DCtx *Decompress = nullptr;
};

ThreadLocal<ZstdContexts> Contexts;
} // namespace

bool ZstdCodec::Compress(Span<const uint8> input, int32 level, List<uint8> &output) const
{
    const usize bound = ZSTD_compressBound(input.Count());
    const usize start = output.Count();
    output.ResizeUninitialized(start + bound);
    ZstdContexts &contexts = Contexts.Get();
    if (contexts.Compress == nullptr)
    {
        contexts.Compress = ZSTD_createCCtx();
    }
    // ZSTD_compressCCtx compresses exactly as ZSTD_compress does, at the level alone.
    const int32 effective = level != 0 ? level : ZSTD_CLEVEL_DEFAULT;
    const usize written =
        contexts.Compress != nullptr
            ? ZSTD_compressCCtx(contexts.Compress, output.Data() + start, bound, input.Data(), input.Count(), effective)
            : ZSTD_compress(output.Data() + start, bound, input.Data(), input.Count(), effective);
    if (ZSTD_isError(written) != 0 || written >= input.Count())
    {
        output.Resize(start);
        return false;
    }
    output.Resize(start + written);
    return true;
}

bool ZstdCodec::Decompress(Span<const uint8> input, Span<uint8> output) const
{
    ZstdContexts &contexts = Contexts.Get();
    if (contexts.Decompress == nullptr)
    {
        contexts.Decompress = ZSTD_createDCtx();
    }
    const usize read =
        contexts.Decompress != nullptr
            ? ZSTD_decompressDCtx(contexts.Decompress, output.Data(), output.Count(), input.Data(), input.Count())
            : ZSTD_decompress(output.Data(), output.Count(), input.Data(), input.Count());
    return ZSTD_isError(read) == 0 && read == output.Count();
}
} // namespace Marlin
