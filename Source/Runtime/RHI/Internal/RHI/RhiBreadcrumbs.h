#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "RHI/RhiResources.h"
#include "String/Name.h"

namespace Marlin
{
class StringWriter;

/// GPU progress for crash reports, shared by every backend. Each command list owns a slot, a uint32 in memory the
/// CPU can read after device loss; every marker begin and end is an event with a rising number, and the queue
/// writes the number into the slot once the commands before it are done. After a crash, the markers whose begin
/// is done and whose end is not are the ones the GPU was in.
class RHI_API RhiBreadcrumbs
{
public:
    struct Event
    {
        Name Marker;
        uint32 Depth = 0;
        bool Begin = false;
    };

    /// The events of one command list; event i writes the value i + 1.
    struct Trail
    {
        uint32 Slot = 0;
        RhiQueueType Queue = RhiQueueType::Graphics;
        uint64 SubmitValue = 0;
        bool Submitted = false;
        uint32 Depth = 0;
        List<Event> Events;
    };

    /// *mapped* holds *slotCount* uint32 values, written by the GPU at *address* and read here after a crash.
    RhiBreadcrumbs(uint32 *mapped, uint64 address, uint32 slotCount);
    ~RhiBreadcrumbs();
    MARLIN_NONCOPYABLE(RhiBreadcrumbs);

    /// A trail for a new command list, or null when every slot is in use; any thread.
    [[nodiscard]] Trail *BeginTrail(RhiQueueType queue);
    /// The value the backend writes for the begin event.
    [[nodiscard]] uint32 BeginMarker(Trail &trail, Name marker);
    [[nodiscard]] uint32 EndMarker(Trail &trail);
    [[nodiscard]] uint64 GetSlotAddress(const Trail &trail) const
    {
        return _address + uint64{trail.Slot} * sizeof(uint32);
    }

    void Submitted(Trail &trail, uint64 queueValue);
    /// Frees a trail that was never submitted.
    void Discard(Trail *trail);
    /// Frees the trails of submissions the queues have finished.
    void Retire(const RhiQueueValues &completed);

    /// Describes, per unfinished command list, the markers in progress and how far it got.
    void WriteReport(StringWriter &output) const;

private:
    void Release(Trail *trail);

    uint32 *_mapped;
    uint64 _address;
    mutable SpinLock _lock;
    List<uint32> _freeSlots;
    List<Trail *> _inFlight;
    /// Released trails, kept with their event lists' memory for the next command lists.
    List<Trail *> _freeTrails;
};
} // namespace Marlin
