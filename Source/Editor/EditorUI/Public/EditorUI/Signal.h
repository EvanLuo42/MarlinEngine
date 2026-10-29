#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Templates/Function.h"
#include "Templates/Optional.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// Fine-grained reactivity for the editor UI: signals, computeds and effects in an owner tree, with effects
/// batched to once a frame (Documentation/Architecture/09 §4.1).
struct ReactiveOwner;

namespace Private
{
enum class ReactiveKind : uint8
{
    Signal,
    Computed,
    Effect,
};

enum class ReactiveState : uint8
{
    Clean,
    /// A source further up may have changed; pull to find out.
    Check,
    Dirty,
};

struct ReactiveNode;

/// One end of a source-observer edge: the node at the other end and where the edge sits in that node's opposite list,
/// so either end removes the edge in constant time.
struct ReactiveLink
{
    ReactiveNode *Node = nullptr;
    uint32 Index = 0;
};

struct EDITORUI_API ReactiveNode
{
    explicit ReactiveNode(ReactiveKind kind) : Kind(kind)
    {
    }
    virtual ~ReactiveNode() = default;
    MARLIN_NONCOPYABLE(ReactiveNode);

    /// Computeds and effects: recomputes or runs. A computed returns whether its value changed.
    virtual bool Run()
    {
        return false;
    }

    ReactiveKind Kind;
    ReactiveState State = ReactiveState::Clean;
    uint32 Slot = 0;
    /// In the effect queue at this index, or ~0u.
    uint32 QueueIndex = ~0u;
    /// The run of the observer that last tracked this node, so a run tracks a source once.
    uint64 TrackedRun = 0;
    /// Computeds and effects: which run this is.
    uint64 RunId = 0;
    List<ReactiveLink> Sources;
    List<ReactiveLink> Observers;
    ReactiveOwner *Owner = nullptr;
    /// Computeds and effects: what their last run created.
    ReactiveOwner *Scope = nullptr;
};

/// A node by slot and generation, so that a handle to a freed node is caught.
struct ReactiveHandle
{
    uint32 Slot = 0;
    uint32 Generation = 0;
};

EDITORUI_API ReactiveHandle Register(ReactiveNode *node);
EDITORUI_API ReactiveNode *Resolve(ReactiveHandle handle);
EDITORUI_API bool IsAlive(ReactiveHandle handle);
EDITORUI_API void Track(ReactiveNode *node);
EDITORUI_API void NotifyChanged(ReactiveNode *node);
EDITORUI_API void UpdateIfNecessary(ReactiveNode *node);
/// Runs a computed or effect: disposes what its last run created, retracks its sources and calls *body*.
EDITORUI_API void RunTracked(ReactiveNode *node, FunctionRef<void()> body);
EDITORUI_API void InitializeEffect(ReactiveNode *node);

template <typename T>
concept ReactiveComparable = requires(const T &a, const T &b) {
    { a == b } -> ConvertibleTo<bool>;
};

template <typename T> bool ReactiveEqual(const T &a, const T &b)
{
    if constexpr (ReactiveComparable<T>)
    {
        return a == b;
    }
    else
    {
        return false;
    }
}

template <typename T> struct SignalNode final : ReactiveNode
{
    explicit SignalNode(T value) : ReactiveNode(ReactiveKind::Signal), Value(Move(value))
    {
    }

    T Value;
};

template <typename T> struct ComputedNode final : ReactiveNode
{
    explicit ComputedNode(Function<T()> compute) : ReactiveNode(ReactiveKind::Computed), Compute(Move(compute))
    {
        State = ReactiveState::Dirty;
    }

    bool Run() override
    {
        bool changed = false;
        RunTracked(this, [&] {
            T next = Compute();
            changed = !Value.HasValue() || !ReactiveEqual(*Value, next);
            if (changed)
            {
                Value.Emplace(Move(next));
            }
        });
        return changed;
    }

    Function<T()> Compute;
    Optional<T> Value;
};

struct EffectNode final : ReactiveNode
{
    explicit EffectNode(Function<void()> body) : ReactiveNode(ReactiveKind::Effect), Body(Move(body))
    {
    }

    bool Run() override
    {
        RunTracked(this, [&] { Body(); });
        return false;
    }

    Function<void()> Body;
};
} // namespace Private

