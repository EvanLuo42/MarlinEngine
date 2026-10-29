//! Threads, their priorities, and the per-thread services the runtime uses.
//!
//! A thread is created from a [`ThreadDesc`], runs one function and is joined exactly once. `PlatformTls` holds
//! thread-local values for code that never runs on fibers.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// The function a thread runs, called with `ThreadDesc::Argument`.
using ThreadEntry = void (*)(void *argument);

/// A thread's scheduling priority, relative to the priority class of the process.
enum class ThreadPriority : uint8
{
    /// The lowest priority.
    Lowest,
    /// Below `Normal` and above `Lowest`.
    BelowNormal,
    /// The default priority of a new thread.
    Normal,
    /// Above `Normal` and below `Highest`.
    AboveNormal,
    /// Below `TimeCritical` and above every other priority.
    Highest,
    /// The highest priority.
    TimeCritical,
};

/// Describes a thread for `PlatformThread::Create`.
///
/// The descriptor is read during the call only; the new thread keeps no pointer to it.
struct ThreadDesc
{
    /// UTF-8 name for debuggers and profilers, or null for an unnamed thread.
    ///
    /// Names longer than 127 characters are ignored.
    const char *Name = nullptr;
    /// The function the thread runs; null is an assertion failure in `Create`.
    ThreadEntry Entry = nullptr;
    /// Passed to `Entry` unchanged; may be null.
    void *Argument = nullptr;
    /// Stack reservation in bytes, or zero for the platform default.
    ///
    /// The OS commits pages of the stack as it grows rather than committing all of it up front.
    usize StackSize = 0;
    /// Scheduling priority of the new thread.
    ThreadPriority Priority = ThreadPriority::Normal;
    /// Bitmask of the logical processors the thread may run on, or zero for any of them.
    ///
    /// Bit 0 is the first logical processor of the process's processor group.
    uint64 AffinityMask = 0;
};

/// A handle to a thread, from `PlatformThread::Create`.
struct ThreadHandle
{
    /// The OS thread handle, or zero for an invalid handle.
    uintptr Value = 0;

    /// Returns whether the handle refers to a thread.
    bool IsValid() const
    {
        return Value != 0;
    }
};

namespace PlatformThread
{
/// Creates a thread that starts by calling `desc.Entry` with `desc.Argument`.
///
/// The new thread takes the name, priority and affinity from `desc` and starts before the call returns. Every
/// created thread must be joined with `Join`.
///
/// # Preconditions
/// `desc.Entry` is not null; the check is an assertion.
///
/// # Errors
/// Returns an invalid handle when the OS cannot create the thread or the runtime cannot allocate its start
/// record.
///
/// # Thread safety
/// May be called from any thread.
///
/// # Examples
///
/// ```cpp
/// void Fill(void *argument)
/// {
///     *static_cast<int32 *>(argument) = 42;
/// }
///
/// int32 answer = 0;
/// ThreadDesc desc;
/// desc.Name = "Fill";
/// desc.Entry = &Fill;
/// desc.Argument = &answer;
/// const ThreadHandle thread = PlatformThread::Create(desc);
/// if (thread.IsValid())
/// {
///     PlatformThread::Join(thread);
/// }
/// const int32 result = answer; // -> 42
/// ```
CORE_API ThreadHandle Create(const ThreadDesc &desc);
/// Waits for `thread` to finish and releases its handle.
///
/// # Preconditions
/// `thread` is valid and has not been joined before; the check is an assertion. A thread must not join itself.
///
/// # Thread safety
/// May be called from any thread other than `thread`.
CORE_API void Join(ThreadHandle thread);

/// Returns the OS identifier of the calling thread.
CORE_API uint32 CurrentId();
/// Names the calling thread for debuggers and profilers.
///
/// The name is copied, so `name` need not outlive the call. Names longer than 127 characters are ignored.
CORE_API void SetCurrentName(const char *name);
/// Restricts the calling thread to the logical processors in `affinityMask`.
///
/// Returns false when the OS rejects the mask, as when it is zero or names a processor outside the process's
/// affinity.
CORE_API bool SetCurrentAffinity(uint64 affinityMask);
/// Suspends the calling thread for at least `milliseconds`.
///
/// A value of zero gives up the rest of the time slice instead of sleeping.
CORE_API void Sleep(uint32 milliseconds);
/// Gives up the rest of the calling thread's time slice to another ready thread on the same processor.
///
/// Returns immediately when no other thread is ready.
CORE_API void YieldThread();
} // namespace PlatformThread

/// OS thread-local storage: one value per slot and OS thread.
///
/// A fiber switch does not change the OS thread, so every fiber that runs on a thread shares its values. Code that
/// may run on fibers uses `FiberLocal` instead.
namespace PlatformTls
{
/// The slot value that marks a failed allocation; no allocated slot has it.
inline constexpr uint32 InvalidSlot = 0xFFFFFFFFu;

/// Allocates a slot for the process and returns its index.
///
/// A new slot holds null on every thread until a thread stores a value in it.
///
/// # Errors
/// Returns `InvalidSlot` when the process has no free slot.
CORE_API uint32 Allocate();
/// Releases `slot`, which `Allocate` returned.
///
/// Values stored by threads are dropped, not freed.
///
/// # Preconditions
/// `slot` is an allocated slot that has not been freed. The OS call is checked by an assertion.
CORE_API void Free(uint32 slot);
/// Returns the value the calling thread stored in `slot`, or null when it stored none.
CORE_API void *Get(uint32 slot);
/// Stores `value` for the calling thread in `slot`, replacing its previous value.
///
/// The OS call is checked by an assertion.
CORE_API void Set(uint32 slot, void *value);
} // namespace PlatformTls
} // namespace Marlin
