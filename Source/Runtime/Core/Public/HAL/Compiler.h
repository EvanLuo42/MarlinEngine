//! Compiler portability macros for inlining, branch hints, unreachable code, sanitizer attributes and token
//! manipulation.
//!
//! Marlin builds only with clang, so each macro maps to a clang builtin or attribute; call sites use the macros
//! instead of the builtins.

#pragma once

/// Requests that every call be inlined. Calls the compiler cannot inline are diagnosed.
#define MARLIN_FORCEINLINE inline __attribute__((always_inline))
/// Prevents the compiler from inlining a function.
#define MARLIN_NOINLINE __attribute__((noinline))

/// Hints that `expression` is usually true; the expression is evaluated once and converted to `bool`.
#define MARLIN_LIKELY(expression) __builtin_expect(static_cast<bool>(expression), 1)
/// Hints that `expression` is usually false; the expression is evaluated once and converted to `bool`.
#define MARLIN_UNLIKELY(expression) __builtin_expect(static_cast<bool>(expression), 0)
/// Tells the optimizer that `expression` is always true.
///
/// # Safety
/// `expression` is not evaluated, so it must be free of side effects, and it must be true on every execution.
/// A false assumption is undefined behavior.
#define MARLIN_ASSUME(expression) __builtin_assume(expression)
/// Tells the optimizer that control never reaches this point.
///
/// # Safety
/// Reaching this point is undefined behavior. The macro does not trap by itself, so use it only where every
/// path has already returned or terminated.
#define MARLIN_UNREACHABLE() __builtin_unreachable()

/// Lets an empty member, such as a stateless allocator, occupy no space.
///
/// The Microsoft ABI needs the `msvc::` spelling; elsewhere the standard attribute applies.
#if MARLIN_PLATFORM_WINDOWS
#define MARLIN_NO_UNIQUE_ADDRESS [[msvc::no_unique_address]]
#else
#define MARLIN_NO_UNIQUE_ADDRESS [[no_unique_address]]
#endif

/// Expands to 1 when AddressSanitizer instruments this translation unit, and to 0 otherwise.
#if __has_feature(address_sanitizer)
#define MARLIN_ASAN 1
#else
#define MARLIN_ASAN 0
#endif

/// Keeps AddressSanitizer from instrumenting a function, for code such as an allocator that reads memory it
/// poisoned itself.
#define MARLIN_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))

/// Breaks into the debugger; crashes the process when no debugger is attached.
#define MARLIN_DEBUG_BREAK() __builtin_debugtrap()
/// Crashes the process immediately with an illegal instruction, in every configuration.
#define MARLIN_TRAP() __builtin_trap()

/// Stringifies `token` without expanding macros in it; use [MARLIN_STRINGIFY] instead.
#define MARLIN_STRINGIFY_INNER(token) #token
/// Stringifies `token` after expanding macros in it; `MARLIN_STRINGIFY(__LINE__)` yields the line number as
/// text.
#define MARLIN_STRINGIFY(token) MARLIN_STRINGIFY_INNER(token)
/// Concatenates `a` and `b` without expanding macros in them; use [MARLIN_CONCAT] instead.
#define MARLIN_CONCAT_INNER(a, b) a##b
/// Concatenates two tokens after expanding macros in them.
#define MARLIN_CONCAT(a, b) MARLIN_CONCAT_INNER(a, b)

// NOLINTBEGIN(bugprone-macro-parentheses)
/// Deletes the copy and move constructors and assignments of `Type`.
///
/// Place it in the class body with a trailing semicolon. The deleted copy and move constructors suppress the
/// implicitly declared default constructor, so declare `Type() = default;` when the type must be
/// default-constructible.
///
/// # Examples
///
/// ```cpp
/// class Thread
/// {
/// public:
///     Thread() = default;
///     MARLIN_NONCOPYABLE(Thread);
/// };
/// ```
#define MARLIN_NONCOPYABLE(Type)                                                                                       \
    Type(const Type &) = delete;                                                                                       \
    Type &operator=(const Type &) = delete;                                                                            \
    Type(Type &&) = delete;                                                                                            \
    Type &operator=(Type &&) = delete
// NOLINTEND(bugprone-macro-parentheses)
