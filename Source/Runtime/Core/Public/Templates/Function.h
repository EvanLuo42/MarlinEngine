//! Owning and non-owning callables: `Function` stores a callable inline or on the heap, `FunctionRef` borrows one.

#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// An owning, move-only callable with a fixed signature.
///
/// `TSignature` is a function type such as `int32(int32)`. A callable that fits in `TInlineSize` bytes and needs
/// no more alignment than `void *` is stored inside the object; a larger or more aligned callable is allocated
/// on the heap with [Memory::Allocate] and freed in the destructor. Copying is disabled; moving leaves the
/// source empty.
///
/// # Thread safety
/// The wrapper is not synchronized. `operator()` is `const` but invokes the stored callable as a non-const
/// object, so concurrent calls are safe only when the callable itself is.
///
/// # Examples
///
/// ```cpp
/// Function<int32(int32)> twice = [](int32 value) { return value * 2; };
/// int32 result = twice(21); // -> 42
/// Function<int32(int32)> moved = Move(twice);
/// bool empty = !twice; // -> true
/// ```
template <typename TSignature, usize TInlineSize = 3 * sizeof(void *)> class Function;

template <typename TResult, typename... TArgs, usize TInlineSize> class Function<TResult(TArgs...), TInlineSize>
{
public:
    /// Creates an empty function.
    Function() = default;

    /// Creates an empty function from `nullptr`.
    Function(decltype(nullptr) /*null*/)
    {
    }

    /// Creates a function that owns `callable`.
    ///
    /// The callable is stored inline when it fits and its alignment allows; otherwise it is constructed on the
    /// heap.
    /// `TCallable` must be invocable with `TArgs...` and its result must convert to `TResult`; the constructor's
    /// constraint enforces that at compile time.
    template <typename TCallable>
        requires(!IsSame<RemoveCvRef<TCallable>, Function> && InvocableReturning<Decay<TCallable> &, TResult, TArgs...>)
    Function(TCallable &&callable)
    {
        using TStored = Decay<TCallable>;
        if constexpr (sizeof(TStored) <= TInlineSize && alignof(TStored) <= alignof(void *))
        {
            ConstructAt(reinterpret_cast<TStored *>(_storage), Forward<TCallable>(callable));
            _operations = &InlineOperations<TStored>;
        }
        else
        {
            auto *stored = static_cast<TStored *>(Memory::Allocate(sizeof(TStored), alignof(TStored)));
            ConstructAt(stored, Forward<TCallable>(callable));
            *reinterpret_cast<TStored **>(_storage) = stored;
            _operations = &HeapOperations<TStored>;
        }
    }

    Function(const Function &) = delete;
    Function &operator=(const Function &) = delete;

    /// Moves the callable out of `other`, leaving `other` empty.
    Function(Function &&other) noexcept : _operations(other._operations)
    {
        if (_operations != nullptr)
        {
            _operations->Relocate(_storage, other._storage);
            other._operations = nullptr;
        }
    }

    /// Replaces the stored callable with the one in `other`, leaving `other` empty.
    ///
    /// Assigning a function to itself does nothing.
    Function &operator=(Function &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            _operations = other._operations;
            if (_operations != nullptr)
            {
                _operations->Relocate(_storage, other._storage);
                other._operations = nullptr;
            }
        }
        return *this;
    }

    /// Destroys the stored callable.
    ~Function()
    {
        Reset();
    }

    /// Destroys the stored callable; afterwards the function is empty.
    void Reset()
    {
        if (_operations != nullptr)
        {
            _operations->Destroy(_storage);
            _operations = nullptr;
        }
    }

    /// Returns whether a callable is stored.
    [[nodiscard]] bool IsBound() const
    {
        return _operations != nullptr;
    }

    /// Returns whether a callable is stored.
    explicit operator bool() const
    {
        return _operations != nullptr;
    }

    /// Invokes the stored callable with `args` and returns its result.
    ///
    /// # Preconditions
    /// A callable must be stored. The check is `MARLIN_ASSERT`, so calling an empty function is undefined
    /// behavior in Profile and Shipping.
    TResult operator()(TArgs... args) const
    {
        MARLIN_ASSERT(_operations != nullptr);
        return _operations->Invoke(_storage, Forward<TArgs>(args)...);
    }

private:
    struct Operations
    {
        TResult (*Invoke)(void *storage, TArgs &&...args);
        void (*Relocate)(void *target, void *source);
        void (*Destroy)(void *storage);
    };

    template <typename TStored>
    static constexpr Operations InlineOperations = {
        [](void *storage, TArgs &&...args) -> TResult {
            return (*static_cast<TStored *>(storage))(Forward<TArgs>(args)...);
        },
        [](void *target, void *source) {
            ConstructAt(static_cast<TStored *>(target), Move(*static_cast<TStored *>(source)));
            DestroyAt(static_cast<TStored *>(source));
        },
        [](void *storage) { DestroyAt(static_cast<TStored *>(storage)); },
    };

    template <typename TStored>
    static constexpr Operations HeapOperations = {
        [](void *storage, TArgs &&...args) -> TResult {
            return (**static_cast<TStored **>(storage))(Forward<TArgs>(args)...);
        },
        [](void *target, void *source) { *static_cast<TStored **>(target) = *static_cast<TStored **>(source); },
        [](void *storage) {
            TStored *stored = *static_cast<TStored **>(storage);
            DestroyAt(stored);
            Memory::Free(stored);
        },
    };

    alignas(void *) mutable uint8 _storage[TInlineSize < sizeof(void *) ? sizeof(void *) : TInlineSize] = {};
    const Operations *_operations = nullptr;
};

/// A non-owning callable reference with a fixed signature.
///
/// `TSignature` is a function type such as `int32(int32)`. The reference stores a pointer to a callable and an
/// invoker, so it costs two pointers, copies freely, and never allocates. It must not outlive the callable it
/// refers to.
///
/// # Safety
/// The referenced callable must stay alive while the reference is used. Constructing a `FunctionRef` from a
/// temporary leaves it dangling at the end of the full expression.
///
/// # Thread safety
/// `operator()` is `const` but invokes the callable as a non-const object, so concurrent calls are safe only
/// when the callable itself is.
///
/// # Examples
///
/// ```cpp
/// auto add = [](int32 a, int32 b) { return a + b; };
/// FunctionRef<int32(int32, int32)> reference = add;
/// int32 sum = reference(2, 3); // -> 5
/// ```
template <typename TSignature> class FunctionRef;

template <typename TResult, typename... TArgs> class FunctionRef<TResult(TArgs...)>
{
public:
    /// Binds `callable`, which must outlive this reference.
    ///
    /// `TCallable` must be invocable with `TArgs...` and its result must convert to `TResult`; the constructor's
    /// constraint enforces that at compile time.
    template <typename TCallable>
        requires(!IsSame<RemoveCvRef<TCallable>, FunctionRef> && InvocableReturning<TCallable &, TResult, TArgs...>)
    FunctionRef(TCallable &&callable)
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        : _object(const_cast<void *>(static_cast<const void *>(AddressOf(callable)))),
          _invoke([](void *object, TArgs &&...args) -> TResult {
              return (*static_cast<RemoveReference<TCallable> *>(object))(Forward<TArgs>(args)...);
          })
    {
    }

    /// Invokes the referenced callable with `args` and returns its result.
    TResult operator()(TArgs... args) const
    {
        return _invoke(_object, Forward<TArgs>(args)...);
    }

private:
    void *_object;
    TResult (*_invoke)(void *object, TArgs &&...args);
};
} // namespace Marlin
