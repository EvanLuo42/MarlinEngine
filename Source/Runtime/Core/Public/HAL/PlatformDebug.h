//! Debugger output and debugger detection, usable before the log exists and while the process is failing.

#pragma once

#include "CoreTypes.h"

namespace Marlin::PlatformDebug
{
/// Writes `text` to standard error and, when a debugger is attached, to the debugger.
///
/// The write is unbuffered. The function may be called from any thread, including while the process is crashing.
///
/// # Preconditions
/// `text` must be a null-terminated UTF-8 string. It is not copied and must stay valid for the call.
CORE_API void Output(const char *text);
/// Returns whether a debugger is attached to the process.
///
/// Assertions and fatal errors use it to decide whether to break before trapping.
CORE_API bool IsDebuggerAttached();
} // namespace Marlin::PlatformDebug
