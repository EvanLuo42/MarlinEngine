#include "RHI/RhiBreadcrumbs.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
const char *QueueName(RhiQueueType queue)
{
    switch (queue)
    {
    case RhiQueueType::Graphics:
        return "Graphics";
    case RhiQueueType::Compute:
        return "Compute";
    case RhiQueueType::Copy:
        return "Copy";
    }
    return "Unknown";
}
} // namespace

RhiBreadcrumbs::RhiBreadcrumbs(uint32 *mapped, uint64 address, uint32 slotCount) : _mapped(mapped), _address(address)
{
    _freeSlots.Reserve(slotCount);
    for (uint32 slot = slotCount; slot-- > 0;)
    {
        _freeSlots.Add(slot);
    }
}

RhiBreadcrumbs::~RhiBreadcrumbs()
{
    for (Trail *trail : _inFlight)
    {
        delete trail;
    }
    for (Trail *trail : _freeTrails)
    {
        delete trail;
    }
}

RhiBreadcrumbs::Trail *RhiBreadcrumbs::BeginTrail(RhiQueueType queue)
{
    uint32 slot = 0;
    Trail *trail = nullptr;
    {
        const ScopeLock lock(_lock);
        if (_freeSlots.IsEmpty())
        {
            return nullptr;
        }
        slot = _freeSlots.Pop();
        if (!_freeTrails.IsEmpty())
        {
            trail = _freeTrails.Pop();
        }
    }
    __atomic_store_n(&_mapped[slot], 0u, __ATOMIC_RELAXED);
    if (trail == nullptr)
    {
        trail = new Trail;
        trail->Events.Reserve(64);
    }
    trail->Slot = slot;
    trail->Queue = queue;
    return trail;
}

uint32 RhiBreadcrumbs::BeginMarker(Trail &trail, Name marker)
{
    trail.Events.Add({marker, trail.Depth++, true});
    return static_cast<uint32>(trail.Events.Count());
}

uint32 RhiBreadcrumbs::EndMarker(Trail &trail)
{
    MARLIN_ASSERT(trail.Depth > 0);
    trail.Events.Add({Name(), --trail.Depth, false});
    return static_cast<uint32>(trail.Events.Count());
}

void RhiBreadcrumbs::Submitted(Trail &trail, uint64 queueValue)
{
    trail.SubmitValue = queueValue;
    trail.Submitted = true;
    const ScopeLock lock(_lock);
    _inFlight.Add(&trail);
}

void RhiBreadcrumbs::Discard(Trail *trail)
{
    Release(trail);
}

void RhiBreadcrumbs::Release(Trail *trail)
{
    trail->SubmitValue = 0;
    trail->Submitted = false;
    trail->Depth = 0;
    trail->Events.Clear();
    const ScopeLock lock(_lock);
    _freeSlots.Add(trail->Slot);
    _freeTrails.Add(trail);
}

void RhiBreadcrumbs::Retire(const RhiQueueValues &completed)
{
    List<Trail *> done;
    {
        const ScopeLock lock(_lock);
        usize kept = 0;
        for (usize i = 0; i < _inFlight.Count(); ++i)
        {
            Trail *trail = _inFlight[i];
            if (trail->SubmitValue <= completed.Values[static_cast<uint32>(trail->Queue)])
            {
                done.Add(trail);
            }
            else
            {
                _inFlight[kept++] = trail;
            }
        }
        _inFlight.Resize(kept);
    }
    for (Trail *trail : done)
    {
        Release(trail);
    }
}

void RhiBreadcrumbs::WriteReport(StringWriter &output) const
{
    const ScopeLock lock(_lock);
    for (const Trail *trail : _inFlight)
    {
        const uint32 reached = __atomic_load_n(&_mapped[trail->Slot], __ATOMIC_RELAXED);
        if (reached >= trail->Events.Count())
        {
            continue;
        }
        FormatTo(output, "{0} queue submission {1}: {2} of {3} marker events done\n", QueueName(trail->Queue),
                 trail->SubmitValue, reached, trail->Events.Count());
        List<Name> open;
        for (uint32 i = 0; i < reached; ++i)
        {
            const Event &event = trail->Events[i];
            if (event.Begin)
            {
                open.Add(event.Marker);
            }
            else if (!open.IsEmpty())
            {
                open.Pop();
            }
        }
        for (usize depth = 0; depth < open.Count(); ++depth)
        {
            output.Append(depth * 2 + 2, ' ');
            FormatTo(output, "in {0}\n", open[depth]);
        }
        for (uint32 i = reached; i < trail->Events.Count(); ++i)
        {
            const Event &event = trail->Events[i];
            if (event.Begin)
            {
                output.Append(event.Depth * 2 + 2, ' ');
                FormatTo(output, "not started: {0}\n", event.Marker);
                break;
            }
        }
    }
}
} // namespace Marlin
