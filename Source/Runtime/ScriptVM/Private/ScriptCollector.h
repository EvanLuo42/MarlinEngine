#pragma once

#include "ScriptHeap.h"

namespace Marlin
{
class ScriptInterpreter;
class ScriptRootVisitor;

/// The collector of a domain's heap (MarlinScript §9.2): copying collections of the young generation, and
/// incremental snapshot-at-the-beginning marking with lazy sweeping of the old one.
class ScriptCollector
{
public:
    enum class Phase : uint8
    {
        Idle,
        Marking,
        Sweeping,
    };

    ScriptCollector(ScriptDomain &domain, ScriptHeap &heap);
    MARLIN_NONCOPYABLE(ScriptCollector);

    /// The mark bit an object allocated in the old generation starts with: marked for the cycle in progress.
    [[nodiscard]] uint64 OldMark() const
    {
        return _markColor;
    }

    [[nodiscard]] bool IsMarking() const
    {
        return _phase == Phase::Marking;
    }

    /// Shades the object a reference field held before a store, while marking (the snapshot barrier).
    void Shade(ScriptObject *previous);

    void CollectYoung();
    /// Runs a whole old-generation cycle now, finishing the one in progress if there is one.
    void CollectFull();
    /// Does incremental old-generation work for about *seconds*, starting a cycle when the old generation has grown.
    void Step(float64 seconds);

private:
    class Forwarder;
    class Marker;

    void CollectYoung(bool tenure);
    void VisitRoots(ScriptRootVisitor &visitor);
    void BeginMarking();
    /// Marks until the gray stack is empty or *deadline* passes; true when marking is done.
    bool MarkSome(float64 deadline);
    /// Sweeps blocks until all are swept or *deadline* passes; true when sweeping is done.
    bool SweepSome(float64 deadline);
    void Push(ScriptObject *object);
    /// After a young collection: follows moved weak references and targets, and clears dead young targets.
    void UpdateWeakRefsAfterYoung();
    /// After marking: forgets dead weak references and clears unmarked old targets.
    void ClearUnmarkedWeakRefs();
    [[nodiscard]] bool IsMarked(const ScriptObject *object) const
    {
        return (object->Gc & ScriptGcBits::Permanent) != 0 || (object->Gc & ScriptGcBits::Marked) == _markColor;
    }

    ScriptDomain *_domain;
    ScriptHeap *_heap;
    Phase _phase = Phase::Idle;
    uint64 _markColor = 0;
    List<ScriptObject *> _gray;
    usize _sweepCursor = 0;
    usize _liveBytes = 0;
    usize _trigger = usize{8} << 20;
};

/// Stores *value* in the reference field at *field* with the write barriers: while marking the value it replaces is
/// shaded, and an old field that now references a young object has its card marked. Inline so the interpreter's
/// stores pay only the two checks when neither applies.
inline void WriteScriptReference(ScriptCollector &collector, ScriptHeap &heap, ScriptObject **field,
                                 ScriptObject *value)
{
    if (collector.IsMarking())
    {
        collector.Shade(*field);
    }
    *field = value;
    if (value != nullptr && heap.IsOld(field) && heap.IsYoung(value))
    {
        heap.MarkCard(field);
    }
}

/// The bytes an object takes on the heap, rounded up to 16.
[[nodiscard]] usize ScriptObjectBytes(const ScriptObject *object);

/// Calls *visit* with the address of every reference field of *object*.
template <typename TVisit> void ForEachScriptReference(ScriptObject *object, TVisit &&visit)
{
    const ScriptType &type = *object->Type;
    uint8 *bytes = reinterpret_cast<uint8 *>(object);
    for (const uint32 offset : type.References)
    {
        visit(reinterpret_cast<ScriptObject **>(bytes + offset));
    }
    if ((type.Kind == ScriptTypeKind::Array || type.Kind == ScriptTypeKind::String) && !type.ElementReferences.IsEmpty())
    {
        const uint32 length = static_cast<ScriptArray *>(object)->Length;
        uint8 *element = bytes + type.DataOffset;
        for (uint32 i = 0; i < length; ++i, element += type.ElementStride)
        {
            for (const uint32 offset : type.ElementReferences)
            {
                visit(reinterpret_cast<ScriptObject **>(element + offset));
            }
        }
    }
}
} // namespace Marlin
