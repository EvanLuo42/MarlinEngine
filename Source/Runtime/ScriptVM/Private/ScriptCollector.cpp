#include "ScriptCollector.h"

#include <initializer_list>

#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "ScriptInterpreter.h"

namespace Marlin
{
namespace
{
// The old generation's size classes, as ScriptHeap lays out its blocks.
constexpr uint32 SizeClassBytes[] = {16,   32,   48,   64,   80,   96,   128,  160,  192,  256,  320,  384, 512,
                                     640,  768,  1024, 1280, 1536, 2048, 2560, 3072, 4096, 5120, 6144, 8192};

constexpr float64 NoDeadline = 1.0e300;

/// The object's type even when the object has moved and its header holds the new address.
const ScriptType *RealTypeOf(const ScriptObject *object)
{
    return (object->Gc & ScriptGcBits::Forwarded) != 0 ? reinterpret_cast<const ScriptObject *>(object->Type)->Type
                                                       : object->Type;
}

bool IsIteratorOwner(const ScriptObject *owner)
{
    return owner != nullptr && RealTypeOf(owner)->Builtin == ScriptBuiltin::Iterator;
}

} // namespace

usize ScriptObjectBytes(const ScriptObject *object)
{
    const ScriptType &type = *object->Type;
    usize bytes = type.InstanceSize;
    if (type.Kind == ScriptTypeKind::Array || type.Kind == ScriptTypeKind::String)
    {
        bytes = usize{type.DataOffset} + usize{static_cast<const ScriptArray *>(object)->Length} * type.ElementStride;
    }
    if (bytes < ScriptObjectHeaderBytes)
    {
        bytes = ScriptObjectHeaderBytes;
    }
    return (bytes + 15) & ~usize{15};
}

ScriptCollector::ScriptCollector(ScriptDomain &domain, ScriptHeap &heap) : _domain(&domain), _heap(&heap)
{
}

void ScriptCollector::Shade(ScriptObject *previous)
{
    if (_phase == Phase::Marking && previous != nullptr && _heap->IsOld(previous) && !IsMarked(previous))
    {
        Push(previous);
    }
}

void ScriptCollector::Push(ScriptObject *object)
{
    object->Gc = (object->Gc & ~ScriptGcBits::Marked) | _markColor;
    _gray.Add(object);
}

void ScriptCollector::VisitRoots(ScriptRootVisitor &visitor)
{
    if (_domain->_interpreter != nullptr)
    {
        _domain->_interpreter->VisitRoots(visitor);
    }
    for (const UniquePtr<ScriptLoadedModule> &module : _domain->_modules)
    {
        for (const uint32 offset : module->StaticReferences)
        {
            visitor.VisitObject(reinterpret_cast<ScriptObject **>(module->Statics + offset));
        }
    }
    for (ScriptObject *&handle : _domain->_handles)
    {
        visitor.VisitObject(&handle);
    }
}

/// Copies the live young objects to the other survivor space or into the old generation.
class ScriptCollector::Forwarder final : public ScriptRootVisitor
{
public:
    Forwarder(ScriptCollector &collector, bool tenure)
        : _collector(&collector), _heap(collector._heap), _tenure(tenure)
    {
        _toTop = _heap->_survivor[1 - _heap->_survivorIndex];
        _toEnd = _toTop + _heap->_survivorBytes;
    }

    void VisitObject(ScriptObject **reference) override
    {
        if (*reference != nullptr && IsFromSpace(*reference))
        {
            *reference = Forward(*reference);
        }
    }

    void VisitManaged(uint8 **pointer) override
    {
        uint8 *address = *pointer;
        if (!IsFromSpace(address))
        {
            return;
        }
        ScriptObject *object = FindStart(address);
        if (object == nullptr)
        {
            return;
        }
        ScriptObject *moved = Forward(object);
        *pointer = reinterpret_cast<uint8 *>(moved) + (address - reinterpret_cast<uint8 *>(object));
    }

