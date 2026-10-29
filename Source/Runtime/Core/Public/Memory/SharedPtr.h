//! Shared ownership pointers: an intrusively counted base, a control-block `SharedPtr`, and a `WeakPtr`.
//!
//! Counts are atomic; the owned object itself is not synchronized.

#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Hash/Hash.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// Base of intrusively counted objects.
///
/// A [`SharedPtr`] to one keeps its count inside the object, so no control block is allocated and a raw pointer can
/// be turned back into a `SharedPtr`. Such objects have no [`WeakPtr`]. The count starts at zero and is not copied by
/// the copy constructor or copy assignment.
class RefCounted
{
public:
    /// Creates an object with zero references.
    RefCounted() = default;
    /// Creates an object with zero references; the count of `other` is not copied.
    RefCounted(const RefCounted & /*other*/)
    {
    }
    /// Leaves the reference count unchanged.
    RefCounted &operator=(const RefCounted & /*other*/)
    {
        return *this;
    }
    RefCounted(RefCounted &&) = delete;
    RefCounted &operator=(RefCounted &&) = delete;

    /// Adds one reference.
    void AddReference() const
    {
        _references.FetchAdd(1, MemoryOrder::Relaxed);
    }

    /// Releases one reference and returns whether it was the last one.
    ///
    /// # Preconditions
    /// The count is greater than zero.
    bool ReleaseReference() const
    {
        return _references.FetchSub(1, MemoryOrder::AcquireRelease) == 1;
    }

    /// Returns the number of strong references to the object.
    [[nodiscard]] uint32 GetReferenceCount() const
    {
        return _references.Load(MemoryOrder::Relaxed);
    }

    /// Destroys the object.
    virtual ~RefCounted() = default;

private:
    template <typename T> friend class SharedPtr;

    mutable Atomic<uint32> _references{0};
};

namespace Private
{
/// The counts and destruction of a non-intrusive shared object. The block lives while weak references remain.
struct SharedControl
{
    Atomic<uint32> Strong{1};
    /// Weak references, plus one held by the strong references together.
    Atomic<uint32> Weak{1};
    void (*DestroyObject)(SharedControl *control) = nullptr;
    void (*FreeControl)(SharedControl *control) = nullptr;

    void AddStrong()
    {
        Strong.FetchAdd(1, MemoryOrder::Relaxed);
    }

