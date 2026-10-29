//! Allocator policies that give containers their element storage.
//!
//! A policy is a small struct whose `ForType<T>` provides `Allocate`, `Free`, `IsInline` and `InlineCapacity`, so a
//! container can keep small buffers inside itself without a heap allocation.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Memory.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// A type-erased allocator, for code that cannot be templated on its allocator.
///
/// Implementations report allocation failure themselves; the engine has no exceptions. The interface does not
/// synchronize, so each implementation defines its own thread safety.
class IAllocator
{
public:
    IAllocator() = default;
    virtual ~IAllocator() = default;
    IAllocator(const IAllocator &) = default;
    IAllocator &operator=(const IAllocator &) = default;
    IAllocator(IAllocator &&) = default;
    IAllocator &operator=(IAllocator &&) = default;

    /// Returns a block of at least `size` bytes aligned to `alignment`.
    ///
    /// # Preconditions
    /// `alignment` is a power of two.
    [[nodiscard]] virtual void *Allocate(usize size, usize alignment) = 0;
    /// Frees a block returned by `Allocate`.
    ///
    /// # Preconditions
    /// `pointer` was returned by `Allocate` with the same `size`.
    virtual void Free(void *pointer, usize size) = 0;
};

namespace Private
{
/// The alignment a container requests for `T`: at least [`DefaultAlignment`].
template <typename T> constexpr usize ElementAlignment()
{
    return alignof(T) > DefaultAlignment ? alignof(T) : DefaultAlignment;
}
} // namespace Private

/// The interface a container needs from its allocator policy.
///
/// A policy's `ForType<T>` provides:
///
/// - `static constexpr usize InlineCapacity`: elements stored inside the container, or 0 for none.
/// - `T *Allocate(usize capacity)`: storage for `capacity` elements.
/// - `void Free(T *data, usize capacity)`: frees storage returned by `Allocate`; inline storage is ignored.
/// - `bool IsInline(const T *data) const`: whether `data` points to the inline storage.
///
/// `ForType` is constructible from the policy, so containers can take a configured policy such as an arena.

/// Allocates container storage from the process heap.
struct HeapAllocator
{
    /// The storage of one container type.
    template <typename T> class ForType
    {
    public:
        /// Heap-allocated storage has no inline capacity.
        static constexpr usize InlineCapacity = 0;

        /// Creates the policy; the argument is ignored.
        constexpr ForType() = default;
        constexpr explicit ForType(const HeapAllocator & /*policy*/)
        {
        }

        /// Returns uninitialized storage for `capacity` elements.
        [[nodiscard]] T *Allocate(usize capacity)
        {
            return static_cast<T *>(Memory::Allocate(capacity * sizeof(T), Private::ElementAlignment<T>()));
        }

        /// Frees storage returned by `Allocate`.
        void Free(T *data, usize /*capacity*/)
        {
            Memory::Free(data);
        }

        /// Returns false, because all storage comes from the heap.
        [[nodiscard]] constexpr bool IsInline(const T * /*data*/) const
        {
            return false;
        }
    };
};

/// Stores the first `TCount` elements inside the container and the rest through `TFallback`.
///
/// # Examples
///
/// ```cpp
/// List<int32, InlineAllocator<8>> values;
/// values.Add(1); // no heap allocation for the first 8 elements
/// ```
template <usize TCount, typename TFallback = HeapAllocator> struct InlineAllocator
{
    /// The policy used once the inline storage is full.
    TFallback Fallback;

    /// The storage of one container type.
    template <typename T> class ForType
    {
    public:
        /// The number of elements stored inside the container.
        static constexpr usize InlineCapacity = TCount;

        /// Creates the policy with the default fallback.
        ForType() = default;
        explicit ForType(const InlineAllocator &policy) : _fallback(policy.Fallback)
        {
        }

        /// Returns the inline storage for up to `TCount` elements, or the fallback's storage.
        [[nodiscard]] T *Allocate(usize capacity)
        {
            return capacity <= TCount ? InlineData() : _fallback.Allocate(capacity);
        }

        /// Frees storage returned by `Allocate`; inline storage is left alone.
        void Free(T *data, usize capacity)
        {
            if (!IsInline(data))
            {
                _fallback.Free(data, capacity);
            }
        }

        /// Returns whether `data` points to the inline storage.
        [[nodiscard]] bool IsInline(const T *data) const
        {
            return data == reinterpret_cast<const T *>(_storage);
        }

    private:
        T *InlineData()
        {
            return reinterpret_cast<T *>(_storage);
        }

        alignas(T) uint8 _storage[TCount * sizeof(T)];
        MARLIN_NO_UNIQUE_ADDRESS TFallback::template ForType<T> _fallback;
    };
};

/// Never grows past `TCount` elements; a request for more is fatal.
template <usize TCount> struct FixedAllocator
{
    /// The fallback policy that fails when the inline storage is full.
    struct Overflow
    {
        template <typename T> class ForType
        {
        public:
            static constexpr usize InlineCapacity = 0;

            constexpr ForType() = default;
            constexpr explicit ForType(const Overflow & /*policy*/)
            {
            }

            [[nodiscard]] T *Allocate(usize /*capacity*/)
            {
                MARLIN_FATAL("FixedAllocator capacity exceeded");
            }

            void Free(T * /*data*/, usize /*capacity*/)
            {
            }

            [[nodiscard]] constexpr bool IsInline(const T * /*data*/) const
            {
                return false;
            }
        };
    };

    /// The storage of one container type.
    template <typename T> using ForType = InlineAllocator<TCount, Overflow>::template ForType<T>;
};

/// Routes container storage through an [`IAllocator`], which must outlive the container.
struct PolymorphicAllocator
{
    /// The allocator that backs the storage; it must outlive the containers that copy it.
    IAllocator *Source = nullptr;

    /// The storage of one container type.
    template <typename T> class ForType
    {
    public:
        /// Polymorphic storage has no inline capacity.
        static constexpr usize InlineCapacity = 0;

        /// Creates the policy with a null source; `Allocate` asserts if the source is null.
        ForType() = default;
        explicit ForType(const PolymorphicAllocator &policy) : _source(policy.Source)
        {
        }

        /// Returns uninitialized storage for `capacity` elements from the source allocator.
        ///
        /// # Preconditions
        /// The source is not null. It is checked by an assertion in builds with assertions.
        [[nodiscard]] T *Allocate(usize capacity)
        {
            MARLIN_ASSERT(_source != nullptr);
            return static_cast<T *>(_source->Allocate(capacity * sizeof(T), Private::ElementAlignment<T>()));
        }

        /// Frees storage returned by `Allocate`; null is ignored.
        void Free(T *data, usize capacity)
        {
            if (data != nullptr)
            {
                _source->Free(data, capacity * sizeof(T));
            }
        }

        /// Returns false, because all storage comes from the source allocator.
        [[nodiscard]] constexpr bool IsInline(const T * /*data*/) const
        {
            return false;
        }

    private:
        IAllocator *_source = nullptr;
    };
};

} // namespace Marlin
