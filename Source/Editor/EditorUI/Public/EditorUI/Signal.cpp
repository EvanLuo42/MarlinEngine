#include "EditorUI/Signal.h"

namespace Marlin
{
struct ReactiveOwner
{
    ReactiveOwner *Parent = nullptr;
    /// In creation order; a disposed child leaves null until the list is trimmed or compacted, so removal keeps the
    /// order without shifting.
    List<ReactiveOwner *> Children;
    /// Where this owner sits in its parent's Children.
    uint32 ParentIndex = 0;
    /// The null entries in Children.
    uint32 VacantChildren = 0;
    List<Private::ReactiveNode *> Nodes;
    List<Function<void()>> Cleanups;
    bool Disposing = false;
};

namespace
{
using Private::ReactiveKind;
using Private::ReactiveNode;
using Private::ReactiveState;

[[maybe_unused]] constexpr usize MaxEffectRuns = 1'000'000;

struct Slot
{
    ReactiveNode *Node = nullptr;
    uint32 Generation = 0;
};

struct Runtime
{
    List<Slot> Slots;
    List<uint32> FreeSlots;
    ReactiveOwner *Owner = nullptr;
    ReactiveNode *Observer = nullptr;
    uint64 NextRun = 1;
    List<ReactiveNode *> Queue;
    bool Flushing = false;
};

Runtime &GetRuntime()
{
    static Runtime runtime;
    if (runtime.Slots.IsEmpty())
    {
        runtime.Slots.Add({});
    }
    return runtime;
}

/// Removes the edge at *index* of *source*'s observers by swapping the last one in, whose other end is repointed.
void RemoveObserverAt(ReactiveNode *source, uint32 index)
{
    List<Private::ReactiveLink> &observers = source->Observers;
    observers.RemoveAtSwap(index);
    if (index < observers.Count())
    {
        const Private::ReactiveLink &moved = observers[index];
        moved.Node->Sources[moved.Index].Index = index;
    }
}

/// Removes the edge at *index* of *observer*'s sources, the same.
void RemoveSourceAt(ReactiveNode *observer, uint32 index)
{
    List<Private::ReactiveLink> &sources = observer->Sources;
    sources.RemoveAtSwap(index);
    if (index < sources.Count())
    {
        const Private::ReactiveLink &moved = sources[index];
        moved.Node->Observers[moved.Index].Index = index;
    }
}

void RemoveSources(ReactiveNode *node)
{
    for (const Private::ReactiveLink &link : node->Sources)
    {
        RemoveObserverAt(link.Node, link.Index);
    }
    node->Sources.Clear();
}

void Enqueue(ReactiveNode *node)
{
    Runtime &runtime = GetRuntime();
    if (node->QueueIndex == ~0u)
    {
        node->QueueIndex = static_cast<uint32>(runtime.Queue.Count());
        runtime.Queue.Add(node);
    }
}

void Mark(ReactiveNode *node, ReactiveState state)
{
    if (node->State >= state)
    {
        return;
    }
    const bool wasClean = node->State == ReactiveState::Clean;
    node->State = state;
    if (node->Kind == ReactiveKind::Effect)
    {
        Enqueue(node);
    }
    else if (wasClean)
    {
        for (const Private::ReactiveLink &observer : node->Observers)
        {
            Mark(observer.Node, ReactiveState::Check);
        }
    }
}

void FreeNode(ReactiveNode *node)
{
    Runtime &runtime = GetRuntime();
    if (node->Scope != nullptr)
    {
        Reactive::DisposeOwner(node->Scope);
        node->Scope = nullptr;
    }
    RemoveSources(node);
    for (const Private::ReactiveLink &link : node->Observers)
    {
        RemoveSourceAt(link.Node, link.Index);
    }
    node->Observers.Clear();
    if (node->QueueIndex != ~0u)
    {
        runtime.Queue[node->QueueIndex] = nullptr;
    }
    Slot &slot = runtime.Slots[node->Slot];
    slot.Node = nullptr;
    ++slot.Generation;
    runtime.FreeSlots.Add(node->Slot);
    delete node;
}

/// Removes *child* from *parent*'s children in constant time, keeping the order of the others.
void DetachChild(ReactiveOwner *parent, ReactiveOwner *child)
{
    List<ReactiveOwner *> &children = parent->Children;
    children[child->ParentIndex] = nullptr;
    ++parent->VacantChildren;
    while (!children.IsEmpty() && children.Last() == nullptr)
    {
        children.Pop();
        --parent->VacantChildren;
    }
    if (parent->VacantChildren > 16 && parent->VacantChildren * 2 > children.Count())
    {
        usize kept = 0;
        for (ReactiveOwner *sibling : children)
        {
            if (sibling != nullptr)
            {
                sibling->ParentIndex = static_cast<uint32>(kept);
                children[kept++] = sibling;
            }
        }
        children.Resize(kept);
        parent->VacantChildren = 0;
    }
}

/// Disposes the children of *owner*, runs its cleanups in reverse and frees its nodes, leaving it empty and usable.
void ClearOwner(ReactiveOwner *owner)
{
    owner->Disposing = true;
    while (!owner->Children.IsEmpty())
    {
        if (ReactiveOwner *child = owner->Children.Last())
        {
            Reactive::DisposeOwner(child);
        }
        else
        {
            owner->Children.Pop();
            --owner->VacantChildren;
        }
    }
    owner->VacantChildren = 0;
    Runtime &runtime = GetRuntime();
    ReactiveNode *observer = runtime.Observer;
    ReactiveOwner *current = runtime.Owner;
    runtime.Observer = nullptr;
    runtime.Owner = nullptr;
    for (usize index = owner->Cleanups.Count(); index-- > 0;)
    {
        owner->Cleanups[index]();
    }
    owner->Cleanups.Clear();
    runtime.Observer = observer;
    runtime.Owner = current;
    for (usize index = owner->Nodes.Count(); index-- > 0;)
    {
        FreeNode(owner->Nodes[index]);
    }
    owner->Nodes.Clear();
    owner->Disposing = false;
}
} // namespace

namespace Private
{
ReactiveHandle Register(ReactiveNode *node)
{
    Runtime &runtime = GetRuntime();
    MARLIN_ASSERT(runtime.Owner != nullptr);
    node->Owner = runtime.Owner;
    runtime.Owner->Nodes.Add(node);
    uint32 index = 0;
    if (!runtime.FreeSlots.IsEmpty())
    {
        index = runtime.FreeSlots.Pop();
    }
    else
    {
        index = static_cast<uint32>(runtime.Slots.Count());
        runtime.Slots.Add({nullptr, 1});
    }
    runtime.Slots[index].Node = node;
    node->Slot = index;
    return {index, runtime.Slots[index].Generation};
}

ReactiveNode *Resolve(ReactiveHandle handle)
{
    MARLIN_ASSERT(IsAlive(handle));
    return GetRuntime().Slots[handle.Slot].Node;
}

bool IsAlive(ReactiveHandle handle)
{
    const Runtime &runtime = GetRuntime();
    return handle.Slot != 0 && handle.Slot < runtime.Slots.Count() &&
           runtime.Slots[handle.Slot].Generation == handle.Generation && runtime.Slots[handle.Slot].Node != nullptr;
}

void Track(ReactiveNode *node)
{
    ReactiveNode *observer = GetRuntime().Observer;
    if (observer == nullptr || node->TrackedRun == observer->RunId)
    {
        return;
    }
    node->TrackedRun = observer->RunId;
    observer->Sources.Add({node, static_cast<uint32>(node->Observers.Count())});
    node->Observers.Add({observer, static_cast<uint32>(observer->Sources.Count() - 1)});
}

void NotifyChanged(ReactiveNode *node)
{
    for (const ReactiveLink &observer : node->Observers)
    {
        Mark(observer.Node, ReactiveState::Dirty);
    }
}

// RECOVERY-DECISION D7: the file is the last full carve (Phase B); the final UpdateIfNecessary was 3 lines longer.
void UpdateIfNecessary(ReactiveNode *node)
{
    if (node->State == ReactiveState::Check)
    {
        for (usize index = 0; index < node->Sources.Count(); ++index)
        {
            ReactiveNode *source = node->Sources[index].Node;
            if (source->Kind == ReactiveKind::Computed)
            {
                UpdateIfNecessary(source);
            }
            if (node->State == ReactiveState::Dirty)
            {
                break;
            }
        }
    }
    if (node->State == ReactiveState::Dirty)
    {
        node->State = ReactiveState::Clean;
        if (node->Run())
        {
            for (const ReactiveLink &link : node->Observers)
            {
                ReactiveNode *observer = link.Node;
                observer->State = ReactiveState::Dirty;
                if (observer->Kind == ReactiveKind::Effect)
                {
                    Enqueue(observer);
                }
            }
        }
    }
    node->State = ReactiveState::Clean;
}

void RunTracked(ReactiveNode *node, FunctionRef<void()> body)
{
    Runtime &runtime = GetRuntime();
    // The scope is kept between runs and emptied, so a run that creates nothing allocates nothing.
    if (node->Scope != nullptr)
    {
        ClearOwner(node->Scope);
    }
    else
    {
        node->Scope = Reactive::CreateOwner(nullptr);
    }
    RemoveSources(node);
    node->RunId = runtime.NextRun++;
    ReactiveNode *observer = runtime.Observer;
    ReactiveOwner *owner = runtime.Owner;
    runtime.Observer = node;
    runtime.Owner = node->Scope;
    body();
    runtime.Observer = observer;
    runtime.Owner = owner;
}

void InitializeEffect(ReactiveNode *node)
{
    (void)node->Run();
}
} // namespace Private

namespace Reactive
{
ReactiveOwner *GetOwner()
{
    return GetRuntime().Owner;
}

ReactiveOwner *CreateOwner(ReactiveOwner *parent)
{
    auto *owner = new ReactiveOwner();
    owner->Parent = parent;
    if (parent != nullptr)
    {
        owner->ParentIndex = static_cast<uint32>(parent->Children.Count());
        parent->Children.Add(owner);
    }
    return owner;
}

void DisposeOwner(ReactiveOwner *owner)
{
    if (owner == nullptr || owner->Disposing)
    {
        return;
    }
    ClearOwner(owner);
    if (owner->Parent != nullptr)
    {
        DetachChild(owner->Parent, owner);
    }
    delete owner;
}

void RunInOwner(ReactiveOwner *owner, FunctionRef<void()> body)
{
    Runtime &runtime = GetRuntime();
    ReactiveNode *observer = runtime.Observer;
    ReactiveOwner *current = runtime.Owner;
    runtime.Observer = nullptr;
    runtime.Owner = owner;
    body();
    runtime.Observer = observer;
    runtime.Owner = current;
}

void Untrack(FunctionRef<void()> body)
{
    Runtime &runtime = GetRuntime();
    ReactiveNode *observer = runtime.Observer;
    runtime.Observer = nullptr;
    body();
    runtime.Observer = observer;
}

void OnCleanup(Function<void()> cleanup)
{
    Runtime &runtime = GetRuntime();
    MARLIN_ASSERT(runtime.Owner != nullptr);
    runtime.Owner->Cleanups.Add(Move(cleanup));
}

void Flush()
{
    Runtime &runtime = GetRuntime();
    if (runtime.Flushing)
    {
        return;
    }
    runtime.Flushing = true;
    for (usize index = 0; index < runtime.Queue.Count(); ++index)
    {
        MARLIN_ASSERT(index < MaxEffectRuns);
        ReactiveNode *node = runtime.Queue[index];
        if (node == nullptr)
        {
            continue;
        }
        runtime.Queue[index] = nullptr;
        node->QueueIndex = ~0u;
        Private::UpdateIfNecessary(node);
    }
    runtime.Queue.Clear();
    runtime.Flushing = false;
}

bool HasPendingEffects()
{
    for (const ReactiveNode *node : GetRuntime().Queue)
    {
        if (node != nullptr)
        {
            return true;
        }
    }
    return false;
}
} // namespace Reactive
} // namespace Marlin
