//! CPU topology and instruction-set detection for the current processor group.

#pragma once

#include "CoreTypes.h"
#include "HAL/PlatformArch.h"

namespace Marlin
{
/// The processor layout reported by [`PlatformCpu::GetTopology`].
struct CpuTopology
{
    /// The maximum number of physical cores the struct can describe.
    static constexpr uint32 MaxCores = 64;

    /// The number of logical processors.
    uint32 LogicalCount = 0;
    /// The number of physical cores.
    uint32 PhysicalCount = 0;
    /// The logical processor mask of each physical core; only the first `PhysicalCount` entries are used.
    uint64 CoreMasks[MaxCores] = {};
};

namespace PlatformCpu
{
/// Returns the topology of the current processor group.
///
/// Covers at most [`CpuTopology::MaxCores`] logical processors; on Windows, only those of the current processor
/// group. The call allocates a temporary OS buffer.
CORE_API CpuTopology GetTopology();
/// Writes the CPU brand string into `buffer` and returns its length.
///
/// The terminator is written only when the length is less than `capacity`; returns zero when the CPU does not
/// report a brand string.
CORE_API usize GetBrand(char *buffer, usize capacity);
/// Returns whether the CPU and OS support the instruction set the engine is compiled for.
///
/// Checks CPUID and the OS's XSAVE state for the baseline x64 features, including AVX2 and BMI.
CORE_API bool SupportsRequiredIsa();
} // namespace PlatformCpu
} // namespace Marlin
