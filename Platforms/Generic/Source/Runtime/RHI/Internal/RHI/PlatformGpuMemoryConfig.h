#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// GPU memory allocator tuning; each platform layer may override this header.
struct PlatformGpuMemoryConfig
{
    /// Size of the blocks GPU-local resources are suballocated from.
    static constexpr uint64 DeviceBlockSize = uint64{256} * 1024 * 1024;
    /// Size of the blocks of upload and readback memory.
    static constexpr uint64 HostBlockSize = uint64{64} * 1024 * 1024;
    /// Resources at least this large get memory of their own instead of a piece of a block.
    static constexpr uint64 DedicatedThreshold = uint64{64} * 1024 * 1024;
    /// The smallest piece of a block, and the alignment of every piece.
    static constexpr uint64 Granularity = 256;
    /// Empty blocks each pool keeps for reuse instead of returning them to the driver.
    static constexpr uint32 RetainedEmptyBlocks = 1;
};
} // namespace Marlin