/// A value that the computeds, effects and element properties reading it follow (09 §4.1). A handle: copies name the
/// same value, which lives until the owner it was created in is disposed.
// RECOVERY-DECISION D6: Signal, Computed and the Reactive declarations were not recovered; written from the outline.
template <typename T> class Signal
{
public:
    Signal() = default;

    /// The value; the running computed or effect follows it.
    [[nodiscard]] const T &Get() const
    {
        Private::SignalNode<T> *node = Node();
        Private::Track(node);
        return node->Value;
    }

    /// The value, without following it.
    [[nodiscard]] const T &Peek() const
    {
        return Node()->Value;
    }

    /// Stores *value*; what follows the signal updates in the next Flush unless the value is equal.
    void Set(T value) const
    {
        Private::SignalNode<T> *node = Node();
        if (Private::ReactiveEqual(node->Value, value))
        {
            return;
        }
        node->Value = Move(value);
        Private::NotifyChanged(node);
    }

    /// Changes the value in place with *update(T &)*; always notifies.
    template <typename TUpdate> void Update(TUpdate &&update) const
    {
        Private::SignalNode<T> *node = Node();
        update(node->Value);
        Private::NotifyChanged(node);
    }

    [[nodiscard]] bool IsValid() const
    {
        return Private::IsAlive(_handle);
    }

private:
    template <typename TValue> friend Signal<TValue> CreateSignal(TValue value);

    explicit Signal(Private::ReactiveHandle handle) : _handle(handle)
    {
    }

    Private::SignalNode<T> *Node() const
    {
        return static_cast<Private::SignalNode<T> *>(Private::Resolve(_handle));
    }

    Private::ReactiveHandle _handle;
};

/// A value derived from signals and other computeds. Lazy and glitch-free: it recomputes when read after a source
/// changed, and only notifies its followers when the result differs.
template <typename T> class Computed
{
public:
    Computed() = default;

    [[nodiscard]] const T &Get() const
    {
        Private::ComputedNode<T> *node = Node();
        Private::UpdateIfNecessary(node);
        Private::Track(node);
        return *node->Value;
    }

    [[nodiscard]] bool IsValid() const
    {
        return Private::IsAlive(_handle);
    }

private:
    template <typename TValue> friend Computed<TValue> CreateComputed(Function<TValue()> compute);

    explicit Computed(Private::ReactiveHandle handle) : _handle(handle)
    {
    }

    Private::ComputedNode<T> *Node() const
    {
        return static_cast<Private::ComputedNode<T> *>(Private::Resolve(_handle));
    }

    Private::ReactiveHandle _handle;
};

/// The owner tree: everything reactive is created under the current owner and freed with it.
namespace Reactive
{
/// The owner that what is created now belongs to, or null.
[[nodiscard]] EDITORUI_API ReactiveOwner *GetOwner();
/// A new owner, freed with *parent* unless *parent* is null.
[[nodiscard]] EDITORUI_API ReactiveOwner *CreateOwner(ReactiveOwner *parent);
/// Disposes the children of *owner*, runs its cleanups in reverse and frees what was created under it.
EDITORUI_API void DisposeOwner(ReactiveOwner *owner);
/// Runs *body* with *owner* current and nothing tracking.
EDITORUI_API void RunInOwner(ReactiveOwner *owner, FunctionRef<void()> body);
/// Runs *body* without the running computed or effect following what it reads.
EDITORUI_API void Untrack(FunctionRef<void()> body);
/// Runs *cleanup* when the current owner is disposed or its computed or effect runs again.
EDITORUI_API void OnCleanup(Function<void()> cleanup);

/// Runs the effects whose sources changed, until none are left; the editor calls it once a frame in its signal
/// propagation phase (09 §4.1), and tests call it directly.
EDITORUI_API void Flush();
[[nodiscard]] EDITORUI_API bool HasPendingEffects();
} // namespace Reactive

/// A signal owned by the current owner, which must exist.
template <typename T> [[nodiscard]] Signal<T> CreateSignal(T value)
{
    return Signal<T>(Private::Register(new Private::SignalNode<T>(Move(value))));
}

/// Component state: a signal owned by the component being built.
template <typename T> [[nodiscard]] Signal<T> UseState(T value)
{
    return CreateSignal(Move(value));
}

template <typename T> [[nodiscard]] Computed<T> CreateComputed(Function<T()> compute)
{
    return Computed<T>(Private::Register(new Private::ComputedNode<T>(Move(compute))));
}

/// Runs *body* now, tracking what it reads, and again in a later Flush whenever any of that changes, until the
/// current owner is disposed. What a run creates is disposed before the next run.
inline void CreateEffect(Function<void()> body)
{
    auto *node = new Private::EffectNode(Move(body));
    (void)Private::Register(node);
    Private::InitializeEffect(node);
}
} // namespace Marlin
