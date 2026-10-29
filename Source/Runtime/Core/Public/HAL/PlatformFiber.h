//! User-mode fibers that run on a thread one at a time.
//!
//! A fiber has its own stack and is scheduled cooperatively: `SwitchTo` runs another fiber on the same thread and
//! returns when some fiber switches back. Nothing preempts a fiber; it runs until it switches.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// The function a fiber runs.
///
/// It must not return: a finished fiber switches to another fiber, which deletes it, so reaching the end of the
/// entry point is a fatal error.
using FiberEntry = void (*)(void *argument);

/// A handle to a fiber, from `PlatformFiber::Create` or `ConvertCurrentThread`.
struct FiberHandle
{
    /// Opaque state of the fiber, or null for an invalid handle.
    void *Value = nullptr;

    /// Returns whether the handle refers to a fiber.
    [[nodiscard]] bool IsValid() const
    {
        return Value != nullptr;
    }
};

/// Creates, switches and deletes fibers on the calling thread.
///
/// A thread must be converted to a fiber with `ConvertCurrentThread` before it can switch to one.
namespace PlatformFiber
{
/// Converts the calling thread to a fiber so that it can switch to other fibers.
///
/// The converted thread is itself a fiber; `RevertCurrentThread` undoes the conversion.
///
/// # Errors
/// Returns an invalid handle when the thread is already a fiber or the OS cannot convert it.
CORE_API FiberHandle ConvertCurrentThread();
/// Reverts the calling thread from a fiber back to a plain thread.
///
/// # Preconditions
/// Runs on the fiber that `ConvertCurrentThread` returned. The OS call is checked by an assertion.
CORE_API void RevertCurrentThread();

/// Creates a fiber that runs `entry` with `argument` on a stack of `stackSize` bytes.
///
/// The fiber does not run until a thread switches to it. `stackSize` is a reservation; the OS commits pages as
/// the stack grows, and zero selects the platform default.
///
/// # Preconditions
/// `entry` is not null; the check is an assertion.
///
/// # Errors
/// Returns an invalid handle when the OS cannot create the fiber or the runtime cannot allocate its state.
///
/// # Examples
///
/// ```cpp
/// struct Runner
/// {
///     FiberHandle Origin;
/// };
///
/// void Run(void *argument)
/// {
///     auto *runner = static_cast<Runner *>(argument);
///     PlatformFiber::SwitchTo(runner->Origin);
/// }
///
/// Runner runner;
/// runner.Origin = PlatformFiber::ConvertCurrentThread();
/// const FiberHandle fiber = PlatformFiber::Create(64 * 1024, &Run, &runner);
/// PlatformFiber::SwitchTo(fiber);
/// PlatformFiber::Delete(fiber);
/// PlatformFiber::RevertCurrentThread();
/// ```
CORE_API FiberHandle Create(usize stackSize, FiberEntry entry, void *argument);
/// Deletes `fiber` and releases its stack.
///
/// # Preconditions
/// The fiber is not running and no thread switches to it afterwards. The handle is checked by an assertion.
CORE_API void Delete(FiberHandle fiber);

/// Switches the calling thread to `fiber`, saving the state of the fiber that runs now.
///
/// The call returns when another fiber switches back. The calling thread must already be a fiber.
///
/// # Preconditions
/// `fiber` is valid and still exists; the check is an assertion.
CORE_API void SwitchTo(FiberHandle fiber);
/// Returns the fiber the calling thread runs, or an invalid handle when the thread is not a fiber.
CORE_API FiberHandle Current();
} // namespace PlatformFiber
} // namespace Marlin
