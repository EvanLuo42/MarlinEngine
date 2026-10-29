//! Assertion and fatal-error macros that take a compile-time-checked format string and arguments.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "String/Format.h"

namespace Marlin::Private
{
/// Formats a failure message and reports it as an assertion or a fatal error.
///
/// When `expression` is not null, the message starts with it followed by `": "`. The text is formatted into
/// a local `StringBuilder` and passed to [AssertFailed] or [FatalError].
template <typename... TArgs>
[[noreturn]] MARLIN_NOINLINE void FailFormatted(bool assertion, const char *expression, const char *file, int32 line,
                                                FormatString<::Marlin::TypeIdentity<TArgs>...> format,
                                                const TArgs &...args)
{
    StringBuilder<512> message;
    if (expression != nullptr)
    {
        message.Append(expression).Append(": ");
    }
    FormatTo(message, format, args...);
    if (assertion)
    {
        AssertFailed(message.CStr(), file, line);
    }
    FatalError(message.CStr(), file, line);
}
} // namespace Marlin::Private

#if MARLIN_ASSERTIONS
/// Checks `expression`; on failure formats `__VA_ARGS__` into the message and terminates the process.
///
/// The format string and arguments are evaluated only when the check fails. The format is checked at compile
/// time: every placeholder must name an argument and every argument must be used.
///
/// # Examples
///
/// ```cpp
/// MARLIN_ASSERTF(index < count, "{0} of {1}", index, count);
/// ```
#define MARLIN_ASSERTF(expression, ...)                                                                                \
    (MARLIN_LIKELY(expression) ? (void)0                                                                               \
                               : ::Marlin::Private::FailFormatted(true, #expression, __FILE__, __LINE__, __VA_ARGS__))
/// Like [MARLIN_ASSERTF], but `expression` is evaluated even when assertions are compiled out; the format
/// arguments are still evaluated only on failure.
#define MARLIN_VERIFYF(expression, ...) MARLIN_ASSERTF(expression, __VA_ARGS__)
#else
/// Does nothing; assertions are compiled out. Neither `expression` nor the format arguments are evaluated.
#define MARLIN_ASSERTF(expression, ...) ((void)0)
/// Evaluates `expression` and discards it; the format arguments are not evaluated.
#define MARLIN_VERIFYF(expression, ...) ((void)(expression))
#endif

/// Reports a formatted message as a fatal error and terminates the process, in every configuration.
///
/// The format string and arguments are always evaluated. Formatting follows the same compile-time checks as
/// [MARLIN_ASSERTF].
///
/// # Examples
///
/// ```cpp
/// MARLIN_FATALF("device lost after {0} frames", frame);
/// ```
#define MARLIN_FATALF(...) ::Marlin::Private::FailFormatted(false, nullptr, __FILE__, __LINE__, __VA_ARGS__)
