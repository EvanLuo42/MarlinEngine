//! A tagged union that stores exactly one value of a fixed set of types inline.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
namespace Private
{
template <typename T, typename... TTypes> constexpr uint32 IndexOfType()
{
    constexpr bool Matches[] = {IsSame<T, TTypes>...};
    for (uint32 i = 0; i < sizeof...(TTypes); ++i)
    {
        if (Matches[i])
        {
            return i;
        }
    }
    return ~0u;
}

template <typename... TTypes> constexpr usize MaxSize()
{
    usize size = 1;
    ((size = sizeof(TTypes) > size ? sizeof(TTypes) : size), ...);
    return size;
}

template <typename... TTypes> constexpr usize MaxAlignment()
{
    usize alignment = 1;
    ((alignment = alignof(TTypes) > alignment ? alignof(TTypes) : alignment), ...);
    return alignment;
}
} // namespace Private

/// A tagged union that stores exactly one value of `TTypes...`, inline.
///
/// `TTypes...` must contain between 1 and 254 types. The first alternative is the default state when it is
/// default constructible. The active alternative is chosen by construction or assignment and can be queried
/// with `Is<T>()`, `Get<T>()` and `GetIndex()`.
///
/// # Examples
///
/// ```cpp
/// Variant<int32, float32> value = 2.5f;
/// bool isFloat = value.Is<float32>(); // -> true
/// float32 number = value.Get<float32>(); // -> 2.5f
/// ```
template <typename... TTypes> class Variant
{
    static_assert(sizeof...(TTypes) > 0 && sizeof...(TTypes) < 255);

public:
    /// The index that `IndexOf<T>` returns for a type that is not one of the alternatives.
    static constexpr uint32 InvalidIndex = ~0u;

    /// The index of `T` in `TTypes...`, or `InvalidIndex` when `T` is not one of them.
    ///
    /// The comparison is exact: `const`, reference and other qualified forms do not match.
    template <typename T> static constexpr uint32 IndexOf = Private::IndexOfType<T, TTypes...>();

    /// Creates a variant holding a value-initialized first alternative.
    ///
    /// # Preconditions
    /// The first alternative must be default constructible; the constructor is constrained away otherwise.
    Variant()
        requires(DefaultConstructible<TypeAt<0, TTypes...>>)
        : _index(0)
    {
        ConstructAt(reinterpret_cast<TypeAt<0, TTypes...> *>(_storage));
    }

    /// Creates a variant holding a `T` constructed from `value`.
    ///
    /// # Preconditions
    /// `RemoveCvRef<T>` must be one of `TTypes...`.
    template <typename T>
        requires(IndexOf<RemoveCvRef<T>> != ~0u)
    Variant(T &&value) : _index(static_cast<uint8>(IndexOf<RemoveCvRef<T>>))
    {
        ConstructAt(reinterpret_cast<RemoveCvRef<T> *>(_storage), Forward<T>(value));
    }

    /// Creates a variant that holds a copy of the value in `other`.
    Variant(const Variant &other)
    {
        other.Visit([this](const auto &value) { this->Assign(value); });
    }

    /// Creates a variant that moves the value out of `other`; `other` keeps its alternative and a moved-from
    /// value.
    Variant(Variant &&other) noexcept
    {
        other.Visit([this](auto &value) { this->Assign(Move(value)); });
    }

    /// Destroys the held value.
    ~Variant()
    {
        Destroy();
    }

    /// Replaces the held value with a copy of the value in `other`.
    Variant &operator=(const Variant &other)
    {
        if (this != &other)
        {
            Destroy();
            other.Visit([this](const auto &value) { this->Assign(value); });
        }
        return *this;
    }

    /// Replaces the held value by moving from `other`; `other` keeps its alternative and a moved-from value.
    Variant &operator=(Variant &&other) noexcept
    {
        if (this != &other)
        {
            Destroy();
            other.Visit([this](auto &value) { this->Assign(Move(value)); });
        }
        return *this;
    }

    /// Replaces the held value with a `T` constructed from `value`.
    ///
    /// # Preconditions
    /// `RemoveCvRef<T>` must be one of `TTypes...`.
    template <typename T>
        requires(IndexOf<RemoveCvRef<T>> != ~0u)
    Variant &operator=(T &&value)
    {
        Destroy();
        Assign(Forward<T>(value));
        return *this;
    }

    /// Destroys the held value, constructs a `T` from `args`, and returns a reference to it.
    ///
    /// # Preconditions
    /// `T` must be one of `TTypes...`; a `static_assert` enforces that.
    template <typename T, typename... TArgs> T &Emplace(TArgs &&...args)
    {
        static_assert(IndexOf<T> != InvalidIndex, "T is not one of the variant's types");
        Destroy();
        T *value = ConstructAt(reinterpret_cast<T *>(_storage), Forward<TArgs>(args)...);
        _index = static_cast<uint8>(IndexOf<T>);
        return *value;
    }

    /// Returns the index of the held alternative in `TTypes...`.
    [[nodiscard]] uint32 GetIndex() const
    {
        return _index;
    }

    /// Returns whether the variant holds a `T`.
    ///
    /// The comparison is exact: `const`, reference and other qualified forms do not match.
    template <typename T> [[nodiscard]] bool Is() const
    {
        return _index == IndexOf<T>;
    }

    /// Returns the held value as a `T`.
    ///
    /// # Preconditions
    /// The variant must hold a `T`. The check is `MARLIN_ASSERT`, so a mismatched access is undefined behavior
    /// in Profile and Shipping.
    template <typename T> [[nodiscard]] T &Get()
    {
        MARLIN_ASSERT(Is<T>());
        return *reinterpret_cast<T *>(_storage);
    }

    /// Returns the held value as a const `T`.
    template <typename T> [[nodiscard]] const T &Get() const
    {
        MARLIN_ASSERT(Is<T>());
        return *reinterpret_cast<const T *>(_storage);
    }

    /// Returns a pointer to the held value when it is a `T`, or null otherwise.
    template <typename T> [[nodiscard]] T *TryGet()
    {
        return Is<T>() ? reinterpret_cast<T *>(_storage) : nullptr;
    }

    /// Returns a const pointer to the held value when it is a `T`, or null otherwise.
    template <typename T> [[nodiscard]] const T *TryGet() const
    {
        return Is<T>() ? reinterpret_cast<const T *>(_storage) : nullptr;
    }

    /// Calls `visitor` with the held value and returns its result.
    ///
    /// Every alternative must produce the same return type. The visitor receives `T &` from a non-const variant
    /// and `const T &` from a const one.
    ///
    /// # Examples
    ///
    /// ```cpp
    /// Variant<int32, float32> value = 3;
    /// int32 doubled = value.Visit([](auto number) { return static_cast<int32>(number) * 2; }); // -> 6
    /// ```
    template <typename TVisitor> decltype(auto) Visit(TVisitor &&visitor)
    {
        return VisitImpl<0>(visitor, this);
    }

    /// Calls `visitor` with the held value as a `const T &` and returns its result.
    template <typename TVisitor> decltype(auto) Visit(TVisitor &&visitor) const
    {
        return VisitImpl<0>(visitor, this);
    }

    /// Returns whether both variants hold the same alternative and equal values.
    ///
    /// `TTypes...` must all support `==`.
    bool operator==(const Variant &other) const
    {
        return _index == other._index &&
               Visit([&other](const auto &value) { return value == other.Get<RemoveCvRef<decltype(value)>>(); });
    }

private:
    template <typename T> void Assign(T &&value)
    {
        using TValue = RemoveCvRef<T>;
        ConstructAt(reinterpret_cast<TValue *>(_storage), Forward<T>(value));
        _index = static_cast<uint8>(IndexOf<TValue>);
    }

    void Destroy()
    {
        if (_index != 0xFF)
        {
            Visit([](auto &value) { DestroyAt(&value); });
            _index = 0xFF;
        }
    }

    template <uint32 TIndex, typename TVisitor, typename TSelf>
    static decltype(auto) VisitImpl(TVisitor &visitor, TSelf *self)
    {
        using TValue = TypeAt<TIndex, TTypes...>;
        using TQualified = Conditional<IsConst<TSelf>, const TValue, TValue>;
        if constexpr (TIndex + 1 == sizeof...(TTypes))
        {
            return visitor(*reinterpret_cast<TQualified *>(self->_storage));
        }
        else
        {
            if (self->_index == TIndex)
            {
                return visitor(*reinterpret_cast<TQualified *>(self->_storage));
            }
            return VisitImpl<TIndex + 1>(visitor, self);
        }
    }

    alignas(Private::MaxAlignment<TTypes...>()) uint8 _storage[Private::MaxSize<TTypes...>()];
    uint8 _index = 0xFF;
};
} // namespace Marlin
