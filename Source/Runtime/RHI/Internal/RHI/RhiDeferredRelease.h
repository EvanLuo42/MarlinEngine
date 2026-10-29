#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiResources.h"

namespace Marlin
{
/// What backends release once the queues are done with it: Destroy calls enqueue with the values the queues had
/// reached in submission, and the submit thread runs the releases whose values have completed. Thread-safe.
class RHI_API RhiDeferredReleaseQueue
{
public:
    using ReleaseFunction = void (*)(void *context, uint64 payload);

    void Enqueue(const RhiQueueValues &after, ReleaseFunction release, void *context, uint64 payload);
    /// Runs the releases whose queue values are all at most *completed*, oldest first.
    void Process(const RhiQueueValues &completed);
    /// Runs every release; the queues must be idle.
    void Flush();

    [[nodiscard]] bool IsEmpty() const;

private:
    struct Entry
    {
        RhiQueueValues After;
        ReleaseFunction Release = nullptr;
        void *Context = nullptr;
        uint64 Payload = 0;
    };

    mutable SpinLock _lock;
    List<Entry> _pending;
    List<Entry> _ready;
};
} // namespace Marlin
