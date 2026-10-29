#include "EditorUI/Signal.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
/// Runs *body* in a root owner and disposes it afterwards.
template <typename TBody> void WithRoot(TBody &&body)
{
    ReactiveOwner *root = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(root, body);
    Reactive::DisposeOwner(root);
    Reactive::Flush();
}
} // namespace

MARLIN_TEST(EditorUISignal, EffectsRunOncePerFlush)
{
    WithRoot([] {
        Signal<int32> count = CreateSignal(1);
        int32 runs = 0;
        int32 seen = 0;
        CreateEffect([=, &runs, &seen] {
            ++runs;
            seen = count.Get();
        });
        MARLIN_EXPECT(runs == 1 && seen == 1);
        count.Set(2);
        count.Set(3);
        MARLIN_EXPECT(runs == 1 && Reactive::HasPendingEffects());
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2 && seen == 3 && !Reactive::HasPendingEffects());
        count.Set(3);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2);
    });
}

MARLIN_TEST(EditorUISignal, ComputedIsLazyAndGlitchFree)
{
    WithRoot([] {
        Signal<int32> first = CreateSignal(1);
        Signal<int32> second = CreateSignal(2);
        int32 computes = 0;
        Computed<int32> sum = CreateComputed<int32>([=, &computes] {
            ++computes;
            return first.Get() + second.Get();
        });
        Computed<bool> large = CreateComputed<bool>([=] { return sum.Get() > 10; });
        MARLIN_EXPECT(computes == 0);
        int32 effectRuns = 0;
        List<int32> sums;
        CreateEffect([=, &effectRuns, &sums] {
            ++effectRuns;
            sums.Add(sum.Get() * (large.Get() ? -1 : 1));
        });
        MARLIN_EXPECT(computes == 1 && sums.Count() == 1 && sums[0] == 3);
        first.Set(5);
        second.Set(6);
        Reactive::Flush();
        MARLIN_EXPECT(computes == 2 && effectRuns == 2 && sums.Last() == -11);
        MARLIN_EXPECT(sum.Get() == 11 && computes == 2);
    });
}

MARLIN_TEST(EditorUISignal, UnchangedComputedStopsPropagation)
{
    WithRoot([] {
        Signal<int32> value = CreateSignal(4);
        Computed<bool> even = CreateComputed<bool>([=] { return value.Get() % 2 == 0; });
        int32 runs = 0;
        CreateEffect([=, &runs] {
            (void)even.Get();
            ++runs;
        });
        value.Set(6);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 1);
        value.Set(7);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2);
    });
}

MARLIN_TEST(EditorUISignal, EffectsRetrackEachRun)
{
    WithRoot([] {
        Signal<bool> useFirst = CreateSignal(true);
        Signal<int32> first = CreateSignal(1);
        Signal<int32> second = CreateSignal(2);
        int32 runs = 0;
        CreateEffect([=, &runs] {
            ++runs;
            (void)(useFirst.Get() ? first.Get() : second.Get());
        });
        second.Set(20);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 1);
        useFirst.Set(false);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2);
        first.Set(10);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2);
        second.Set(30);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 3);
    });
}

MARLIN_TEST(EditorUISignal, OwnersDisposeEverythingUnderThem)
{
    Signal<int32> outer;
    Signal<int32> inner;
    int32 cleanups = 0;
    int32 runs = 0;
    ReactiveOwner *root = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(root, [&] {
        outer = CreateSignal(0);
        ReactiveOwner *child = Reactive::CreateOwner(Reactive::GetOwner());
        Reactive::RunInOwner(child, [&] {
            inner = CreateSignal(1);
            Reactive::OnCleanup([&] { ++cleanups; });
            CreateEffect([&] {
                ++runs;
                (void)outer.Get();
            });
        });
    });
    MARLIN_EXPECT(outer.IsValid() && inner.IsValid() && runs == 1);
    Reactive::DisposeOwner(root);
    MARLIN_EXPECT(!outer.IsValid() && !inner.IsValid() && cleanups == 1);
}

