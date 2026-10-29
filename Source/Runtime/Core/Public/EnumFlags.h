//! Bitwise operators for scoped flag enums and a helper that tests whether any flag is set.

#pragma once

#include "CoreTypes.h"

// NOLINTBEGIN(bugprone-macro-parentheses)
/// Defines `|`, `&`, `~`, `|=` and `&=` for the scoped enum `Enum`.
///
/// Invoke it at namespace scope after the enum definition, so argument-dependent lookup finds the operators. The
/// enum must have a fixed underlying type of at most 64 bits: each operator converts its operands to `uint64`,
/// combines them, and converts the result back to `Enum`.
///
/// # Examples
///
/// ```cpp
/// enum class Permissions : uint8
/// {
///     Read = 1 << 0,
///     Write = 1 << 1,
/// };
/// MARLIN_ENUM_FLAGS(Permissions)
///
/// Permissions permissions = Permissions::Read | Permissions::Write;
/// bool writable = HasAnyFlags(permissions, Permissions::Write); // -> true
/// ```
#define MARLIN_ENUM_FLAGS(Enum)                                                                                        \
    constexpr Enum operator|(Enum a, Enum b)                                                                           \
    {                                                                                                                  \
        return static_cast<Enum>(static_cast<Marlin::uint64>(a) | static_cast<Marlin::uint64>(b));                     \
    }                                                                                                                  \
    constexpr Enum operator&(Enum a, Enum b)                                                                           \
    {                                                                                                                  \
        return static_cast<Enum>(static_cast<Marlin::uint64>(a) & static_cast<Marlin::uint64>(b));                     \
    }                                                                                                                  \
    constexpr Enum operator~(Enum a)                                                                                   \
    {                                                                                                                  \
        return static_cast<Enum>(~static_cast<Marlin::uint64>(a));                                                     \
    }                                                                                                                  \
    constexpr Enum &operator|=(Enum &a, Enum b)                                                                        \
    {                                                                                                                  \
        return a = a | b;                                                                                              \
    }                                                                                                                  \
    constexpr Enum &operator&=(Enum &a, Enum b)                                                                        \
    {                                                                                                                  \
        return a = a & b;                                                                                              \
    }
// NOLINTEND(bugprone-macro-parentheses)

namespace Marlin
{
/// Returns whether any bit of `flags` is set in `value`.
///
/// The values are converted to `uint64` and combined, matching the operators that [MARLIN_ENUM_FLAGS] defines.
///
/// # Examples
///
/// ```cpp
/// #include "HAL/PlatformFile.h"
///
/// FileFlags flags = FileFlags::Append | FileFlags::Exclusive;
/// bool exclusive = HasAnyFlags(flags, FileFlags::Exclusive); // -> true
/// ```
template <typename TEnum> constexpr bool HasAnyFlags(TEnum value, TEnum flags)
{
    return (static_cast<uint64>(value) & static_cast<uint64>(flags)) != 0;
}
} // namespace Marlin
