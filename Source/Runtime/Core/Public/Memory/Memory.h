//! The process heap: allocation, tag accounting and statistics.
//!
//! Engine allocations go through `Memory::Allocate` or the global operators in `MemoryOperators.inl`. The heap has
//! no shutdown step; its pages are returned to the OS when the process exits.

#pragma once

#include "CoreTypes.h"
#include "Hash/RapidHash.h"

#ifndef MARLIN_MEMORY_TAGS
/// Whether allocations are counted under a [`Marlin::MemoryTag`].
///
/// Defaults to `!MARLIN_BUILD_SHIPPING`. Define it to 0 to compile tagging away; the tag functions then do nothing and
/// no per-tag counters are kept.
#define MARLIN_MEMORY_TAGS (!MARLIN_BUILD_SHIPPING)
#endif

namespace Marlin
{
/// The alignment used by [`Memory::Allocate`] and by `operator new` without an explicit alignment.
///
/// Requests for a smaller alignment are raised to this value.
inline constexpr usize DefaultAlignment = 16;

/// A category that memory is counted under.
///
/// Tags are open: any module can define one. Equality compares the hash of the name, so two tags constructed from the
/// same text are the same tag. The predefined tags are the categories the engine counts separately.
struct MemoryTag
{
    /// The hash of `Name`, used for equality and for the heap's tag table.
    uint64 Id;
    /// The name shown in reports; it must outlive the tag.
    const char *Name;

    /// Creates a tag from `name`, which must outlive the tag.
    constexpr explicit MemoryTag(const char *name) : Id(RapidHash(name, __builtin_strlen(name))), Name(name)
    {
    }

    /// Returns whether both tags have the same name hash.
    constexpr bool operator==(const MemoryTag &other) const
    {
        return Id == other.Id;
    }

    /// Allocations with no tag.
    static const MemoryTag Untagged;
    /// Allocations made by the core runtime.
    static const MemoryTag Core;
    /// Allocations made by the job system.
    static const MemoryTag Jobs;
    /// Allocations made by the containers.
    static const MemoryTag Containers;
    /// Allocations made by strings and string builders.
    static const MemoryTag Strings;
    /// Allocations made by assets and the asset manager.
    static const MemoryTag Assets;
    /// Allocations made by the world and ECS.
    static const MemoryTag World;
    /// Allocations made by the renderer and RHI.
    static const MemoryTag Rendering;
    /// Allocations made by physics.
    static const MemoryTag Physics;
    /// Allocations made by animation.
    static const MemoryTag Animation;
    /// Allocations made by audio.
    static const MemoryTag Audio;
    /// Allocations made by networking.
    static const MemoryTag Network;
    /// Allocations made by scripts and the script VM.
    static const MemoryTag Scripts;
    /// Allocations made by the UI.
    static const MemoryTag UI;
    /// Allocations made by the editor.
    static const MemoryTag Editor;
    /// Allocations made by third-party code.
    static const MemoryTag ThirdParty;
};

inline constexpr MemoryTag MemoryTag::Untagged{"Untagged"};
inline constexpr MemoryTag MemoryTag::Core{"Core"};
inline constexpr MemoryTag MemoryTag::Jobs{"Jobs"};
inline constexpr MemoryTag MemoryTag::Containers{"Containers"};
inline constexpr MemoryTag MemoryTag::Strings{"Strings"};
inline constexpr MemoryTag MemoryTag::Assets{"Assets"};
inline constexpr MemoryTag MemoryTag::World{"World"};
inline constexpr MemoryTag MemoryTag::Rendering{"Rendering"};
inline constexpr MemoryTag MemoryTag::Physics{"Physics"};
inline constexpr MemoryTag MemoryTag::Animation{"Animation"};
inline constexpr MemoryTag MemoryTag::Audio{"Audio"};
inline constexpr MemoryTag MemoryTag::Network{"Network"};
inline constexpr MemoryTag MemoryTag::Scripts{"Scripts"};
inline constexpr MemoryTag MemoryTag::UI{"UI"};
inline constexpr MemoryTag MemoryTag::Editor{"Editor"};
inline constexpr MemoryTag MemoryTag::ThirdParty{"ThirdParty"};

/// The counters of one tag at the time of a query.
struct MemoryTagStats
{
    /// The tag's name, or null for an unused slot.
    const char *Name = nullptr;
    /// The hash of `Name`.
    uint64 Id = 0;
    /// Bytes in use, including size-class rounding.
    usize Current = 0;
    /// The highest `Current` reached, in bytes.
    usize Peak = 0;
    /// The budget in bytes, or zero for no budget.
    usize Budget = 0;
    /// Allocations made under the tag; frees do not decrement it.
    uint64 Allocations = 0;
};

/// Process-wide heap totals at the time of a query.
struct HeapStats
{
    /// Bytes reserved from the OS.
    usize Reserved = 0;
    /// Bytes committed to physical memory.
    usize Committed = 0;
    /// Bytes in use in the small size classes.
    usize SmallInUse = 0;
    /// Bytes in use in the medium size classes.
    usize MediumInUse = 0;
    /// Bytes in use by large allocations.
    usize LargeInUse = 0;
};

/// One distinct call stack of the allocations alive when a snapshot was taken.
struct AllocationSite
{
    /// The maximum number of frames recorded per site.
    static constexpr uint32 MaxFrames = 24;