MARLIN_TEST(EditorUISignal, EffectRunsDisposeWhatTheLastRunCreated)
{
    int32 cleanups = 0;
    int32 innerRuns = 0;
    Signal<int32> created;
    ReactiveOwner *root = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(root, [&] {
        Signal<int32> generation = CreateSignal(0);
        CreateEffect([=, &cleanups, &innerRuns, &created] {
            (void)generation.Get();
            Reactive::OnCleanup([&cleanups] { ++cleanups; });
            created = CreateSignal(generation.Peek());
            CreateEffect([&innerRuns] { ++innerRuns; });
        });
        const Signal<int32> first = created;
        generation.Set(1);
        Reactive::Flush();
        MARLIN_EXPECT(cleanups == 1 && innerRuns == 2 && !first.IsValid() && created.IsValid() && created.Peek() == 1);
    });
    Reactive::DisposeOwner(root);
    MARLIN_EXPECT(cleanups == 2 && !created.IsValid());
}

MARLIN_TEST(EditorUISignal, EffectsMaySetSignals)
{
    WithRoot([] {
        Signal<int32> source = CreateSignal(1);
        Signal<int32> doubled = CreateSignal(0);
        CreateEffect([=] { doubled.Set(source.Get() * 2); });
        int32 seen = 0;
        CreateEffect([=, &seen] { seen = doubled.Get(); });
        MARLIN_EXPECT(seen == 2);
        source.Set(5);
        Reactive::Flush();
        MARLIN_EXPECT(seen == 10);
    });
}

MARLIN_TEST(EditorUISignal, UntrackAndUpdate)
{
    WithRoot([] {
        Signal<int32> tracked = CreateSignal(0);
        Signal<List<int32>> items = CreateSignal(List<int32>());
        int32 runs = 0;
        usize count = 0;
        CreateEffect([=, &runs, &count] {
            ++runs;
            count = items.Get().Count();
            Reactive::Untrack([=] { (void)tracked.Get(); });
        });
        tracked.Set(1);
        Reactive::Flush();
        MARLIN_EXPECT(runs == 1);
        items.Update([](List<int32> &list) { list.Add(7); });
        Reactive::Flush();
        MARLIN_EXPECT(runs == 2 && count == 1);
    });
}

MARLIN_TEST(EditorUISignal, ManyObserversOfOneSignalStayLinked)
{
    // Effects that each read a shared signal and one of their own, rerun and disposed in an order unlike the one
    // they subscribed in, so edges are removed from the middle of the shared signal's observers.
    WithRoot([] {
        Signal<int32> shared = CreateSignal(0);
        constexpr uint32 Count = 64;
        List<Signal<int32>> own;
        List<ReactiveOwner *> owners;
        List<int32> seen;
        seen.Resize(Count);
        for (uint32 index = 0; index < Count; ++index)
        {
            own.Add(CreateSignal(static_cast<int32>(index)));
            owners.Add(Reactive::CreateOwner(Reactive::GetOwner()));
            const Signal<int32> mine = own.Last();
            int32 *target = &seen[index];
            Reactive::RunInOwner(owners.Last(),
                                 [=] { CreateEffect([=] { *target = shared.Get() * 1000 + mine.Get(); }); });
        }
        for (uint32 index = 0; index < Count; index += 3)
        {
            own[index].Set(static_cast<int32>(index) + 1);
        }
        Reactive::Flush();
        for (uint32 index = 1; index < Count; index += 2)
        {
            Reactive::DisposeOwner(owners[index]);
        }
        shared.Set(2);
        Reactive::Flush();
        bool linked = true;
        for (uint32 index = 0; index < Count; ++index)
        {
            const int32 mine = static_cast<int32>(index) + (index % 3 == 0 ? 1 : 0);
            // Disposed effects keep what they last saw: the shared value was still 0.
            linked = linked && seen[index] == (index % 2 == 0 ? 2000 + mine : mine);
        }
        MARLIN_EXPECT(linked);
    });
}

MARLIN_TEST(EditorUISignal, EffectScopeIsReusedAcrossRuns)
{
    WithRoot([] {
        Signal<int32> source = CreateSignal(0);
        int32 cleanups = 0;
        int32 innerRuns = 0;
        CreateEffect([=, &cleanups, &innerRuns] {
            const int32 value = source.Get();
            Reactive::OnCleanup([&cleanups] { ++cleanups; });
            if (value % 2 == 0)
            {
                CreateEffect([=, &innerRuns] {
                    (void)source.Peek();
                    ++innerRuns;
                });
            }
        });
        for (int32 value = 1; value <= 4; ++value)
        {
            source.Set(value);
            Reactive::Flush();
        }
        // Five runs (0 to 4); every run but the last was cleaned up, and the even ones made an inner effect.
        MARLIN_EXPECT(cleanups == 4 && innerRuns == 3);
    });
}
} // namespace Marlin::Tests
