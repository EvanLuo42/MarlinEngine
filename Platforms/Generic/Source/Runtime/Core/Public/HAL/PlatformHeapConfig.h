#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// MarlinHeap tuning; each platform layer may override this header.
struct PlatformHeapConfig
{
    /// Address space reserved at startup and cut into segments; zero reserves each segment from the OS.
    static constexpr usize ArenaReserve = usize{16} * 1024 * 1024 * 1024;
    /// Alignment and granularity of heap segments; a power of two.
    static constexpr usize SegmentSize = usize{2} * 1024 * 1024;
    /// Slab page of the small tier; divides SegmentSize.
    static constexpr usize SmallPageSize = usize{64} * 1024;
    /// Empty segments always kept committed for reuse.
    static constexpr usize RetainedBytes = usize{32} * 1024 * 1024;
    /// Empty segments beyond RetainedBytes stay committed this long before going back to the OS.
    static constexpr uint32 PurgeDelayMilliseconds = 100;
    /// Commit a small segment's pages one at a time instead of all at once.
    static constexpr bool CommitOnDemand = true;
};
} // namespace Marlin
