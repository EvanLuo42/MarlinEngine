#pragma once

#include "CoreTypes.h"
#include "EditorUI/Signal.h"
#include "Math/Color.h"
#include "Math/Easing.h"
#include "Math/Vector.h"
#include "Templates/Function.h"

namespace Marlin
{
/// How a value moves to a new target.
struct UITransition
{
    /// Seconds; zero jumps.
    float32 Duration = 0.12f;
    /// Shared with game UI (Core Math/Easing.h).
    EasingFunction Easing = &Easing::EaseOutCubic;

    constexpr bool operator==(const UITransition &) const = default;
};

/// The UI clock (Documentation/Architecture/09 §4.1): EditorUISubsystem sets it once a frame, before Reactive::Flush.
/// Reading it with Now makes the running computed, effect or bound property follow it, so it updates every frame;
/// Tween does that only while it moves.
namespace UIAnimation
{
/// Seconds; the running computed or effect follows the clock.
[[nodiscard]] EDITORUI_API float64 Now();
/// Seconds, without following the clock.
[[nodiscard]] EDITORUI_API float64 PeekNow();
/// Moves the clock to *seconds*; EditorUISubsystem calls it each frame, tests call it directly.
EDITORUI_API void SetTime(float64 seconds);
} // namespace UIAnimation

[[nodiscard]] inline float32 UILerp(float32 a, float32 b, float32 t)
{
    return a + (b - a) * t;
}

[[nodiscard]] inline Vector2 UILerp(Vector2 a, Vector2 b, float32 t)
{
    return a + (b - a) * t;
}

[[nodiscard]] inline LinearColor UILerp(LinearColor a, LinearColor b, float32 t)
{
    return LinearColor::Lerp(a, b, t);
}

/// A value that eases to each new target over its transition. A handle, like Signal: copies name the same value,
/// which lives as long as the owner it was created in. Get follows the value, and the UI clock only while it moves.
template <typename T> class Tween
{
public:
    Tween() = default;

    Tween(T initial, UITransition transition) : _state(CreateSignal(State{initial, initial, 0, transition}))
    {
    }

    /// The value now.
    [[nodiscard]] T Get() const
    {
        const State &state = _state.Get();
        const float32 progress = Progress(state, UIAnimation::PeekNow());
        if (progress >= 1)
        {
            return state.To;
        }
        (void)UIAnimation::Now();
        return UILerp(state.From, state.To, state.Transition.Easing(progress));
    }

    /// The value now, without following it.
    [[nodiscard]] T Peek() const
    {
        const State &state = _state.Peek();
        const float32 progress = Progress(state, UIAnimation::PeekNow());
        return progress >= 1 ? state.To : UILerp(state.From, state.To, state.Transition.Easing(progress));
    }

    [[nodiscard]] T GetTarget() const
    {
        return _state.Peek().To;
    }

    /// Moves from where it is now to *target*.
    void Set(T target) const
    {
        const State &state = _state.Peek();
        if (state.To == target)
        {
            return;
        }
        _state.Set(State{Peek(), target, UIAnimation::PeekNow(), state.Transition});
    }

    /// Jumps to *value*.
    void Snap(T value) const
    {
        _state.Set(State{value, value, UIAnimation::PeekNow(), _state.Peek().Transition});
    }

    /// Starts again from *from* towards the current target.
    void Restart(T from) const
    {
        const State &state = _state.Peek();
        _state.Set(State{from, state.To, UIAnimation::PeekNow(), state.Transition});
    }

    [[nodiscard]] bool IsMoving() const
    {
        return Progress(_state.Peek(), UIAnimation::PeekNow()) < 1;
    }

    [[nodiscard]] bool IsValid() const
    {
        return _state.IsValid();
    }

private:
    struct State
    {
        T From;
        T To;
        float64 Start = 0;
        UITransition Transition;

        bool operator==(const State &) const = default;
    };

    static float32 Progress(const State &state, float64 now)
    {
        const float32 duration = state.Transition.Duration;
        if (duration <= 0 || state.From == state.To)
        {
            return 1;
        }
        const float64 progress = (now - state.Start) / duration;
        return progress >= 1 ? 1.0f : progress <= 0 ? 0.0f : static_cast<float32>(progress);
    }

    Signal<State> _state;
};

/// *source* (a function of signals), easing into each of its changes: `.Background(Animate<LinearColor>([=] {
/// return hovered.Get() ? a : b; }))`. Returns a property value (a copyable function) that follows the eased value.
template <typename T, typename TSource> [[nodiscard]] auto Animate(TSource source, UITransition transition = {})
{
    T initial{};
    Reactive::Untrack([&] { initial = static_cast<T>(source()); });
    const Tween<T> tween(initial, transition);
    CreateEffect([source = Move(source), tween] { tween.Set(static_cast<T>(source())); });
    return [tween] { return tween.Get(); };
}
} // namespace Marlin