    ScriptObject *Forward(ScriptObject *object)
    {
        if ((object->Gc & ScriptGcBits::Forwarded) != 0)
        {
            return reinterpret_cast<ScriptObject *>(const_cast<ScriptType *>(object->Type));
        }
        const usize bytes = ScriptObjectBytes(object);
        const uint64 age = (object->Gc & ScriptGcBits::AgeMask) >> ScriptGcBits::AgeShift;
        uint8 *target = nullptr;
        bool old = false;
        // RECOVERY-DECISION SC2: objects are tenured after surviving two young collections, when the survivor space
        // is full, or when a full collection empties the young generation.
        if (_tenure || age + 1 >= 2 || _toTop + bytes > _toEnd)
        {
            target = _heap->AllocateOld(bytes);
            old = target != nullptr;
        }
        if (target == nullptr)
        {
            if (_toTop + bytes > _toEnd)
            {
                MARLIN_FATAL("the script heap is exhausted during a collection");
            }
            target = _toTop;
            _toTop += bytes;
            _heap->SetYoungStart(target);
        }
        MemCopy(target, object, bytes);
        ScriptObject *moved = reinterpret_cast<ScriptObject *>(target);
        if (old)
        {
            moved->Gc = (moved->Gc & ~(ScriptGcBits::AgeMask | ScriptGcBits::Marked)) | ScriptGcBits::Old |
                        _collector->OldMark();
        }
        else
        {
            moved->Gc = (moved->Gc & ~ScriptGcBits::AgeMask) | ((age + 1) << ScriptGcBits::AgeShift);
        }
        object->Type = reinterpret_cast<const ScriptType *>(moved);
        object->Gc |= ScriptGcBits::Forwarded;
        _work.Add(moved);
        return moved;
    }

    /// Forwards the young objects the copied ones reference, keeping the cards of old objects that still do.
    void Drain()
    {
        while (!_work.IsEmpty())
        {
            ScanObject(_work.Pop());
        }
    }

    void ScanObject(ScriptObject *object)
    {
        const bool old = _heap->IsOld(object);
        ForEachScriptReference(object, [&](ScriptObject **reference) {
            VisitObject(reference);
            if (old && *reference != nullptr && _heap->IsYoung(*reference))
            {
                _heap->MarkCard(reference);
            }
        });
    }

    [[nodiscard]] uint8 *GetToTop() const
    {
        return _toTop;
    }

private:
    /// Whether *address* is in the nursery or the survivor space being evacuated: a slot visited a second time
    /// already points into to-space and must not be copied again.
    [[nodiscard]] bool IsFromSpace(const void *address) const
    {
        const uint8 *at = static_cast<const uint8 *>(address);
        const uint8 *survivor = _heap->_survivor[_heap->_survivorIndex];
        return (at >= _heap->_edenBegin && at < _heap->_edenEnd) ||
               (at >= survivor && at < survivor + _heap->_survivorBytes);
    }

    /// The young object holding *address*, found by the heap's map of young object starts.
    ScriptObject *FindStart(uint8 *address) const
    {
        uint8 *begin = nullptr;
        if (address >= _heap->_edenBegin && address < _heap->_edenTop)
        {
            begin = _heap->_edenBegin;
        }
        else if (address >= _heap->_survivor[_heap->_survivorIndex] && address < _heap->_survivorTop)
        {
            begin = _heap->_survivor[_heap->_survivorIndex];
        }
        if (begin == nullptr)
        {
            return nullptr;
        }
        auto *object = reinterpret_cast<ScriptObject *>(_heap->FindYoungStart(address, begin));
        if (object == nullptr)
        {
            return nullptr;
        }
        const ScriptObject *sized =
            (object->Gc & ScriptGcBits::Forwarded) != 0 ? reinterpret_cast<const ScriptObject *>(object->Type) : object;
        return address < reinterpret_cast<uint8 *>(object) + ScriptObjectBytes(sized) ? object : nullptr;
    }

    ScriptCollector *_collector;
    ScriptHeap *_heap;
    bool _tenure;
    uint8 *_toTop = nullptr;
    uint8 *_toEnd = nullptr;
    List<ScriptObject *> _work;
};

/// Shades what the roots reference at the start of a marking cycle.
class ScriptCollector::Marker final : public ScriptRootVisitor
{
public:
    explicit Marker(ScriptCollector &collector) : _collector(&collector), _heap(collector._heap)
    {
    }

