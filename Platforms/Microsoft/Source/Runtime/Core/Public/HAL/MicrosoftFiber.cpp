#include "HAL/PlatformFiber.h"

#include "Diagnostics/Assert.h"
#include "Diagnostics/Sanitizer.h"
#include "HAL/MicrosoftHeaders.h"

namespace Marlin::PlatformFiber
{
namespace
{
struct FiberState
{
    void *OsFiber;
    FiberEntry Entry;
    void *Argument;
    const void *StackBottom;
    usize StackSize;
};

FiberState *NewFiberState(const FiberEntry entry, void *argument)
{
    auto *state = static_cast<FiberState *>(HeapAlloc(GetProcessHeap(), 0, sizeof(FiberState)));
    if (state != nullptr)
    {
        *state = {.OsFiber = nullptr, .Entry = entry, .Argument = argument, .StackBottom = nullptr, .StackSize = 0};
    }
    return state;
}

void ReadStackBounds(FiberState &state)
{
    ULONG_PTR low = 0;
    ULONG_PTR high = 0;
    GetCurrentThreadStackLimits(&low, &high);
    state.StackBottom = reinterpret_cast<const void *>(low);
    state.StackSize = high - low;
}

[[noreturn]] VOID WINAPI FiberTrampoline(const LPVOID parameter)
{
    FiberState &state = *static_cast<FiberState *>(parameter);
    Sanitizer::FinishSwitchFiber(nullptr);
    if constexpr (MARLIN_ASAN)
    {
        ReadStackBounds(state);
        void *fakeStack = nullptr;
        Sanitizer::StartSwitchFiber(&fakeStack, state.StackBottom, state.StackSize);
        Sanitizer::FinishSwitchFiber(fakeStack);
    }
    state.Entry(state.Argument);
    MARLIN_FATAL("A fiber returned from its entry point");
}
} // namespace

FiberHandle ConvertCurrentThread()
{
    FiberState *state = NewFiberState(nullptr, nullptr);
    if (state == nullptr)
    {
        return {};
    }
    state->OsFiber = ConvertThreadToFiberEx(state, FIBER_FLAG_FLOAT_SWITCH);
    if (state->OsFiber == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, state);
        return {};
    }
    ReadStackBounds(*state);
    return {state};
}

void RevertCurrentThread()
{
    void *state = GetFiberData();
    MARLIN_VERIFY(ConvertFiberToThread());
    HeapFree(GetProcessHeap(), 0, state);
}

FiberHandle Create(usize stackSize, FiberEntry entry, void *argument)
{
    MARLIN_ASSERT(entry != nullptr);
    FiberState *state = NewFiberState(entry, argument);
    if (state == nullptr)
    {
        return {};
    }
    state->OsFiber = CreateFiberEx(0, stackSize, FIBER_FLAG_FLOAT_SWITCH, FiberTrampoline, state);
    if (state->OsFiber == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, state);
        return {};
    }
    return {state};
}

void Delete(FiberHandle fiber)
{
    MARLIN_ASSERT(fiber.IsValid());
    auto *state = static_cast<FiberState *>(fiber.Value);
    Sanitizer::UnpoisonMemory(state->StackBottom, state->StackSize);
    DeleteFiber(state->OsFiber);
    HeapFree(GetProcessHeap(), 0, state);
}

void SwitchTo(FiberHandle fiber)
{
    MARLIN_ASSERT(fiber.IsValid());
    const auto *state = static_cast<FiberState *>(fiber.Value);
    void *fakeStack = nullptr;
    Sanitizer::StartSwitchFiber(&fakeStack, state->StackBottom, state->StackSize);
    SwitchToFiber(state->OsFiber);
    Sanitizer::FinishSwitchFiber(fakeStack);
}

FiberHandle Current()
{
    return IsThreadAFiber() ? FiberHandle{GetFiberData()} : FiberHandle{};
}
} // namespace Marlin::PlatformFiber
