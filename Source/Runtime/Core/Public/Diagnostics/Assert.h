//! Assertion and fatal-error macros and the failure functions they call.

#pragma once

#include "CoreTypes.h"

namespace Marlin::Private
{
/// Reports a failed assertion: records it for the crash report, writes it to the log and the debugger, and
/// terminates the process.
///
/// Called by [MARLIN_ASSERT] and [MARLIN_ASSERTF]; the process never resumes.
[[noreturn]] CORE_API void AssertFailed(const char *expression, const char *file, int32 line);
/// Reports an unconditional fatal error with the same reporting and termination as [AssertFailed].
[[noreturn]] CORE_API void FatalError(const char *message, const char *file, int32 line);
} // namespace Marlin::Private

#if MARLIN_ASSERTIONS
/// Checks `expression` when assertions are enabled; a failure reports the expression and location and crashes
/// the process.
///
/// A failed check writes a fatal log entry, breaks into an attached debugger, and traps. `MARLIN_ASSERTIONS` is 1
/// in Debug and Development and 0 in Profile and Shipping.
///
/// # Examples
///
/// ```cpp
/// MARLIN_ASSERT(index < count);
/// ```
#define MARLIN_ASSERT(expression)                                                                                      \
    (MARLIN_LIKELY(expression) ? (void)0 : ::Marlin::Private::AssertFailed(#expression, __FILE__, __LINE__))
/// Like [MARLIN_ASSERT], but evaluates `expression` in every configuration.
#define MARLIN_VERIFY(expression) MARLIN_ASSERT(expression)
#else
/// Does nothing; assertions are compiled out and `expression` is not evaluated.
#define MARLIN_ASSERT(expression) ((void)0)
/// Evaluates `expression` and discards the result; the check is compiled out in this configuration.
#define MARLIN_VERIFY(expression) ((void)(expression))
#endif

#if MARLIN_BUILD_DEBUG
/// Checks `expression` like [MARLIN_ASSERT], but only in Debug builds.
#define MARLIN_CHECK_SLOW(expression) MARLIN_ASSERT(expression)
#else
/// Does nothing; slow checks are compiled in only in Debug builds.
#define MARLIN_CHECK_SLOW(expression) ((void)0)
#endif

/// Reports `message` as a fatal error and terminates the process, in every configuration.
///
/// The message is a null-terminated string; it is written to the log and the debugger with the file and line.
///
/// # Examples
///
/// ```cpp
/// MARLIN_FATAL("unsupported config version");
/// ```
#define MARLIN_FATAL(message) ::Marlin::Private::FatalError(message, __FILE__, __LINE__)