    void ReleaseStrong()
    {
        if (Strong.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
        {
            DestroyObject(this);
            ReleaseWeak();
        }
    }

    void AddWeak()
    {
        Weak.FetchAdd(1, MemoryOrder::Relaxed);
    }

    void ReleaseWeak()
    {
        if (Weak.FetchSub(1, MemoryOrder::AcquireRelease) == 1)
        {
            FreeControl(this);
        }
    }

    /// Takes a strong reference unless the object is already gone.
    bool TryAddStrong()
    {
        uint32 count = Strong.Load(MemoryOrder::Relaxed);
        while (count != 0)
        {
            if (Strong.CompareExchangeWeak(count, count + 1, MemoryOrder::Relaxed))
            {
                return true;
            }
        }
        return false;
    }
};

/// Object and control block in one allocation, as MakeShared builds them.
template <typename T> struct SharedInline : SharedControl
{
    alignas(T) uint8 Storage[sizeof(T)];
};

/// A control block for an object allocated on its own.
template <typename T> struct SharedSeparate : SharedControl
{
    T *Object = nullptr;
};
} // namespace Private

template <typename T> class WeakPtr;

/// Shared ownership with thread-safe counts.
///
/// Objects deriving from [`RefCounted`] are counted intrusively; other objects are counted through a control block,
/// which [`MakeShared`] allocates together with the object. The object is destroyed when the last `SharedPtr` is
/// released; a control block stays alive while a [`WeakPtr`] refers to it.
///
/// # Thread safety
/// Copies of the same object may be created and destroyed from different threads. Concurrent access to one
/// `SharedPtr` instance, and to the owned object, is not synchronized.
template <typename T> class SharedPtr
{
    static constexpr bool Intrusive = IsBaseOf<RefCounted, T>;

public:
    /// Creates a null pointer.
    constexpr SharedPtr() = default;

    /// Creates a null pointer from `nullptr`.
    constexpr SharedPtr(decltype(nullptr) /*null*/)
    {
    }

    /// Takes ownership of `object`, which must come from `new`.
    ///
    /// For an intrusive type, this adds a reference instead of allocating a control block; the pointer may already
    /// be owned. For a non-intrusive type, the pointer must not already be owned by another `SharedPtr`.
    explicit SharedPtr(T *object) : _object(object)
    {
        if (object == nullptr)
        {
            return;
        }
        if constexpr (Intrusive)
        {
            object->AddReference();
        }
        else
        {
            auto *control = ConstructAt(
                static_cast<Private::SharedSeparate<T> *>(Memory::Allocate(sizeof(Private::SharedSeparate<T>))));
            control->Object = object;
            control->DestroyObject = [](Private::SharedControl *self) {
                delete static_cast<Private::SharedSeparate<T> *>(self)->Object;
            };
            control->FreeControl = [](Private::SharedControl *self) { Memory::Free(self); };
            _control = control;
        }
    }

    /// Shares ownership with `other`.
    SharedPtr(const SharedPtr &other) : _object(other._object), _control(other._control)
    {
        AddStrong();
    }

    /// Takes ownership from `other`, which becomes null.
    SharedPtr(SharedPtr &&other) noexcept
        : _object(Exchange(other._object, nullptr)), _control(Exchange(other._control, nullptr))
    {
    }

    /// Shares ownership with a `SharedPtr` whose pointer converts to `T*`.
    template <typename TOther>
        requires(IsConvertible<TOther *, T *>)
    SharedPtr(const SharedPtr<TOther> &other) : _object(other._object), _control(other._control)
    {
        AddStrong();
    }

    /// Takes ownership from a `SharedPtr` whose pointer converts to `T*`; `other` becomes null.
    template <typename TOther>
        requires(IsConvertible<TOther *, T *>)
    SharedPtr(SharedPtr<TOther> &&other) noexcept
        : _object(Exchange(other._object, nullptr)), _control(Exchange(other._control, nullptr))
    {
    }

    /// Releases this reference.
    ~SharedPtr()
    {
        ReleaseStrong();
    }

    /// Releases this reference and shares ownership with `other`.
    SharedPtr &operator=(const SharedPtr &other) // NOLINT(bugprone-unhandled-self-assignment)
    {
        SharedPtr(other).Swap(*this);
        return *this;
    }

    /// Releases this reference and takes ownership from `other`, which becomes null.
    SharedPtr &operator=(SharedPtr &&other) noexcept
    {
        SharedPtr(Move(other)).Swap(*this);
        return *this;
    }

    /// Releases this reference and becomes null.
    void Reset()
    {
        SharedPtr().Swap(*this);
    }

    /// Exchanges the owned object and control block with `other`.
    void Swap(SharedPtr &other) noexcept
    {
        Marlin::Swap(_object, other._object);
        Marlin::Swap(_control, other._control);
    }

    /// Returns the owned object, or null.
    [[nodiscard]] T *Get() const
    {
        return _object;
    }

    /// Returns whether an object is owned.
    [[nodiscard]] bool IsValid() const
    {
        return _object != nullptr;
    }

    /// Returns whether an object is owned.
    explicit operator bool() const
    {
        return _object != nullptr;
    }

    /// Returns the owned object.
    ///
    /// # Preconditions
    /// An object is owned. The pointer is checked by an assertion in builds with assertions.
    T &operator*() const
    {
        MARLIN_ASSERT(_object != nullptr);
        return *_object;
    }

    /// Returns the owned pointer.
    ///
    /// # Preconditions
    /// An object is owned. The pointer is checked by an assertion in builds with assertions.
    T *operator->() const
    {
        MARLIN_ASSERT(_object != nullptr);
        return _object;
    }

    /// Returns the number of strong references, including this one; zero when null.
    [[nodiscard]] uint32 GetReferenceCount() const
    {
        if constexpr (Intrusive)
        {
            return _object != nullptr ? _object->GetReferenceCount() : 0;
        }
        else
        {
            return _control != nullptr ? _control->Strong.Load(MemoryOrder::Relaxed) : 0;
        }
    }

    /// Returns whether both own the same object.
    template <typename TOther> bool operator==(const SharedPtr<TOther> &other) const
    {
        return _object == other.Get();
    }

    /// Returns whether no object is owned.
    bool operator==(decltype(nullptr) /*null*/) const
    {
        return _object == nullptr;
    }

    /// Returns a hash of the owned pointer.
    [[nodiscard]] uint64 GetHash() const
    {
        return Marlin::GetHash(_object);
    }

private:
    template <typename TOther> friend class SharedPtr;
    template <typename TOther> friend class WeakPtr;
    template <typename TOther, typename... TArgs> friend SharedPtr<TOther> MakeShared(TArgs &&...args);

    SharedPtr(T *object, Private::SharedControl *control) : _object(object), _control(control)
    {
    }

    void AddStrong() const
    {
        if constexpr (Intrusive)
        {
            if (_object != nullptr)
            {
                _object->AddReference();
            }
        }
        else if (_control != nullptr)
        {
            _control->AddStrong();
        }
    }

    void ReleaseStrong()
    {
        if constexpr (Intrusive)
        {
            if (_object != nullptr && _object->ReleaseReference())
            {
                delete _object;
            }
        }
        else if (_control != nullptr)
        {
            _control->ReleaseStrong();
        }
    }

    T *_object = nullptr;
    Private::SharedControl *_control = nullptr;
};

/// A `SharedPtr` is trivially relocatable, so its storage can be moved without destroying the source.
template <typename T> inline constexpr bool IsTriviallyRelocatable<SharedPtr<T>> = true;

/// Creates a `T` and returns a `SharedPtr` that owns it.
///
/// For an intrusive type, the object is allocated on its own. Otherwise the control block and the object share one
/// allocation. `args` are forwarded to the constructor; allocation failure is fatal.
template <typename T, typename... TArgs> [[nodiscard]] SharedPtr<T> MakeShared(TArgs &&...args)
{
    if constexpr (IsBaseOf<RefCounted, T>)
    {
        return SharedPtr<T>(new T(Forward<TArgs>(args)...));
    }
    else
    {
        using TBlock = Private::SharedInline<T>;
        auto *block = ConstructAt(static_cast<TBlock *>(Memory::Allocate(sizeof(TBlock), alignof(TBlock))));
        T *object = ConstructAt(reinterpret_cast<T *>(block->Storage), Forward<TArgs>(args)...);
        block->DestroyObject = [](Private::SharedControl *self) {
            DestroyAt(reinterpret_cast<T *>(static_cast<TBlock *>(self)->Storage));
        };
        block->FreeControl = [](Private::SharedControl *self) { Memory::Free(self); };
        return SharedPtr<T>(object, block);
    }
}

/// Observes a non-intrusive shared object without keeping it alive.
///
/// A weak reference keeps the control block alive, not the object. [`Lock`] turns it into a strong reference when
/// the object is still alive.
///
/// # Thread safety
/// Weak references may be copied, released and locked from different threads; the object itself is not synchronized.
template <typename T> class WeakPtr
{
    static_assert(!IsBaseOf<RefCounted, T>, "Intrusively counted objects have no weak references");

public:
    /// Creates an empty weak reference.
    constexpr WeakPtr() = default;

    /// Observes the object of `shared` without keeping it alive.
    template <typename TOther>
        requires(IsConvertible<TOther *, T *>)
    WeakPtr(const SharedPtr<TOther> &shared) : _object(shared._object), _control(shared._control)
    {
        if (_control != nullptr)
        {
            _control->AddWeak();
        }
    }

    /// Copies the weak reference.
    WeakPtr(const WeakPtr &other) : _object(other._object), _control(other._control)
    {
        if (_control != nullptr)
        {
            _control->AddWeak();
        }
    }

    /// Moves the weak reference; `other` becomes empty.
    WeakPtr(WeakPtr &&other) noexcept
        : _object(Exchange(other._object, nullptr)), _control(Exchange(other._control, nullptr))
    {
    }

    /// Releases the weak reference.
    ~WeakPtr()
    {
        if (_control != nullptr)
        {
            _control->ReleaseWeak();
        }
    }

    /// Copies the weak reference.
    WeakPtr &operator=(const WeakPtr &other)
    {
        WeakPtr(other).Swap(*this);
        return *this;
    }

    /// Moves the weak reference; `other` becomes empty.
    WeakPtr &operator=(WeakPtr &&other) noexcept
    {
        WeakPtr(Move(other)).Swap(*this);
        return *this;
    }

    /// Exchanges the observed object with `other`.
    void Swap(WeakPtr &other) noexcept
    {
        Marlin::Swap(_object, other._object);
        Marlin::Swap(_control, other._control);
    }

    /// Returns a strong reference, or null if the object is gone.
    [[nodiscard]] SharedPtr<T> Lock() const
    {
        if (_control != nullptr && _control->TryAddStrong())
        {
            return SharedPtr<T>(_object, _control);
        }
        return {};
    }

    /// Returns whether the object is gone or this weak reference is empty.
    [[nodiscard]] bool IsExpired() const
    {
        return _control == nullptr || _control->Strong.Load(MemoryOrder::Relaxed) == 0;
    }

private:
    T *_object = nullptr;
    Private::SharedControl *_control = nullptr;
};
} // namespace Marlin