    /// The return addresses of the stack, innermost first.
    void *Frames[MaxFrames] = {};
    /// The number of valid entries in `Frames`.
    uint32 FrameCount = 0;
    /// The heap's internal slot for the tag the allocations were made under.
    uint16 TagSlot = 0;
    /// Live bytes allocated at this site.
    usize Bytes = 0;
    /// Live allocations made at this site.
    uint64 Allocations = 0;
};

/// Called once per distinct allocation site by [`Memory::VisitAllocationSites`].
using AllocationSiteVisitor = void (*)(const AllocationSite &site, void *context);
/// Called when a tag first exceeds its budget.
using MemoryBudgetHandler = void (*)(const MemoryTagStats &stats);

/// The process heap: thread-cached slabs up to 1 KiB, TLSF up to 1 MiB, and OS pages above.
///
/// # Thread safety
/// Allocation and free are safe from any thread. The heap is initialized on first use.
namespace Memory
{
/// Allocates `size` bytes with `alignment` and counts them under the current tag.
///
/// Never returns null; running out of memory is fatal. `alignment` must be a power of two and is raised to
/// [`DefaultAlignment`] when smaller. The contents are uninitialized.
///
/// # Thread safety
/// Safe to call from any thread.
CORE_API void *Allocate(usize size, usize alignment = DefaultAlignment);
/// Resizes an allocation, keeping the contents up to the smaller of the old and new size.
///
/// A null `pointer` allocates. When the existing block already fits, is aligned, and `size` is at least half of its
/// usable size, the same pointer is returned; otherwise a new block is allocated, the contents are copied and the
/// old block is freed. Under AddressSanitizer the block is always reallocated.
///
/// # Preconditions
/// `pointer` is null or was returned by `Allocate` or `Reallocate`, and `alignment` is a power of two.
CORE_API void *Reallocate(void *pointer, usize size, usize alignment = DefaultAlignment);
/// Frees an allocation.
///
/// Accepts null. Freeing a pointer the heap did not allocate is fatal.
///
/// # Preconditions
/// `pointer` is null or was returned by `Allocate` or `Reallocate`.
CORE_API void Free(void *pointer);
/// Returns the usable size of an allocation, which is at least the requested size.
///
/// Returns zero for null. Size-class rounding can make the result larger than the request.
///
/// # Preconditions
/// `pointer` is null or was returned by `Allocate` or `Reallocate`.
CORE_API usize GetAllocationSize(const void *pointer);
/// Returns free pages to the OS.
///
/// Does nothing under AddressSanitizer.
CORE_API void Trim();
/// Returns the process heap's totals.
///
/// The calling thread's pending counters are flushed first; counters that other threads have not flushed yet are not
/// included, so the result can lag behind them.
CORE_API HeapStats GetHeapStats();

/// Pushes `tag` for the allocations of the current execution context.
///
/// The tag applies until the matching `PopTag`. Pushes nest, and the innermost tag wins. Does nothing when
/// `MARLIN_MEMORY_TAGS` is 0.
///
/// # Preconditions
/// When tagging is enabled, the context's tag stack is not full. The depth is checked by an assertion in builds with
/// assertions.
CORE_API void PushTag(MemoryTag tag);
/// Pops the current tag.
///
/// # Preconditions
/// When tagging is enabled, a tag was pushed on the current execution context and not yet popped.
CORE_API void PopTag();
/// Returns the innermost tag of the current execution context, or [`MemoryTag::Untagged`].
CORE_API MemoryTag GetCurrentTag();
/// Writes the counters of used tags into `stats`.
///
/// Writes at most `maxCount` entries and returns the number of used tags, which may be larger than `maxCount`.
/// The untagged tag is always reported, so the returned count is at least one.
CORE_API uint32 GetTagStats(MemoryTagStats *stats, uint32 maxCount);
/// Sets the byte budget of `tag`, or clears it when `bytes` is zero.
///
/// The budget handler runs once each time the tag's current bytes cross above the budget. The budget applies to
/// `Current`, not to `Peak`.
CORE_API void SetBudget(MemoryTag tag, usize bytes);
/// Sets the handler called when a tag first goes over its budget.
///
/// The default writes a message to the debug output. Passing null restores the default.
///
/// # Thread safety
/// Safe to call from any thread; the handler runs on the thread that made the allocation.
CORE_API void SetBudgetHandler(MemoryBudgetHandler handler);

/// Starts recording the call stack of every allocation.
///
/// Tracking is slow and uses memory reserved from the OS. Does nothing when `MARLIN_MEMORY_TAGS` is 0. Calling it more
/// than once has no further effect.
///
/// # Thread safety
/// Safe to call from any thread.
CORE_API void EnableTracking();
/// Returns whether allocation tracking is on.
CORE_API bool IsTracking();
/// Calls `visitor` once for each distinct call stack with live allocations.
///
/// `context` is passed through unchanged. The visitor runs while the tracker is locked, so it must not allocate from
/// this heap. Does nothing when tracking is off.
///
/// # Thread safety
/// Safe to call from any thread; the visitor is serialized with tracking and with other visitors.
CORE_API void VisitAllocationSites(AllocationSiteVisitor visitor, void *context);
} // namespace Memory

/// Counts the allocations of its scope under `tag`.
///
/// Construction pushes the tag and destruction pops it, so scopes must be destroyed in reverse order.
///
/// # Examples
///
/// ```cpp
/// void Load()
/// {
///     MARLIN_MEMORY_SCOPE(MemoryTag::Assets);
///     void *data = Memory::Allocate(1024);
///     Memory::Free(data);
/// }
/// ```
class MemoryScope
{
public:
    /// Pushes `tag` for the lifetime of the scope.
    explicit MemoryScope(MemoryTag tag)
    {
        Memory::PushTag(tag);
    }

    /// Pops the tag.
    ~MemoryScope()
    {
        Memory::PopTag();
    }

    MARLIN_NONCOPYABLE(MemoryScope);
};
} // namespace Marlin

#if MARLIN_MEMORY_TAGS
/// Counts the allocations of the current scope under `tag`.
///
/// Expands to a [`Marlin::MemoryScope`] with a unique name, or to `(void)(tag)` when `MARLIN_MEMORY_TAGS` is 0.
#define MARLIN_MEMORY_SCOPE(tag) const ::Marlin::MemoryScope MARLIN_CONCAT(_memoryScope, __LINE__)(tag)
#else
#define MARLIN_MEMORY_SCOPE(tag) ((void)(tag))
#endif