    void VisitObject(ScriptObject **reference) override
    {
        Mark(*reference);
    }

    void VisitManaged(uint8 **pointer) override
    {
        uint8 *address = *pointer;
        if (!_heap->IsOld(address))
        {
            return;
        }
        const usize offset = static_cast<usize>(address - _heap->_oldBase);
        const usize block = offset / ScriptHeap::BlockBytes;
        if (block >= _heap->_blockHighWater)
        {
            return;
        }
        usize first = block;
        const ScriptHeap::BlockInfo &info = _heap->_blocks[block];
        uint8 *start = nullptr;
        if (info.Kind == 1)
        {
            const usize cell = SizeClassBytes[info.SizeClass];
            const usize inBlock = offset % ScriptHeap::BlockBytes;
            start = _heap->_oldBase + block * ScriptHeap::BlockBytes + inBlock / cell * cell;
        }
        else if (info.Kind == 2 || info.Kind == 3)
        {
            while (_heap->_blocks[first].Kind == 3 && first > 0)
            {
                --first;
            }
            start = _heap->_oldBase + first * ScriptHeap::BlockBytes;
        }
        if (start == nullptr)
        {
            return;
        }
        const usize bit = static_cast<usize>(start - _heap->_oldBase) / 16;
        if ((_heap->_starts[bit / 64] & (uint64{1} << (bit % 64))) != 0)
        {
            Mark(reinterpret_cast<ScriptObject *>(start));
        }
    }

