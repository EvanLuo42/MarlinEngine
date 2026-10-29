#include "HAL/PlatformThread.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformCrash.h"

namespace Marlin
{
static_assert(PlatformTls::InvalidSlot == TLS_OUT_OF_INDEXES);

namespace
{
struct ThreadStart
{
    ThreadEntry Entry;
    void *Argument;
};

DWORD WINAPI ThreadTrampoline(LPVOID parameter)
{
    const ThreadStart start = *static_cast<ThreadStart *>(parameter);
    HeapFree(GetProcessHeap(), 0, parameter);
    ULONG stackGuarantee = PlatformCrash::HandlerStackReserve;
    SetThreadStackGuarantee(&stackGuarantee);
    start.Entry(start.Argument);
    return 0;
}

int ThreadPriorityValue(ThreadPriority priority)
{
    switch (priority)
    {
    case ThreadPriority::Lowest:
        return THREAD_PRIORITY_LOWEST;
    case ThreadPriority::BelowNormal:
        return THREAD_PRIORITY_BELOW_NORMAL;
    case ThreadPriority::Normal:
        return THREAD_PRIORITY_NORMAL;
    case ThreadPriority::AboveNormal:
        return THREAD_PRIORITY_ABOVE_NORMAL;
    case ThreadPriority::Highest:
        return THREAD_PRIORITY_HIGHEST;
    case ThreadPriority::TimeCritical:
        return THREAD_PRIORITY_TIME_CRITICAL;
    }
    MARLIN_UNREACHABLE();
}

void SetThreadName(HANDLE thread, const char *name)
{
    wchar_t wideName[128];
    if (Microsoft::Utf8ToWide(name, wideName, sizeof(wideName) / sizeof(wideName[0])) <
        sizeof(wideName) / sizeof(wideName[0]))
    {
        SetThreadDescription(thread, wideName);
    }
}
} // namespace

namespace PlatformThread
{
ThreadHandle Create(const ThreadDesc &desc)
{
    MARLIN_ASSERT(desc.Entry != nullptr);
    auto *start = static_cast<ThreadStart *>(HeapAlloc(GetProcessHeap(), 0, sizeof(ThreadStart)));
    if (start == nullptr)
    {
        return {};
    }
    *start = {.Entry = desc.Entry, .Argument = desc.Argument};
    const DWORD flags = desc.StackSize != 0 ? CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION : CREATE_SUSPENDED;
    const HANDLE thread = CreateThread(nullptr, desc.StackSize, ThreadTrampoline, start, flags, nullptr);
    if (thread == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, start);
        return {};
    }
    if (desc.Name != nullptr)
    {
        SetThreadName(thread, desc.Name);
    }
    if (desc.Priority != ThreadPriority::Normal)
    {
        SetThreadPriority(thread, ThreadPriorityValue(desc.Priority));
    }
    if (desc.AffinityMask != 0)
    {
        SetThreadAffinityMask(thread, static_cast<DWORD_PTR>(desc.AffinityMask));
    }
    ResumeThread(thread);
    return {reinterpret_cast<uintptr>(thread)};
}

void Join(ThreadHandle thread)
{
    MARLIN_ASSERT(thread.IsValid());
    auto *const handle = reinterpret_cast<HANDLE>(thread.Value);
    WaitForSingleObject(handle, INFINITE);
    CloseHandle(handle);
}

uint32 CurrentId()
{
    return GetCurrentThreadId();
}

void SetCurrentName(const char *name)
{
    SetThreadName(GetCurrentThread(), name);
}

bool SetCurrentAffinity(uint64 affinityMask)
{
    return SetThreadAffinityMask(GetCurrentThread(), static_cast<DWORD_PTR>(affinityMask)) != 0;
}

void Sleep(uint32 milliseconds)
{
    ::Sleep(milliseconds);
}

void YieldThread()
{
    SwitchToThread();
}
} // namespace PlatformThread

namespace PlatformTls
{
uint32 Allocate()
{
    return TlsAlloc();
}

void Free(uint32 slot)
{
    MARLIN_VERIFY(TlsFree(slot));
}

void *Get(uint32 slot)
{
    return TlsGetValue(slot);
}

void Set(uint32 slot, void *value)
{
    MARLIN_VERIFY(TlsSetValue(slot, value));
}
} // namespace PlatformTls
} // namespace Marlin
