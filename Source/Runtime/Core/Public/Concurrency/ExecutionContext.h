//! State that follows the code running on a thread, including across fiber switches.
//!
//! The job system gives every fiber its own context and switches the current context with the fiber, so a job keeps
//! its memory tag stack and fiber-local values when it resumes on another thread.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// State that follows the code running on a thread: the thread itself, or the job a fiber is running.
///
/// The job system switches the context with the fiber, so a job that resumes on another thread keeps its memory tag
/// stack and fiber-local values. The job system and the memory tag implementation maintain the fields; code normally
/// uses `FiberLocal` and the memory tag API instead of reading them.
struct ExecutionContext
{
    /// The maximum depth of the memory tag stack.
    static constexpr uint32 MaxMemoryTagDepth = 32;
    /// The maximum number of fiber-local slots.
    static constexpr uint32 MaxFiberLocals = 64;

    /// The memory tag slots pushed on this context, innermost last.
    ///
    /// Only the first `MemoryTagDepth` entries are valid. The entries are internal tag-table slots, not
    /// `MemoryTag` values.
    uint16 MemoryTags[MaxMemoryTagDepth] = {};
    /// The number of valid entries in `MemoryTags`.
    uint32 MemoryTagDepth = 0;
    /// The pointer-sized values of this context's `FiberLocal` objects, indexed by slot.
    ///
    /// Slots that no `FiberLocal` has used stay zero. The job system clears the allocated slots when a job starts
    /// and saves and restores them around jobs that run inline on a waiting thread.
    void *FiberLocals[MaxFiberLocals] = {};
};

/// Returns the execution context of the calling thread or job.
///
/// The reference is never null; it refers to the thread's own context when no job has switched one in. A fiber
/// switch can change which context the calling thread returns.
///
/// # Thread safety
/// Returns a context private to the calling thread. Do not share the reference with another thread.
CORE_API ExecutionContext &GetExecutionContext();
/// Replaces the calling thread's current execution context.
///
/// Passing null restores the thread's own context. This is for the job system; other code should not switch
/// contexts.
///
/// # Safety
/// `context` must stay alive until it is replaced or reset to null. No ownership is transferred, and a stale
/// pointer makes [GetExecutionContext] return a dangling reference.
///
/// # Thread safety
/// Changes only the calling thread's current context; call it on the thread that is switching.
CORE_API void SetExecutionContext(ExecutionContext *context);
} // namespace Marlin