    void Mark(ScriptObject *object)
    {
        if (object != nullptr && _heap->IsOld(object) && !_collector->IsMarked(object))
        {
            _collector->Push(object);
        }
    }

private:
    ScriptCollector *_collector;
    ScriptHeap *_heap;
};

void ScriptCollector::CollectYoung()
{
    CollectYoung(false);
}

void ScriptCollector::CollectYoung(bool tenure)
{
    Forwarder forwarder(*this, tenure);
    VisitRoots(forwarder);
    // Old objects on dirty cards may reference young ones: their fields are roots too.
    ScriptObject *scanned = nullptr;
    const usize cards = _heap->_cards.Count();
    for (usize card = 0; card < cards; ++card)
    {
        // Clean cards are skipped eight at a time.
        if (card % 8 == 0 && card + 8 <= cards)
        {
            uint64 eight;
            MemCopy(&eight, &_heap->_cards[card], sizeof(eight));
            if (eight == 0)
            {
                card += 7;
                continue;
            }
        }
        if (_heap->_cards[card] == 0)
        {
            continue;
        }
        _heap->_cards[card] = 0;
        const usize cardOffset = card * ScriptHeap::CardBytes;
        const usize block = cardOffset / ScriptHeap::BlockBytes;
        const ScriptHeap::BlockInfo &info = _heap->_blocks[block];
        uint8 *blockBase = _heap->_oldBase + block * ScriptHeap::BlockBytes;
        auto scan = [&](uint8 *start) {
            const usize bit = static_cast<usize>(start - _heap->_oldBase) / 16;
            ScriptObject *object = reinterpret_cast<ScriptObject *>(start);
            if (object != scanned && (_heap->_starts[bit / 64] & (uint64{1} << (bit % 64))) != 0)
            {
                scanned = object;
                forwarder.ScanObject(object);
            }
        };
        if (info.Kind == 1)
        {
            const usize cell = SizeClassBytes[info.SizeClass];
            const usize cells = ScriptHeap::BlockBytes / cell;
            const usize inBlock = cardOffset % ScriptHeap::BlockBytes;
            for (usize i = inBlock / cell; i < cells && i * cell < inBlock + ScriptHeap::CardBytes; ++i)
            {
                scan(blockBase + i * cell);
            }
        }
        else if (info.Kind == 2 || info.Kind == 3)
        {
            usize first = block;
            while (_heap->_blocks[first].Kind == 3 && first > 0)
            {
                --first;
            }
            scan(_heap->_oldBase + first * ScriptHeap::BlockBytes);
        }
    }
    forwarder.Drain();
    UpdateWeakRefsAfterYoung();

    // Iterators are weak owners of their coroutines: a coroutine whose iterator died is freed.
    ScriptInterpreter *interpreter = _domain->_interpreter.Get();
    if (interpreter != nullptr)
    {
        for (usize i = 0; i < interpreter->_coroutines.Count(); ++i)
        {
            ScriptCoroutine *coroutine = interpreter->_coroutines[i].Get();
            if (coroutine == nullptr || coroutine->Owner == nullptr || !_heap->IsYoung(coroutine->Owner) ||
                !IsIteratorOwner(coroutine->Owner))
            {
                continue;
            }
            if ((coroutine->Owner->Gc & ScriptGcBits::Forwarded) != 0)
            {
                coroutine->Owner = reinterpret_cast<ScriptObject *>(const_cast<ScriptType *>(coroutine->Owner->Type));
            }
            else
            {
                coroutine->Owner = nullptr;
                interpreter->FreeCoroutine(static_cast<uint32>(i));
            }
        }
    }

    _heap->ClearYoungStarts(_heap->_edenBegin, _heap->_edenTop);
    _heap->ClearYoungStarts(_heap->_survivor[_heap->_survivorIndex], _heap->_survivorTop);
    _heap->_edenTop = _heap->_edenBegin;
    _heap->_survivorIndex = 1 - _heap->_survivorIndex;
    _heap->_survivorTop = forwarder.GetToTop();
    ++_domain->_collections;
}

void ScriptCollector::BeginMarking()
{
    CollectYoung(true);
    // RECOVERY-DECISION SC3: the mark bit's meaning flips each cycle, so marking never has to clear it: what the last
    // cycle marked reads as unmarked now.
    _markColor ^= ScriptGcBits::Marked;
    _gray.Clear();
    _liveBytes = 0;
    _phase = Phase::Marking;
    Marker marker(*this);
    VisitRoots(marker);
}

bool ScriptCollector::MarkSome(float64 deadline)
{
    Marker marker(*this);
    uint32 counter = 0;
    while (!_gray.IsEmpty())
    {
        ScriptObject *object = _gray.Pop();
        _liveBytes += ScriptObjectBytes(object);
        ForEachScriptReference(object, [&](ScriptObject **reference) { marker.Mark(*reference); });
        if (++counter % 256 == 0 && PlatformTime::Seconds() >= deadline)
        {
            return _gray.IsEmpty();
        }
    }
    return true;
}

void ScriptCollector::UpdateWeakRefsAfterYoung()
{
    List<ScriptObject *> &weakRefs = _domain->_weakRefs;
    for (usize i = 0; i < weakRefs.Count();)
    {
        ScriptObject *weakRef = weakRefs[i];
        if (_heap->IsYoung(weakRef))
        {
            if ((weakRef->Gc & ScriptGcBits::Forwarded) == 0)
            {
                weakRefs.RemoveAtSwap(i);
                continue;
            }
            weakRef = reinterpret_cast<ScriptObject *>(const_cast<ScriptType *>(weakRef->Type));
            weakRefs[i] = weakRef;
        }
        ScriptObject *&target = StateOf<ScriptWeakRefState>(weakRef).Target;
        if (target != nullptr && _heap->IsYoung(target))
        {
            target = (target->Gc & ScriptGcBits::Forwarded) != 0
                         ? reinterpret_cast<ScriptObject *>(const_cast<ScriptType *>(target->Type))
                         : nullptr;
        }
        ++i;
    }
}

void ScriptCollector::ClearUnmarkedWeakRefs()
{
    List<ScriptObject *> &weakRefs = _domain->_weakRefs;
    for (usize i = 0; i < weakRefs.Count();)
    {
        ScriptObject *weakRef = weakRefs[i];
        if (_heap->IsOld(weakRef) && !IsMarked(weakRef))
        {
            weakRefs.RemoveAtSwap(i);
            continue;
        }
        ScriptObject *&target = StateOf<ScriptWeakRefState>(weakRef).Target;
        if (target != nullptr && _heap->IsOld(target) && !IsMarked(target))
        {
            target = nullptr;
        }
        ++i;
    }
}

bool ScriptCollector::SweepSome(float64 deadline)
{
    ScriptHeap &heap = *_heap;
    auto isLive = [&](usize bit) { return (heap._starts[bit / 64] & (uint64{1} << (bit % 64))) != 0; };
    while (_sweepCursor < heap._blockHighWater)
    {
        const usize block = _sweepCursor;
        const ScriptHeap::BlockInfo info = heap._blocks[block];
        uint8 *blockBase = heap._oldBase + block * ScriptHeap::BlockBytes;
        if (info.Kind == 1)
        {
            const usize cell = SizeClassBytes[info.SizeClass];
            const usize cells = ScriptHeap::BlockBytes / cell;
            for (usize i = 0; i < cells; ++i)
            {
                uint8 *at = blockBase + i * cell;
                const usize bit = static_cast<usize>(at - heap._oldBase) / 16;
                if (!isLive(bit) || IsMarked(reinterpret_cast<ScriptObject *>(at)))
                {
                    continue;
                }
                heap._starts[bit / 64] &= ~(uint64{1} << (bit % 64));
                // RECOVERY-DECISION SC4: freed cells go back on their class's free list; blocks of small cells stay
                // with their class even when all their cells are free.
                MemCopy(at, &heap._freeCells[info.SizeClass], sizeof(uint8 *));
                heap._freeCells[info.SizeClass] = at;
                heap._oldBytes -= cell;
            }
            ++_sweepCursor;
        }
        else if (info.Kind == 2)
        {
            const usize bit = static_cast<usize>(blockBase - heap._oldBase) / 16;
            if (isLive(bit) && !IsMarked(reinterpret_cast<ScriptObject *>(blockBase)))
            {
                heap._starts[bit / 64] &= ~(uint64{1} << (bit % 64));
                heap.FreeBlocks(block, info.Run);
            }
            _sweepCursor += info.Run > 0 ? info.Run : 1;
        }
        else
        {
            ++_sweepCursor;
        }
        if (PlatformTime::Seconds() >= deadline)
        {
            return _sweepCursor >= heap._blockHighWater;
        }
    }
    return true;
}

void ScriptCollector::CollectFull()
{
    // Finishes the cycle in progress, then runs a whole new one, so everything dead now is freed.
    Step(NoDeadline);
    if (_phase == Phase::Sweeping)
    {
        Step(NoDeadline);
    }
    BeginMarking();
    while (_phase != Phase::Idle)
    {
        Step(NoDeadline);
    }
}

void ScriptCollector::Step(float64 seconds)
{
    const float64 deadline = seconds >= NoDeadline ? NoDeadline : PlatformTime::Seconds() + seconds;
    if (_phase == Phase::Idle)
    {
        if (_heap->_oldBytes < _trigger)
        {
            return;
        }
        BeginMarking();
    }
    if (_phase == Phase::Marking)
    {
        if (!MarkSome(deadline))
        {
            return;
        }
        ClearUnmarkedWeakRefs();
        ScriptInterpreter *interpreter = _domain->_interpreter.Get();
        if (interpreter != nullptr)
        {
            for (usize i = 0; i < interpreter->_coroutines.Count(); ++i)
            {
                ScriptCoroutine *coroutine = interpreter->_coroutines[i].Get();
                if (coroutine != nullptr && coroutine->Owner != nullptr && _heap->IsOld(coroutine->Owner) &&
                    !IsMarked(coroutine->Owner) && IsIteratorOwner(coroutine->Owner))
                {
                    coroutine->Owner = nullptr;
                    interpreter->FreeCoroutine(static_cast<uint32>(i));
                }
            }
        }
        _phase = Phase::Sweeping;
        _sweepCursor = 0;
    }
    if (_phase == Phase::Sweeping && SweepSome(deadline))
    {
        _phase = Phase::Idle;
        // RECOVERY-DECISION SC5: the next cycle starts when the old generation has doubled since this one, and not
        // before 8 MiB.
        const usize minimum = usize{8} << 20;
        _trigger = _heap->_oldBytes * 2 > minimum ? _heap->_oldBytes * 2 : minimum;
        ++_domain->_collections;
    }
}

void ScriptDomain::Collect(bool full)
{
    _collectionRequested = false;
    _fullCollectionRequested = false;
    if (full)
    {
        _collector->CollectFull();
    }
    else
    {
        _collector->CollectYoung();
    }
}

void ScriptDomain::StepCollector()
{
    _collector->Step(_options.GcBudgetSeconds);
}

uint32 ScriptDomain::CreateHandle(ScriptObject *object)
{
    if (!_freeHandles.IsEmpty())
    {
        const uint32 handle = _freeHandles.Pop();
        _handles[handle] = object;
        return handle;
    }
    _handles.Add(object);
    return static_cast<uint32>(_handles.Count() - 1);
}

void ScriptDomain::DestroyHandle(uint32 handle)
{
    if (handle < _handles.Count())
    {
        _handles[handle] = nullptr;
        _freeHandles.Add(handle);
    }
}

void ScriptDomain::WriteReference(ScriptObject **field, ScriptObject *value)
{
    WriteScriptReference(*_collector, *_heap, field, value);
}

void ScriptDomain::RegisterWeakRef(ScriptObject *weakRef, ScriptObject *target)
{
    StateOf<ScriptWeakRefState>(weakRef).Target = target;
    _weakRefs.Add(weakRef);
}

void ScriptDomain::SetWeakTarget(ScriptObject *weakRef, ScriptObject *target)
{
    StateOf<ScriptWeakRefState>(weakRef).Target = target;
}

ScriptObject *ScriptDomain::GetWeakTarget(ScriptObject *weakRef)
{
    ScriptObject *target = StateOf<ScriptWeakRefState>(weakRef).Target;
    // A target read while marking becomes strongly referenced: it must not be cleared at the end of the cycle.
    _collector->Shade(target);
    return target;
}

void ScriptDomain::CopyElements(const ScriptArray &source, uint32 sourceIndex, ScriptArray &destination,
                                uint32 destinationIndex, uint32 count)
{
    const ScriptType &type = *destination.Type;
    const usize stride = type.ElementStride;
    const uint8 *from = reinterpret_cast<const uint8 *>(&source) + source.Type->DataOffset + sourceIndex * stride;
    uint8 *to = reinterpret_cast<uint8 *>(&destination) + type.DataOffset + destinationIndex * stride;
    if (type.ElementReferences.IsEmpty())
    {
        MemMove(to, from, count * stride);
        return;
    }
    if (_collector->IsMarking())
    {
        for (uint32 i = 0; i < count; ++i)
        {
            for (const uint32 offset : type.ElementReferences)
            {
                ScriptObject *previous;
                MemCopy(&previous, to + i * stride + offset, sizeof(previous));
                _collector->Shade(previous);
            }
        }
    }
    MemMove(to, from, count * stride);
    if (!_heap->IsOld(&destination))
    {
        return;
    }
    for (uint32 i = 0; i < count; ++i)
    {
        for (const uint32 offset : type.ElementReferences)
        {
            uint8 *field = to + i * stride + offset;
            ScriptObject *value;
            MemCopy(&value, field, sizeof(value));
            if (value != nullptr && _heap->IsYoung(value))
            {
                _heap->MarkCard(field);
            }
        }
    }
}

void ScriptDomain::ClearElements(ScriptArray &array, uint32 start, uint32 count)
{
    const ScriptType &type = *array.Type;
    uint8 *at = reinterpret_cast<uint8 *>(&array) + type.DataOffset + start * type.ElementStride;
    if (_collector->IsMarking())
    {
        for (uint32 i = 0; i < count; ++i)
        {
            for (const uint32 offset : type.ElementReferences)
            {
                ScriptObject *previous;
                MemCopy(&previous, at + i * type.ElementStride + offset, sizeof(previous));
                _collector->Shade(previous);
            }
        }
    }
    MemZero(at, usize{count} * type.ElementStride);
}

void ScriptInterpreter::VisitRoots(ScriptRootVisitor &visitor)
{
    auto visitParameters = [&](uint8 *window, const ScriptFunctionInfo &function, uint32 skip) {
        const uint32 start = skip < function.ParameterOffsets.Count() ? function.ParameterOffsets[skip] * ScriptSlotBytes
                             : skip == 0                               ? 0
                                                                       : ~0u;
        for (const uint32 offset : function.ParameterReferences)
        {
            if (offset >= start)
            {
                visitor.VisitObject(reinterpret_cast<ScriptObject **>(window + (offset - start)));
            }
        }
        for (const uint32 offset : function.ParameterManaged)
        {
            if (offset >= start)
            {
                visitor.VisitManaged(reinterpret_cast<uint8 **>(window + (offset - start)));
            }
        }
    };
    for (const UniquePtr<ScriptCoroutine> &pointer : _coroutines)
    {
        ScriptCoroutine *coroutine = pointer.Get();
        if (coroutine == nullptr)
        {
            continue;
        }
        // RECOVERY-DECISION SC6: a task keeps its coroutine's owner alive, an iterator does not: an abandoned
        // iterator dies and the collector frees its coroutine.
        const bool iterator = IsIteratorOwner(coroutine->Owner);
        if (coroutine->Owner != nullptr && !iterator)
        {
            visitor.VisitObject(&coroutine->Owner);
        }
        if (iterator && coroutine->Reserved != nullptr && coroutine->Entry != nullptr &&
            coroutine->Entry->ResultShape != ~0u && coroutine->Entry->Module != nullptr)
        {
            ScriptObject *owner = coroutine->Owner;
            if ((owner->Gc & ScriptGcBits::Forwarded) != 0)
            {
                owner = reinterpret_cast<ScriptObject *>(const_cast<ScriptType *>(owner->Type));
            }
            if (StateOf<ScriptIteratorState>(owner).Current != 0)
            {
                const ScriptLoadedModule &module = *coroutine->Entry->Module;
                if (coroutine->Entry->ResultShape < module.ShapeReferences.Count())
                {
                    for (const uint32 offset : module.ShapeReferences[coroutine->Entry->ResultShape])
                    {
                        visitor.VisitObject(reinterpret_cast<ScriptObject **>(coroutine->Reserved + offset));
                    }
                }
            }
        }
        for (ScriptFrame &frame : coroutine->Frames)
        {
            const ScriptFunctionInfo &function = *frame.Function;
            if (!frame.Entered)
            {
                visitParameters(frame.Base, function, 0);
                continue;
            }
            const ScriptFunctionRecord *record = function.Record;
            if (record == nullptr || record->MapBytes == 0)
            {
                continue;
            }
            usize low = 0;
            usize high = record->Safepoints.Count();
            while (low < high)
            {
                const usize middle = (low + high) / 2;
                if (record->Safepoints[middle].Pc < frame.Pc)
                {
                    low = middle + 1;
                }
                else
                {
                    high = middle;
                }
            }
            if (low == record->Safepoints.Count() || record->Safepoints[low].Pc != frame.Pc)
            {
                continue;
            }
            const uint8 *map = record->Maps.Data() + usize{record->Safepoints[low].Map} * record->MapBytes * 2;
            for (uint32 byte = 0; byte < record->MapBytes; ++byte)
            {
                for (uint32 bit = 0; bit < 8; ++bit)
                {
                    uint8 *slot = frame.Base + (usize{byte} * 8 + bit) * 8;
                    if ((map[byte] & (1u << bit)) != 0)
                    {
                        visitor.VisitObject(reinterpret_cast<ScriptObject **>(slot));
                    }
                    if ((map[record->MapBytes + byte] & (1u << bit)) != 0)
                    {
                        visitor.VisitManaged(reinterpret_cast<uint8 **>(slot));
                    }
                }
            }
        }
    }
    for (const ScriptRootWindow &root : _roots)
    {
        uint8 *arguments = root.Window;
        if (root.Delegate)
        {
            visitor.VisitObject(reinterpret_cast<ScriptObject **>(root.Window));
            arguments += ScriptSlotBytes;
        }
        if (root.Function != nullptr)
        {
            visitParameters(arguments, *root.Function, root.Skip);
        }
    }
    for (List<ScriptObject *> *tasks : {&_ready, &_frameWaiters, &_fixedWaiters, &_delays, &_due})
    {
        for (ScriptObject *&task : *tasks)
        {
            visitor.VisitObject(&task);
        }
    }
}
} // namespace Marlin
