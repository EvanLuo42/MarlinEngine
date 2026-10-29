//! Virtual memory: reserving address space, committing it, changing page access, and the C runtime heap.
//!
//! `Memory` and the heaps build on these calls. A reservation borrows address space only; `Commit` backs pages
//! with physical memory and zeroes them.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// The access mode that `Protect` gives a range.
enum class PageAccess : uint8
{
    /// Any access faults.
    None,
    /// Reads succeed and writes fault.
    ReadOnly,
    /// Reads and writes succeed.
    ReadWrite,
};

/// The process's memory geometry and the machine's physical memory totals, in bytes.
struct PlatformMemoryInfo
{
    /// The smallest unit that `Commit`, `Decommit` and `Protect` act on.
    usize PageSize;
    /// Alignment of reserved address space.
    usize AllocationGranularity;
    /// Zero if the OS has no large pages.
    usize LargePageSize;
    /// Physical memory installed in the machine.
    uint64 TotalPhysical;
    /// Physical memory currently available to processes.
    uint64 AvailablePhysical;
};

/// Reserves, commits and protects virtual memory.
///
/// `Commit`, `Decommit` and `Protect` take page-aligned ranges.
namespace PlatformMemory
{
/// Returns the current memory geometry and physical memory totals.
CORE_API PlatformMemoryInfo GetInfo();

/// Reserves `size` bytes of address space without committing it.
///
/// `alignment` is the required alignment of the returned address, or zero for the allocation granularity. The
/// address space is not backed by memory until `Commit`.
///
/// # Preconditions
/// An `alignment` above the allocation granularity is a power of two; the check is an assertion.
///
/// # Errors
/// Returns null when the address space cannot be reserved, including when the platform cannot honor `alignment`.
///
/// # Examples
///
/// ```cpp
/// void *arena = PlatformMemory::Reserve(64 * 1024 * 1024, 2 * 1024 * 1024); // -> a 64 MiB reservation
/// if (arena != nullptr && PlatformMemory::Commit(arena, 64 * 1024 * 1024))
/// {
///     // The first 64 MiB of the reservation now read as zero.
/// }
/// PlatformMemory::Release(arena, 64 * 1024 * 1024);
/// ```
CORE_API void *Reserve(usize size, usize alignment = 0);
/// Releases the reservation that starts at `address`.
///
/// `size` is the size that was reserved; a null `address` is ignored. Every address in the reservation becomes
/// invalid.
CORE_API void Release(void *address, usize size);

/// Backs the pages of a reservation with zeroed read-write memory.
///
/// Committing pages that are already committed succeeds and keeps their contents.
///
/// # Errors
/// Returns false when physical memory or the commit limit is exhausted.
CORE_API bool Commit(void *address, usize size);
/// Returns the pages of a range to the OS, dropping their contents.
///
/// The range may include pages that are not committed, as long as it lies in one reservation. A later `Commit`
/// backs the pages with zeroed memory again.
CORE_API void Decommit(void *address, usize size);

/// Sets the access mode of a page-aligned range.
///
/// # Preconditions
/// The range lies in one reservation and is committed.
///
/// # Errors
/// Returns false when the OS rejects the change.
CORE_API bool Protect(void *address, usize size, PageAccess access);

/// Allocates `size` bytes from the C runtime heap, which AddressSanitizer instruments.
///
/// `Memory` uses this heap in place of the engine heap in instrumented builds.
///
/// # Errors
/// Returns null when out of memory.
CORE_API void *SystemAllocate(usize size);
/// Frees a block from `SystemAllocate`.
///
/// A null pointer is accepted and ignored.
CORE_API void SystemFree(void *pointer);
} // namespace PlatformMemory
} // namespace Marlin
