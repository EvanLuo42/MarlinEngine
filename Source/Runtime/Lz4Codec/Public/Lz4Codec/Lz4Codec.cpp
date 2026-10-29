#include "Lz4Codec/Lz4Codec.h"

#include <lz4.h>
#include <lz4hc.h>

#include "Concurrency/ThreadLocal.h"
#include "Memory/Memory.h"

namespace Marlin
{
namespace
{
/// A thread's LZ4-HC state, allocated on its first HC compression and reused: LZ4_compress_HC allocates one per call.
struct Lz4HcState
{
    void *State = nullptr;
};

ThreadLocal<Lz4HcState> HcStates;
} // namespace

bool Lz4Codec::Compress(Span<const uint8> input, int32 level, List<uint8> &output) const
{
    if (input.Count() > LZ4_MAX_INPUT_SIZE)
    {
        return false;
    }
    const int size = static_cast<int>(input.Count());
    const int bound = LZ4_compressBound(size);
    const usize start = output.Count();
    output.ResizeUninitialized(start + static_cast<usize>(bound));
    const auto *source = reinterpret_cast<const char *>(input.Data());
    auto *target = reinterpret_cast<char *>(output.Data() + start);
    int written = 0;
    if (level <= 0)
    {
        written = LZ4_compress_default(source, target, size, bound);
    }
    else
    {
        // LZ4_compress_HC is LZ4_compress_HC_extStateHC on a state it allocates: the same output.
        Lz4HcState &state = HcStates.Get();
        if (state.State == nullptr)
        {
            state.State = Memory::Allocate(static_cast<usize>(LZ4_sizeofStateHC()), alignof(void *));
        }
        written = LZ4_compress_HC_extStateHC(state.State, source, target, size, bound, level);
    }
    if (written <= 0 || static_cast<usize>(written) >= input.Count())
    {
        output.Resize(start);
        return false;
    }
    output.Resize(start + static_cast<usize>(written));
    return true;
}

bool Lz4Codec::Decompress(Span<const uint8> input, Span<uint8> output) const
{
    if (input.Count() > LZ4_MAX_INPUT_SIZE || output.Count() > LZ4_MAX_INPUT_SIZE)
    {
        return false;
    }
    const int read =
        LZ4_decompress_safe(reinterpret_cast<const char *>(input.Data()), reinterpret_cast<char *>(output.Data()),
                            static_cast<int>(input.Count()), static_cast<int>(output.Count()));
    return read == static_cast<int>(output.Count());
}
} // namespace Marlin
