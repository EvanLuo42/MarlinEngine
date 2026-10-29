//! Heap tuning for the Windows platform layer.
//!
//! Windows reserves address space cheaply and commits 4 KiB pages against the commit limit, so the heap reserves a
//! large arena up front, commits on demand, and keeps freed segments committed for a while for reuse.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// Geometry and retention settings for the engine heap on Windows.
struct PlatformHeapConfig
{
    /// Address space reserved at startup and cut into segments, in bytes. Zero reserves each segment from the OS.
    static constexpr usize ArenaReserve = usize{256} * 1024 * 1024 * 1024;
    /// Alignment and granularity of a heap segment, in bytes; a power of two.
    static constexpr usize SegmentSize = usize{2} * 1024 * 1024;
    /// Size of a small-tier slab page, in bytes; it divides `SegmentSize`.
    static constexpr usize SmallPageSize = usize{64} * 1024;
    /// Committed bytes kept for reuse before empty segments are decommitted.
    static constexpr usize RetainedBytes = usize{64} * 1024 * 1024;
    /// How long an empty segment beyond `RetainedBytes` stays committed before its pages go back to the OS, in
    /// milliseconds.
    static constexpr uint32 PurgeDelayMilliseconds = 250;
    /// Whether a new small segment commits its pages one at a time instead of all at once.
    static constexpr bool CommitOnDemand = true;
};
} // namespace Marlin
