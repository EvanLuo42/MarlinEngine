#include "CrashReport/CrashReportClient.h"

#include "Concurrency/Atomic.h"
#include "CrashReport/CrashProtocol.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformIpc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "Misc/CommandLine.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// Mirrors the log into the shared ring, so a report carries the lines before the crash.
class CrashLogSink final : public ILogSink
{
public:
    explicit CrashLogSink(CrashSharedLog &log) : _log(log)
    {
    }

    void Write(const LogRecord &record) override
    {
        StringBuilder<1024> line;
        Log::FormatRecord(line, record);
        line.Append('\n');
        const uint64 written = _log.Written.Load(MemoryOrder::Relaxed);
        for (usize i = 0; i < line.Length(); ++i)
        {
            _log.Data[(written + i) % CrashLogCapacity] = line.Data()[i];
        }
        _log.Written.Store(written + line.Length(), MemoryOrder::Release);
    }

private:
    CrashSharedLog &_log;
};

constexpr uint32 MaxCrashHooks = 8;

struct CrashHookEntry
{
    Atomic<CrashReport::CrashHook> Hook{nullptr};
    Atomic<void *> Context{nullptr};
};

/// Never destroyed, so the crash handler can use it at any time; the events and the sink live in its storage.
struct ClientState
{
    SharedMemory Memory;
    Atomic<CrashSharedState *> Shared{nullptr};
    Event *Crashed = nullptr;
    Event *Written = nullptr;
    CrashLogSink *Sink = nullptr;
    Mutex Lock;
    /// The thread being reported, and its crash.
    Atomic<uint32> Crashing{0};
    CrashInfo First;
    CrashHookEntry Hooks[MaxCrashHooks];
    char FallbackDirectory[CrashPathCapacity] = {};
    alignas(Event) uint8 CrashedStorage[sizeof(Event)];
    alignas(Event) uint8 WrittenStorage[sizeof(Event)];
    alignas(CrashLogSink) uint8 SinkStorage[sizeof(CrashLogSink)];

    static ClientState &Get()
    {
        alignas(ClientState) static uint8 storage[sizeof(ClientState)];
        static ClientState *state = ConstructAt(reinterpret_cast<ClientState *>(storage));
        return *state;
    }
};

void CopyText(StringView source, char *target, usize capacity)
{
    const usize length = source.Length() < capacity - 1 ? source.Length() : capacity - 1;
    MemCopy(target, source.Data(), length);
    target[length] = '\0';
}

template <usize N> void CopyText(StringView source, char (&target)[N])
{
    CopyText(source, target, N);
}

template <usize N> void CopyText(const char *source, char (&target)[N])
{
    CopyText(source != nullptr ? StringView(source) : StringView(), target, N);
}

/// Appends to a terminated string in *target*, truncating; allocates nothing, for the crash handler.
template <usize N> void AppendText(char (&target)[N], StringView text)
{
    usize length = 0;
    while (length < N - 1 && target[length] != '\0')
    {
        ++length;
    }
    const usize count = text.Length() < N - 1 - length ? text.Length() : N - 1 - length;
    MemCopy(target + length, text.Data(), count);
    target[length + count] = '\0';
}

/// Without a reporter: writes the dump from the crashed process itself, which works unless the crash damaged what
/// the dump writer needs.
void DumpInProcess(const CrashInfo &info, const char *directory)
{
    if (directory[0] == '\0')
    {
        return;
    }
    (void)PlatformFile::CreateDirectory(directory);
    char path[CrashPathCapacity] = {};
    char digits[24];
    AppendText(path, directory);
    // RECOVERY-DECISION D5: in-process dump name is Crash-<UTC ticks>.dmp.
    AppendText(path, "/Crash-");
    AppendText(path, StringView(digits, FormatUnsigned(static_cast<uint64>(PlatformTime::UtcNow()), digits)));
    AppendText(path, ".dmp");
    if (PlatformCrash::WriteMinidump(path, PlatformProcess::CurrentId(), &info))
    {
        char message[CrashPathCapacity + 64] = {};
        AppendText(message, "The process crashed; the dump is ");
        AppendText(message, path);
        AppendText(message, "\n");
        PlatformDebug::Output(message);
    }
}

void Report(const CrashInfo &info);

