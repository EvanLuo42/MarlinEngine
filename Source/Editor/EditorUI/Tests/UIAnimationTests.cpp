#include "EditorUI/UIAnimation.h"
#include "EditorUI/UIElement.h"
#include "EditorUI/UIWindowRoot.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
constexpr UITransition OneSecondLinear{1.0f, &Easing::Linear};

bool AnimationNear(float32 a, float32 b)
{
    return a - b < 0.001f && b - a < 0.001f;
}

/// Runs *body* in a root owner, on a clock starting at zero, and disposes it afterwards.
template <typename TBody> void WithClock(TBody &&body)
{
    UIAnimation::SetTime(0);
    ReactiveOwner *root = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(root, body);
    Reactive::DisposeOwner(root);
    Reactive::Flush();
}
} // namespace

MARLIN_TEST(EditorUIAnimation, TweensEaseToTheirTargetOverTheirDuration)
{
    WithClock([] {
        const Tween<float32> tween(0, OneSecondLinear);
        tween.Set(10);
        MARLIN_EXPECT(AnimationNear(tween.Peek(), 0) && tween.IsMoving());
        UIAnimation::SetTime(0.25);
        MARLIN_EXPECT(AnimationNear(tween.Peek(), 2.5f));
        // Retargeted mid-way, it starts from where it is.
        tween.Set(0);
        UIAnimation::SetTime(0.75);
        MARLIN_EXPECT(AnimationNear(tween.Peek(), 1.25f));
        UIAnimation::SetTime(2);
        MARLIN_EXPECT(AnimationNear(tween.Peek(), 0) && !tween.IsMoving());
        tween.Snap(4);
        MARLIN_EXPECT(AnimationNear(tween.Peek(), 4) && !tween.IsMoving());
    });
}

MARLIN_TEST(EditorUIAnimation, ReadersFollowTheClockOnlyWhileMoving)
{
    WithClock([] {
        const Tween<float32> tween(0, OneSecondLinear);
        int32 runs = 0;
        float32 seen = -1;
        CreateEffect([&runs, &seen, tween] {
            ++runs;
            seen = tween.Get();
        });
        UIAnimation::SetTime(0.5);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 1);

        tween.Set(1);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2 && AnimationNear(seen, 0));
        UIAnimation::SetTime(1);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 3 && AnimationNear(seen, 0.5f));
        UIAnimation::SetTime(1.5);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 4 && AnimationNear(seen, 1));
        // Finished: frames go by without running it.
        UIAnimation::SetTime(3);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 4);
    });
}

MARLIN_TEST(EditorUIAnimation, AnimateEasesIntoChangesOfItsSource)
{
    WithClock([] {
        const Signal<bool> on = CreateSignal(false);
        const auto value = Animate<float32>([on] { return on.Get() ? 8.0f : 0.0f; }, OneSecondLinear);
        MARLIN_EXPECT(AnimationNear(value(), 0));
        on.Set(true);
        Reactive::Flush();
        UIAnimation::SetTime(0.5);
        MARLIN_EXPECT(AnimationNear(value(), 4));
        UIAnimation::SetTime(1.5);
        MARLIN_EXPECT(AnimationNear(value(), 8));
    });
}

MARLIN_TEST(EditorUIAnimation, ScaleShrinksHitsAboutTheTopLeft)
{
    UIWindowRoot window(nullptr);
    UIElementNode *scaled = nullptr;
    window.SetContent([&] {
        UIElement box = Box().Width(100).Height(100).Scale(0.5f);
        scaled = box.GetNode();
        return Row({box});
    });
    window.Layout({200, 200});
    MARLIN_EXPECT(window.HitTest({25, 25}) == scaled);
    MARLIN_EXPECT(window.HitTest({75, 75}) != scaled);
}
} // namespace Marlin::Tests
