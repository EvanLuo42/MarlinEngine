//! A move-only owning pointer that deletes its object.
//!
//! A `UniquePtr` is the sole owner of a heap object; moving transfers ownership and leaves the source null.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Hash/Hash.h"
#include "Templates/Utility.h"

#include <compare>

namespace Marlin
{
/// Sole ownership of a heap object, deleted with it.
///
/// `T` must be complete when the pointer is destroyed. A `UniquePtr` can be moved but not copied; moving leaves the
/// source null.
///
/// # Examples
///
/// ```cpp
/// UniquePtr<int32> value = MakeUnique<int32>(42);
/// int32 read = *value; // -> 42
/// value.Reset();
/// bool empty = !value; // -> true
/// ```
template <typename T> class UniquePtr
{
public:
    /// Creates a null pointer.
    constexpr UniquePtr() = default;

    /// Creates a null pointer from `nullptr`.
    constexpr UniquePtr(decltype(nullptr) /*null*/)
    {
    }

    /// Takes ownership of `pointer`, which must point to a single object allocated with `new`.
    ///
    /// # Preconditions
    /// `pointer` is null, or was allocated with `new` and is not owned by another `UniquePtr`.
    constexpr explicit UniquePtr(T *pointer) : _pointer(pointer)
    {
    }

    UniquePtr(const UniquePtr &) = delete;
    UniquePtr &operator=(const UniquePtr &) = delete;

    /// Takes ownership from `other`, which becomes null.
    constexpr UniquePtr(UniquePtr &&other) noexcept : _pointer(other.Release())
    {
    }

    /// Takes ownership from a `UniquePtr` whose pointer converts to `T*`; `other` becomes null.
    template <typename TOther>
        requires(IsConvertible<TOther *, T *>)
    constexpr UniquePtr(UniquePtr<TOther> &&other) noexcept : _pointer(other.Release())
    {
    }

    /// Deletes the owned object and takes ownership from `other`, which becomes null.
    constexpr UniquePtr &operator=(UniquePtr &&other) noexcept
    {
        Reset(other.Release());
        return *this;
    }

    /// Deletes the owned object and takes ownership from a `UniquePtr` whose pointer converts to `T*`; `other`
    /// becomes null.
    template <typename TOther>
        requires(IsConvertible<TOther *, T *>)
    constexpr UniquePtr &operator=(UniquePtr<TOther> &&other) noexcept
    {
        Reset(other.Release());
        return *this;
    }

    /// Deletes the owned object.
    constexpr ~UniquePtr()
    {
        Reset();
    }

    /// Deletes the owned object and takes ownership of `pointer`.
    ///
    /// # Preconditions
    /// `pointer` is null, was allocated with `new`, is not owned elsewhere, and is not the currently owned pointer.
    constexpr void Reset(T *pointer = nullptr)
    {
        const T *previous = Exchange(_pointer, pointer);
        delete previous;
    }

    /// Returns the owned pointer and sets this to null, without deleting it.
    [[nodiscard]] constexpr T *Release()
    {
        return Exchange(_pointer, nullptr);
    }

    /// Returns the owned pointer, or null.
    [[nodiscard]] constexpr T *Get() const
    {
        return _pointer;
    }

    /// Returns whether an object is owned.
    [[nodiscard]] constexpr bool IsValid() const
    {
        return _pointer != nullptr;
    }

    /// Returns whether an object is owned.
    constexpr explicit operator bool() const
    {
        return _pointer != nullptr;
    }

    /// Returns the owned object.
    ///
    /// # Preconditions
    /// An object is owned. The pointer is checked by an assertion in builds with assertions; dereferencing null is
    /// undefined behavior otherwise.
    constexpr T &operator*() const
    {
        MARLIN_ASSERT(_pointer != nullptr);
        return *_pointer;
    }

    /// Returns the owned pointer.
    ///
    /// # Preconditions
    /// An object is owned. The pointer is checked by an assertion in builds with assertions; dereferencing null is
    /// undefined behavior otherwise.
    constexpr T *operator->() const
    {
        MARLIN_ASSERT(_pointer != nullptr);
        return _pointer;
    }

    /// Returns whether both own the same pointer.
    constexpr bool operator==(const UniquePtr &other) const
    {
        return _pointer == other._pointer;
    }

    /// Returns whether no object is owned.
    constexpr bool operator==(decltype(nullptr) /*null*/) const
    {
        return _pointer == nullptr;
    }

    /// Returns a hash of the owned pointer.
    [[nodiscard]] uint64 GetHash() const
    {
        return Marlin::GetHash(_pointer);
    }

private:
    T *_pointer = nullptr;
};

/// A `UniquePtr` is trivially relocatable, so its storage can be moved without destroying the source.
template <typename T> inline constexpr bool IsTriviallyRelocatable<UniquePtr<T>> = true;

/// Creates a `T` with `new` and returns a `UniquePtr` that owns it.
///
/// `args` are forwarded to the constructor. Allocation failure is fatal.
template <typename T, typename... TArgs> [[nodiscard]] UniquePtr<T> MakeUnique(TArgs &&...args)
{
    return UniquePtr<T>(new T(Forward<TArgs>(args)...));
}
} // namespace Marlin