void OnCrash(const CrashInfo &info)
{
    ClientState &state = ClientState::Get();
    // Thread ids are never zero.
    uint32 crashing = 0;
    if (!state.Crashing.CompareExchange(crashing, info.ThreadId))
    {
        if (crashing == info.ThreadId)
        {
            // A crash hook crashed: the first crash is what gets reported.
            Report(state.First);
            return;
        }
        // Another thread crashed first and is being reported; this one waits for the process to end.
        while (true)
        {
            PlatformThread::Sleep(1000);
        }
    }
    state.First = info;
    for (CrashHookEntry &entry : state.Hooks)
    {
        if (const CrashReport::CrashHook hook = entry.Hook.Load(MemoryOrder::Acquire))
        {
            hook(info, entry.Context.Load(MemoryOrder::Relaxed));
        }
    }
    Report(info);
}

/// Hands *info* over to the reporter, or dumps it from inside the process.
void Report(const CrashInfo &info)
{
    ClientState &state = ClientState::Get();
    CrashSharedState *shared = state.Shared.Load(MemoryOrder::Acquire);
    const uint32 reporter = shared != nullptr ? shared->ReporterId.Load(MemoryOrder::Acquire) : 0;
    if (reporter != 0 && PlatformProcess::IsRunning(reporter))
    {
        CrashSharedRecord &record = shared->Record;
        record.ExceptionCode = info.ExceptionCode;
        record.ThreadId = info.ThreadId;
        record.Address = reinterpret_cast<uintptr>(info.Address);
        record.PlatformContext = reinterpret_cast<uintptr>(info.PlatformContext);
        record.Time = PlatformTime::UtcNow();
        if (info.Failure != nullptr)
        {
            CopyText(info.Failure->Kind, record.FailureKind);
            CopyText(info.Failure->Message, record.FailureMessage);
            CopyText(info.Failure->File, record.FailureFile);
            record.FailureLine = info.Failure->Line;
        }
        shared->State.Store(static_cast<uint32>(CrashProcessState::Crashed), MemoryOrder::Release);
        state.Crashed->Set();
        if (state.Written->Wait(CrashWriteTimeout) &&
            shared->State.Load(MemoryOrder::Acquire) == static_cast<uint32>(CrashProcessState::Written))
        {
            return;
        }
    }
    DumpInProcess(info, state.FallbackDirectory);
}

CrashSharedState *GetShared()
{
    return ClientState::Get().Shared.Load(MemoryOrder::Acquire);
}

bool LaunchReporter(CrashSharedState &shared, StringView executableDirectory)
{
    StringBuilder<512> executable(executableDirectory);
    executable.Append("/MarlinCrashReporter");
    if (MARLIN_PLATFORM_WINDOWS)
    {
        executable.Append(".exe");
    }
    if (!PlatformFile::FileExists(executable.CStr()))
    {
        MARLIN_LOG(LogCore, Warning, "{0} is not built; crashes are dumped from inside the process",
                   executable.AsView());
        return false;
    }
    StringBuilder<64> arguments;
    FormatTo(arguments, "-Pid={0}", shared.ProcessId);
    ProcessDesc desc;
    desc.Executable = executable.CStr();
    desc.Arguments = arguments.CStr();
    desc.Flags =
        ProcessFlags::Hidden | ProcessFlags::Detached; // RECOVERY-DECISION D18: the reporter outlives the process.
    ProcessHandle process;
    if (!PlatformProcess::Create(desc, process))
    {
        MARLIN_LOG(LogCore, Warning, "Cannot start {0}; crashes are dumped from inside the process",
                   executable.AsView());
        return false;
    }
    const uint64 deadline = PlatformTime::Ticks() + PlatformTime::TicksPerSecond() * CrashAttachTimeout / 1000;
    while (shared.ReporterId.Load(MemoryOrder::Acquire) == 0 && PlatformProcess::IsRunning(process) &&
           PlatformTime::Ticks() < deadline)
    {
        PlatformThread::Sleep(5);
    }
    PlatformProcess::Close(process);
    if (shared.ReporterId.Load(MemoryOrder::Acquire) == 0)
    {
        MARLIN_LOG(LogCore, Warning, "MarlinCrashReporter did not attach; crashes are dumped from inside the process");
        return false;
    }
    return true;
}
} // namespace

