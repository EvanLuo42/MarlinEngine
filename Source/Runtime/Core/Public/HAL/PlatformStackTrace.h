//! Capturing the calling thread's call stack and resolving addresses to function names, files and lines.
//!
//! `Capture` is cheap and needs no debug symbols; `Symbolize` loads them on first use and is slow.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// The function, source location and module that describe one frame of a stack.
///
/// A field is left empty when its text does not fit or is unknown.
struct StackFrameSymbol
{
    /// The undecorated function name, or empty when it is unknown.
    char Function[256] = {};
    /// The source file path, or empty without line information.
    char File[512] = {};
    /// The module that contains the address: the executable or a dynamic library.
    char Module[128] = {};
    /// Zero without line information.
    uint32 Line = 0;
};

/// Captures and resolves call stacks of the calling thread.
namespace PlatformStackTrace
{
/// Fills `frames` with up to `maxCount` return addresses of the calling thread, innermost first.
///
/// `skip` frames above `Capture` are omitted, so zero records the calling function and one its caller. Returns the
/// number of addresses written, which is less than `maxCount` at the top of the stack.
///
/// # Preconditions
/// `frames` points to `maxCount` addresses. Nothing is checked; a null or too small buffer is undefined behavior.
///
/// # Examples
///
/// ```cpp
/// void *frames[16];
/// const uint32 count = PlatformStackTrace::Capture(frames, 16); // -> at most 16
/// ```
CORE_API uint32 Capture(void **frames, uint32 maxCount, uint32 skip = 0);
/// Resolves `address` to the function, source location and module that contain it.
///
/// The first call loads the process's debug symbols, which takes time and memory. Without them, as in Shipping
/// builds, the call returns false and leaves `symbol` empty. Returns whether a function name was found; `File`,
/// `Line` and `Module` may be filled even when it returns false.
///
/// # Thread safety
/// Calls from any thread are serialized.
///
/// # Performance
/// Slow enough for diagnostic paths only.
CORE_API bool Symbolize(const void *address, StackFrameSymbol &symbol);
} // namespace PlatformStackTrace
} // namespace Marlin
