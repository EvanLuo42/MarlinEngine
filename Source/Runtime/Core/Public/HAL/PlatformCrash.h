//! Crash handling: the failure a thread records, the handler that receives an exception, and minidump writing.
//!
//! A handler set with `SetHandler` runs on the crashed thread, and the process ends when it returns. Assertions,
//! fatal errors and runtime-library failures reach the same handler.

#pragma once

#include "CoreTypes.h"
#include "String/StringView.h"

namespace Marlin
{
/// Why a thread ended the process on purpose: an assertion, a fatal error or a runtime-library failure.
///
/// The strings are copies owned by the platform layer and stay valid until the thread records another failure.
struct CrashFailure
{
    /// A short category, such as "Assertion failed".
    const char *Kind = nullptr;
    /// The failure message; may be empty.
    const char *Message = nullptr;
    /// Null when the failure has no source location.
    const char *File = nullptr;
    /// The source line, or zero when it is unknown.
    int32 Line = 0;
};

/// What the crash handler knows about the exception that ended the process.
struct CrashInfo
{
    /// The OS exception code, or `PlatformCrash::FailureExceptionCode` for a deliberate failure.
    uint32 ExceptionCode = 0;
    /// The address of the instruction that raised the exception.
    void *Address = nullptr;
    /// The identifier of the thread that crashed.
    uint32 ThreadId = 0;
    /// EXCEPTION_POINTERS on Windows; an address in the crashed process.
    void *PlatformContext = nullptr;
    /// What the crashed thread recorded with `PlatformCrash::NoteFailure`, or null for a plain exception.
    const CrashFailure *Failure = nullptr;
};

/// Handles a crash on the crashing thread, whose stack may be nearly exhausted.
///
/// The process ends after the handler returns.
using CrashHandler = void (*)(const CrashInfo &info);

namespace PlatformCrash
{
/// The exception code of crashes that the runtime library's failure hooks raise: an abort, a pure virtual call or
/// an invalid parameter.
inline constexpr uint32 FailureExceptionCode = 0xE04D524Cu;
/// Stack kept for the crash handler when a thread overflows its stack.
///
/// Every thread reserves this many extra bytes, so the handler can still run after a stack overflow.
inline constexpr uint32 HandlerStackReserve = 64 * 1024;

/// Installs `handler` for unhandled exceptions and runtime-library failures.
///
/// It also routes the runtime library's pure virtual call, invalid parameter and abort failures to `handler`. The
/// process ends after `handler` returns. While a debugger is attached, the exception goes to the debugger and
/// `handler` does not run. Passing null restores the default behavior.
///
/// # Thread safety
/// Call it during startup, before other threads run.
CORE_API void SetHandler(CrashHandler handler);
/// Installs the handler again if a library replaced it, as some drivers and overlays do.
///
/// Does nothing when no handler is installed; cheap enough to call every frame.
CORE_API void RestoreHandler();
/// Records why this thread is about to crash, for the handler.
///
/// The strings are copied into fixed buffers and truncated to 63, 2047 and 259 bytes; `file` may be null. A later
/// call replaces the record.
///
/// # Thread safety
/// Each thread has its own record, so no synchronization is needed.
CORE_API void NoteFailure(const char *kind, StringView message, const char *file, int32 line);
/// Returns the failure this thread recorded with `NoteFailure`, or null when it recorded none.
///
/// The pointer stays valid until this thread records another failure.
[[nodiscard]] CORE_API const CrashFailure *GetNotedFailure();
/// Writes a minidump of `processId` to `path`, a UTF-8 path.
///
/// `info` describes the crash to record, or is null for a snapshot of the process as it is now. A snapshot of this
/// process is written from a helper thread, so the calling thread's stack is complete.
///
/// # Errors
/// Returns false when `path` is not valid UTF-8, the file cannot be created, or the process cannot be opened or
/// dumped.
///
/// # Thread safety
/// May be called from any thread.
///
/// # Examples
///
/// ```cpp
/// #include "HAL/PlatformProcess.h"
///
/// const uint32 processId = PlatformProcess::CurrentId();
/// const bool written = PlatformCrash::WriteMinidump("Saved/Crashes/Snapshot.dmp", processId, nullptr); // -> true
/// ```
CORE_API bool WriteMinidump(const char *path, uint32 processId, const CrashInfo *info);
} // namespace PlatformCrash
} // namespace Marlin
