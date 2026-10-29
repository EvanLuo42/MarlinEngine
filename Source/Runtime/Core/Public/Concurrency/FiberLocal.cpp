#include "Concurrency/FiberLocal.h"

#include "Diagnostics/Assert.h"

namespace Marlin::Private
{
namespace
{
/// Slot 0 means unallocated in FiberLocal, so allocation starts at 1.
Atomic<uint32> _nextFiberLocalSlot{1};
} // namespace

uint32 AllocateFiberLocalSlot()
{
    const uint32 slot = _nextFiberLocalSlot.FetchAdd(1);
    MARLIN_ASSERT(slot < ExecutionContext::MaxFiberLocals);
    return slot;
}

uint32 GetFiberLocalSlotCount()
{
    const uint32 count = _nextFiberLocalSlot.Load(MemoryOrder::Relaxed);
    return count < ExecutionContext::MaxFiberLocals ? count : ExecutionContext::MaxFiberLocals;
}
} // namespace Marlin::Private
