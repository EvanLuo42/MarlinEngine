#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "Reflection/Reflection.h"
#include "Reflection/TypeOf.h"

namespace Marlin
{
/// A world's singletons, Resource<T> (Documentation/Architecture/05 §3): one value of each reflected type, created
/// on first use. Systems declare their access with SystemBuilder::ReadResource and WriteResource.
class WORLD_API ResourceStore
{
public:
    ResourceStore() = default;
    ~ResourceStore() = default;
    MARLIN_NONCOPYABLE(ResourceStore);

    /// The value of *type*, or null if there is none yet.
    [[nodiscard]] void *Find(const TypeInfo &type) const;
    /// The value of *type*, default-constructed on first use.
    void *GetOrAdd(const TypeInfo &type);
    void Remove(const TypeInfo &type);

    template <typename T> [[nodiscard]] T *Find() const
    {
        return static_cast<T *>(Find(TypeOf<T>()));
    }

    template <typename T> T &GetOrAdd()
    {
        return *static_cast<T *>(GetOrAdd(TypeOf<T>()));
    }

private:
    HashMap<const TypeInfo *, UniquePtr<ReflectedValue>> _resources;
};

class WORLD_API EventStreamBase
{
public:
    EventStreamBase() = default;
    virtual ~EventStreamBase() = default;
    MARLIN_NONCOPYABLE(EventStreamBase);

    /// Ends a frame of the stream: the current events become the previous ones.
    virtual void Update() = 0;
};

/// A double-buffered stream of events of one type, such as collisions or damage (Documentation/Architecture/05 §4):
/// an event sent during one update can be read during that update and the next.
template <typename T> class Events final : public EventStreamBase
{
public:
    /// Thread-safe, so systems running in parallel may send.
    void Send(const T &event)
    {
        const ScopeLock lock(_lock);
        _current.Add(event);
    }

    [[nodiscard]] Span<const T> GetPrevious() const
    {
        return _previous.AsSpan();
    }

    [[nodiscard]] Span<const T> GetCurrent() const
    {
        return _current.AsSpan();
    }

    /// Visits the previous events, then the current ones.
    template <typename TFunction> void ForEach(TFunction &&function) const
    {
        for (const T &event : _previous)
        {
            function(event);
        }
        for (const T &event : _current)
        {
            function(event);
        }
    }

    void Update() override
    {
        _previous = Move(_current);
        _current.Clear();
    }

private:
    SpinLock _lock;
    List<T> _previous;
    List<T> _current;
};

/// A world's event streams, one per event type, created on first use.
class WORLD_API EventStore
{
public:
    EventStore() = default;
    ~EventStore() = default;
    MARLIN_NONCOPYABLE(EventStore);

    template <typename T> Events<T> &Get()
    {
        const ScopeLock lock(_lock);
        UniquePtr<EventStreamBase> *stream = _streams.Find(&TypeOf<T>());
        if (stream == nullptr)
        {
            stream = &_streams.Add(&TypeOf<T>(), MakeUnique<Events<T>>());
        }
        return static_cast<Events<T> &>(*stream->Get());
    }

    /// Advances every stream; the world calls it once a frame.
    void Update();

private:
    SpinLock _lock;
    HashMap<const TypeInfo *, UniquePtr<EventStreamBase>> _streams;
};
} // namespace Marlin
