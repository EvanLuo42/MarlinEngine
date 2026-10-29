#include "Engine/StartupProgress.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "String/StringView.h"

namespace Marlin
{
namespace
{
struct ReadyCheck
{
    StartupReadyCheckId Id = 0;
    String Name;
    Function<bool()> Check;
};

struct StartupState
{
    Atomic<bool> Active{false};
    SpinLock Lock;
    uint32 StepsDone = 0;
    uint32 StepsTotal = 0;
    char Status[sizeof(StartupSnapshot::Status)] = {};
    bool MainLoopStarted = false;
    List<ReadyCheck> Checks;
    StartupReadyCheckId NextCheck = 1;
};

/// Built on first use and never destroyed, so reports during shutdown stay safe.
StartupState &GetState()
{
    alignas(StartupState) static uint8 storage[sizeof(StartupState)];
    static StartupState *state = ConstructAt(reinterpret_cast<StartupState *>(storage));
    return *state;
}

/// Copies *text* into the status, cut at a whole UTF-8 character. Called under the lock.
void WriteStatus(StartupState &state, StringView text)
{
    usize length = text.Length() < sizeof(state.Status) - 1 ? text.Length() : sizeof(state.Status) - 1;
    while (length < text.Length() && length > 0 && (static_cast<uint8>(text[length]) & 0xC0) == 0x80)
    {
        --length;
    }
    MemCopy(state.Status, text.Data(), length);
    state.Status[length] = '\0';
}
} // namespace

namespace StartupProgress
{
void Begin()
{
    GetState().Active.Store(true);
}

void End()
{
    GetState().Active.Store(false);
}

bool IsActive()
{
    return GetState().Active.Load(MemoryOrder::Relaxed);
}

void AddSteps(uint32 count)
{
    StartupState &state = GetState();
    if (!IsActive())
    {
        return;
    }
    const ScopeLock lock(state.Lock);
    state.StepsTotal += count;
}

void BeginStep(StringView verb, StringView name)
{
    StartupState &state = GetState();
    if (!IsActive())
    {
        return;
    }
    const ScopeLock lock(state.Lock);
    StringBuilder<sizeof(StartupSnapshot::Status)> text;
    FormatTo(text, "{0} {1}", verb, name);
    WriteStatus(state, text.AsView());
}

void EndStep()
{
    StartupState &state = GetState();
    if (!IsActive())
    {
        return;
    }
    const ScopeLock lock(state.Lock);
    ++state.StepsDone;
}

void SetStatus(StringView status)
{
    StartupState &state = GetState();
    if (!IsActive())
    {
        return;
    }
    const ScopeLock lock(state.Lock);
    WriteStatus(state, status);
}

void SetMainLoopStarted()
{
    StartupState &state = GetState();
    const ScopeLock lock(state.Lock);
    state.MainLoopStarted = true;
}

StartupSnapshot GetSnapshot()
{
    StartupState &state = GetState();
    StartupSnapshot snapshot;
    const ScopeLock lock(state.Lock);
    snapshot.StepsDone = state.StepsDone;
    snapshot.StepsTotal = state.StepsTotal;
    MemCopy(snapshot.Status, state.Status, sizeof(snapshot.Status));
    snapshot.MainLoopStarted = state.MainLoopStarted;
    return snapshot;
}

StartupReadyCheckId AddReadyCheck(StringView name, Function<bool()> check)
{
    StartupState &state = GetState();
    ReadyCheck &entry = state.Checks.AddDefaulted();
    entry.Id = state.NextCheck++;
    entry.Name = String(name);
    entry.Check = Move(check);
    return entry.Id;
}

void RemoveReadyCheck(StartupReadyCheckId id)
{
    List<ReadyCheck> &checks = GetState().Checks;
    for (usize i = 0; i < checks.Count(); ++i)
    {
        if (checks[i].Id == id)
        {
            checks.RemoveAt(i);
            return;
        }
    }
}

bool AreReadyChecksPassing(StringView &failing)
{
    for (const ReadyCheck &entry : GetState().Checks)
    {
        if (!entry.Check())
        {
            failing = entry.Name.AsView();
            return false;
        }
    }
    return true;
}
} // namespace StartupProgress
} // namespace Marlin
