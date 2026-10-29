//! Hashing for containers and persisted keys: `HashCombine`, `HashInteger`, and the `Hasher` policy used by
//! `HashMap` and `HashSet`.

#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Hash/RapidHash.h"
#include "Templates/Bit.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// Folds `hash` into `seed`, for hashing several fields into one value.
[[nodiscard]] constexpr uint64 HashCombine(uint64 seed, uint64 hash) noexcept
{
    return RapidMix(seed ^ Private::RapidSecret[0], hash ^ Private::RapidSecret[1]);
}

/// Spreads an integer over all 64 bits.
[[nodiscard]] constexpr uint64 HashInteger(uint64 value) noexcept
{
    return RapidMix(value ^ Private::RapidSecret[0], Private::RapidSecret[1]);
}

/// The hash a `HashMap` or `HashSet` uses for `T`.
///
/// Types opt in with a `uint64 GetHash() const` member, or by specializing `Hasher`. The primary template is
/// undefined, so an unsupported type fails to compile.
template <typename T> struct Hasher;

/// Whether `T` has a `GetHash()` member that returns a 64-bit value.
template <typename T>
concept HasGetHash = requires(const T &value) {
    { value.GetHash() } -> ConvertibleTo<uint64>;
};

/// Whether `Hasher<T>` has a `Get` that returns a 64-bit value.
template <typename T>
concept Hashable = requires(const T &value) {
    { Hasher<T>::Get(value) } -> ConvertibleTo<uint64>;
};

/// Hashes a type by its `GetHash()` member.
template <HasGetHash T> struct Hasher<T>
{
    [[nodiscard]] static constexpr uint64 Get(const T &value) noexcept
    {
        return value.GetHash();
    }
};

/// Hashes an integer or enum by mixing its bits.
template <typename T>
    requires(IsIntegral<T> || IsEnum<T>)
struct Hasher<T>
{
    [[nodiscard]] static constexpr uint64 Get(T value) noexcept
    {
        return HashInteger(static_cast<uint64>(value));
    }
};

/// Hashes a pointer by its address.
template <typename T> struct Hasher<T *>
{
    [[nodiscard]] static uint64 Get(const T *value) noexcept
    {
        return HashInteger(reinterpret_cast<uintptr>(value));
    }
};

/// Hashes a C string by its contents, the same as `StringView` and `String`, so string-keyed maps accept C strings
/// for lookup.
template <> struct Hasher<const char *>
{
    /// Returns the hash of the string's contents.
    [[nodiscard]] static constexpr uint64 Get(const char *value) noexcept
    {
        return RapidHash(value, __builtin_strlen(value));
    }
};

/// Hashes a writable C string by its contents.
template <> struct Hasher<char *> : Hasher<const char *>
{
};

/// Hashes a character array by its contents.
template <usize TCount> struct Hasher<char[TCount]> : Hasher<const char *>
{
};

/// Hashes a floating-point value by its bits; zero and negative zero hash equally.
template <FloatingPoint T> struct Hasher<T>
{
    /// Returns a hash of `value`.
    [[nodiscard]] static constexpr uint64 Get(T value) noexcept
    {
        if (value == T{0})
        {
            return HashInteger(0);
        }
        if constexpr (sizeof(T) == 4)
        {
            return HashInteger(BitCast<uint32>(value));
        }
        else
        {
            return HashInteger(BitCast<uint64>(value));
        }
    }
};

/// Returns the hash of `value` from `Hasher<T>`.
template <typename T> [[nodiscard]] constexpr uint64 GetHash(const T &value) noexcept
{
    return Hasher<T>::Get(value);
}
} // namespace Marlin