namespace CrashReport
{
void Install(StringView directory)
{
    ClientState &state = ClientState::Get();
    CopyText(directory, state.FallbackDirectory);
    PlatformCrash::SetHandler(&OnCrash);
}

void Start(const CrashReportStartDesc &desc)
{
    ClientState &state = ClientState::Get();
    const ScopeLock lock(state.Lock);
    if (state.Shared.Load() != nullptr)
    {
        return;
    }
    if (!desc.ReportDirectory.IsEmpty())
    {
        CopyText(desc.ReportDirectory, state.FallbackDirectory);
    }
    PlatformCrash::SetHandler(&OnCrash);

    const uint32 processId = PlatformProcess::CurrentId();
    StringBuilder<64> name(CrashSharedMemoryPrefix);
    FormatTo(name, "{0}", processId);
    if (!PlatformSharedMemory::Create(name.CStr(), sizeof(CrashSharedState), state.Memory))
    {
        MARLIN_LOG(LogCore, Warning, "Cannot share crash state; crashes are dumped from inside the process");
        return;
    }
    auto *shared = static_cast<CrashSharedState *>(state.Memory.Data);
    shared->Magic = CrashSharedMagic;
    shared->Version = CrashSharedVersion;
    shared->ProcessId = processId;
    shared->State.Store(static_cast<uint32>(CrashProcessState::Starting));
    CrashSharedContext &context = shared->Context;
    CopyText(desc.TargetName, context.TargetName);
    CopyText(desc.EngineId, context.EngineId);
    CopyText(desc.EngineVersion, context.EngineVersion);
    CopyText(desc.Platform, context.Platform);
    CopyText(desc.Configuration, context.Configuration);
    CopyText(StringView(state.FallbackDirectory), context.ReportDirectory);
    char executable[CrashPathCapacity];
    const usize length = PlatformProcess::ExecutablePath(executable, sizeof(executable));
    CopyText(length < sizeof(executable) ? StringView(executable, length) : StringView(), context.Executable);
    CopyText(CommandLine::Get(), context.CommandLine);
    context.MaxLocalReports = CrashDefaultMaxLocalReports;
    context.LocalRetentionDays = CrashDefaultRetentionDays;

    StringBuilder<96> eventName(name.AsView());
    eventName.Append(CrashCrashedEventSuffix);
    state.Crashed = ConstructAt(reinterpret_cast<Event *>(state.CrashedStorage), EventReset::Manual, eventName.CStr());
    eventName.Truncate(name.Length());
    eventName.Append(CrashWrittenEventSuffix);
    state.Written = ConstructAt(reinterpret_cast<Event *>(state.WrittenStorage), EventReset::Manual, eventName.CStr());
    state.Sink = ConstructAt(reinterpret_cast<CrashLogSink *>(state.SinkStorage), shared->Log);
    Log::AddSink(state.Sink);
    state.Shared.Store(shared, MemoryOrder::Release);

    if (desc.LaunchReporter && !CommandLine::HasSwitch("NoCrashReporter"))
    {
        (void)LaunchReporter(*shared, desc.ExecutableDirectory);
    }
    shared->State.Store(static_cast<uint32>(CrashProcessState::Running), MemoryOrder::Release);
}

void Stop()
{
    ClientState &state = ClientState::Get();
    const ScopeLock lock(state.Lock);
    CrashSharedState *shared = state.Shared.Load();
    if (shared == nullptr)
    {
        return;
    }
    shared->State.Store(static_cast<uint32>(CrashProcessState::Exited), MemoryOrder::Release);
    state.Shared.Store(nullptr, MemoryOrder::Release);
    Log::RemoveSink(state.Sink);
    DestroyAt(state.Sink);
    DestroyAt(state.Crashed);
    DestroyAt(state.Written);
    state.Sink = nullptr;
    state.Crashed = nullptr;
    state.Written = nullptr;
    PlatformSharedMemory::Close(state.Memory);
}

bool IsReporterAttached()
{
    const CrashSharedState *shared = GetShared();
    return shared != nullptr && shared->ReporterId.Load(MemoryOrder::Acquire) != 0;
}

void SetReportDirectory(StringView directory)
{
    ClientState &state = ClientState::Get();
    const ScopeLock lock(state.Lock);
    CopyText(directory, state.FallbackDirectory);
    if (CrashSharedState *shared = GetShared())
    {
        CopyText(directory, shared->Context.ReportDirectory);
    }
}

void SetProduct(StringView projectName, StringView productName, StringView productVersion)
{
    const ScopeLock lock(ClientState::Get().Lock);
    if (CrashSharedState *shared = GetShared())
    {
        CopyText(projectName, shared->Context.ProjectName);
        CopyText(productName, shared->Context.ProductName);
        CopyText(productVersion, shared->Context.ProductVersion);
    }
}

void SetUpload(StringView host, uint16 port, bool askUser)
{
    const ScopeLock lock(ClientState::Get().Lock);
    if (CrashSharedState *shared = GetShared())
    {
        CopyText(host, shared->Context.ServerHost);
        shared->Context.ServerPort = port;
        shared->Context.AskUser = askUser ? 1 : 0;
    }
}

void SetHangSeconds(uint32 seconds)
{
    const ScopeLock lock(ClientState::Get().Lock);
    if (CrashSharedState *shared = GetShared())
    {
        shared->Context.HangSeconds = seconds;
    }
}

void SetPrivacy(bool scrub, StringView keptArguments)
{
    const ScopeLock lock(ClientState::Get().Lock);
    if (CrashSharedState *shared = GetShared())
    {
        shared->Context.Scrub = scrub ? 1 : 0;
        CopyText(keptArguments, shared->Context.KeptArguments);
    }
}

void SetRetention(uint32 maxReports, uint32 days)
{
    const ScopeLock lock(ClientState::Get().Lock);
    if (CrashSharedState *shared = GetShared())
    {
        shared->Context.MaxLocalReports = maxReports;
        shared->Context.LocalRetentionDays = days;
    }
}

bool AddAttachment(StringView path)
{
    const ScopeLock lock(ClientState::Get().Lock);
    CrashSharedState *shared = GetShared();
    if (shared == nullptr || path.IsEmpty() || path.Length() >= CrashPathCapacity)
    {
        return false;
    }
    CrashSharedAttachments &attachments = shared->Attachments;
    const uint32 count = attachments.Count.Load(MemoryOrder::Relaxed);
    if (count >= CrashMaxAttachments)
    {
        return false;
    }
    CopyText(path, attachments.Paths[count]);
    attachments.Count.Store(count + 1, MemoryOrder::Release);
    return true;
}

bool AddCrashHook(CrashHook hook, void *context)
{
    ClientState &state = ClientState::Get();
    const ScopeLock lock(state.Lock);
    for (CrashHookEntry &entry : state.Hooks)
    {
        if (entry.Hook.Load(MemoryOrder::Relaxed) == nullptr)
        {
            // The context first: the crash handler reads the hook, then its context.
            entry.Context.Store(context, MemoryOrder::Relaxed);
            entry.Hook.Store(hook, MemoryOrder::Release);
            return true;
        }
    }
    return false;
}

void RemoveCrashHook(CrashHook hook, void *context)
{
    ClientState &state = ClientState::Get();
    const ScopeLock lock(state.Lock);
    for (CrashHookEntry &entry : state.Hooks)
    {
        if (entry.Hook.Load(MemoryOrder::Relaxed) == hook && entry.Context.Load(MemoryOrder::Relaxed) == context)
        {
            entry.Hook.Store(nullptr, MemoryOrder::Release);
        }
    }
}

void Heartbeat(uint64 frame)
{
    CrashSharedState *shared = GetShared();
    if (shared == nullptr)
    {
        return;
    }
    shared->HeartbeatThread.Store(PlatformThread::CurrentId(), MemoryOrder::Relaxed);
    shared->HeartbeatFrame.Store(frame, MemoryOrder::Relaxed);
    shared->DebuggerAttached.Store(PlatformDebug::IsDebuggerAttached() ? 1 : 0, MemoryOrder::Relaxed);
    shared->HeartbeatTicks.Store(PlatformTime::Ticks(), MemoryOrder::Release);
    PlatformCrash::RestoreHandler();
}
} // namespace CrashReport
} // namespace Marlin
