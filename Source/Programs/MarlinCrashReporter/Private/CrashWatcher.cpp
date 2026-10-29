#include "CrashWatcher.h"

#include "CrashAnalysis/CrashAnalysis.h"
#include "CrashDialog.h"
#include "CrashReport/CrashPrivacy.h"
#include "CrashUploader.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Algorithm.h"

namespace Marlin
{
namespace
{
constexpr uint32 PollMilliseconds = 100; // RECOVERY-DECISION D6: reporter timing chosen (not recovered).
/// Seconds after attaching before reports that earlier runs could not upload are sent: by then the process has
/// loaded its config and set the server.
constexpr uint32 PendingDelaySeconds = 30; // RECOVERY-DECISION D6: reporter timing chosen (not recovered).

bool WriteFile(StringView path, StringView text)
{
    const String file(path);
    const FileHandle handle = PlatformFile::Open(file.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!handle.IsValid())
    {
        return false;
    }
    const bool succeeded = PlatformFile::Write(handle, text.Data(), text.Length());
    PlatformFile::Close(handle);
    return succeeded;
}

bool ReadFile(StringView path, String &text)
{
    const String file(path);
    const FileHandle handle = PlatformFile::Open(file.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!handle.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    const bool succeeded = PlatformFile::GetSize(handle, size) && size < (uint64{1} << 31) &&
                           PlatformFile::Read(handle, text.ResizeUninitialized(static_cast<usize>(size)),
                                              static_cast<usize>(size), read) &&
                           read == size;
    PlatformFile::Close(handle);
    return succeeded;
}

// RECOVERY-DECISION D16: .txt, .mtext and .log attachments are scrubbed; others are copied as they are.
/// Files that are scrubbed like the report, rather than sent as they are.
bool IsText(StringView name)
{
    return name.EndsWith(".txt") || name.EndsWith(".mtext") || name.EndsWith(".log");
}

/// Deletes the report folders in *root* older than *days*, then the oldest beyond *maxReports*. Only folders that
/// hold a Report.mtext are touched.
void PruneReports(StringView root, uint32 maxReports, uint32 days)
{
    struct Folder
    {
        String Name;
        int64 Time = 0;
    };
    List<Folder> folders;
    DirectoryIterator iterator;
    DirectoryEntry entry;
    const String path(root);
    if (root.IsEmpty() || !iterator.Open(path.CStr()))
    {
        return;
    }
    while (iterator.Next(entry))
    {
        if (entry.Info.IsDirectory &&
            PlatformFile::FileExists(Format("{0}/{1}/{2}", root, entry.Name, CrashReportFiles::Report).CStr()))
        {
            folders.Add({.Name = String(entry.Name), .Time = entry.Info.ModifiedTime});
        }
    }
    iterator.Close();
    Sort(folders.Data(), folders.Count(), [](const Folder &a, const Folder &b) { return a.Time > b.Time; });
    const int64 cutoff = PlatformTime::UtcNow() - int64{days} * TimeSpan::TicksPerDay;
    for (usize i = 0; i < folders.Count(); ++i)
    {
        if ((maxReports != 0 && i >= maxReports) || (days != 0 && folders[i].Time < cutoff))
        {
            (void)PlatformFile::DeleteDirectory(Format("{0}/{1}", root, folders[i].Name).CStr(), true);
        }
    }
}

bool Exists(StringView directory, StringView name)
{
    return PlatformFile::FileExists(Format("{0}/{1}", directory, name).CStr());
}

StringView FileNameOf(StringView path)
{
    usize start = path.Length();
    while (start > 0)
    {
        if (path[start - 1] == '/' || path[start - 1] == '\\')
        {
            break;
        }
        --start;
    }
    return path.Substring(start);
}

// RECOVERY-DECISION D17: any NTSTATUS error exit but Ctrl+C counts as an abnormal exit.
constexpr uint32 ControlCExit = 0xC000013Au;

/// An exit code that only a failure gives: an NTSTATUS error, such as a fail-fast that bypasses every handler. A
/// console window closed with Ctrl+C is not a crash.
bool IsAbnormalExit(int32 exitCode)
{
    const auto code = static_cast<uint32>(exitCode);
    return code >= 0xC0000000u && code != ControlCExit;
}

String TimeText(int64 ticks, StringView spec)
{
    StringBuilder<64> text;
    DateTime(ticks).FormatTo(text, spec);
    return text.ToString();
}

struct PendingUpload
{
    String Root;
    String Host;
    uint16 Port = 0;
    uint32 MaxReports = 0;
    uint32 Days = 0;
};

/// Uploads the folders in Root that may be sent but are not uploaded, such as those a missing server left behind.
void UploadPending(void *argument)
{
    const auto *pending = static_cast<const PendingUpload *>(argument);
    DirectoryIterator iterator;
    DirectoryEntry entry;
    List<String> folders;
    if (!pending->Root.IsEmpty() && iterator.Open(pending->Root.CStr()))
    {
        while (iterator.Next(entry))
        {
            const String folder = Format("{0}/{1}", pending->Root, entry.Name);
            if (entry.Info.IsDirectory && Exists(folder, CrashReportFiles::Report) &&
                Exists(folder, CrashReportFiles::Consented) && !Exists(folder, CrashReportFiles::Uploaded))
            {
                folders.Add(folder);
            }
        }
        iterator.Close();
    }
    for (const String &folder : folders)
    {
        CrashUploadReply reply;
        if (!UploadReport(folder, pending->Host, pending->Port, reply))
        {
            break;
        }
        if (reply.Status != CrashUploadStatus::Error)
        {
            (void)WriteFile(Format("{0}/{1}", folder, CrashReportFiles::Uploaded), {});
        }
    }
    PruneReports(pending->Root, pending->MaxReports, pending->Days);
}
} // namespace

CrashWatcher::~CrashWatcher()
{
    _crashed.Reset();
    _written.Reset();
    PlatformSharedMemory::Close(_memory);
    if (_process.IsValid())
    {
        PlatformProcess::Close(_process);
    }
}

bool CrashWatcher::Attach(uint32 processId)
{
    StringBuilder<64> name(CrashSharedMemoryPrefix);
    FormatTo(name, "{0}", processId);
    if (!PlatformSharedMemory::Open(name.CStr(), sizeof(CrashSharedState), _memory))
    {
        MARLIN_LOG(LogCore, Error, "Process {0} shares no crash state", processId);
        return false;
    }
    _shared = static_cast<CrashSharedState *>(_memory.Data);
    if (_shared->Magic != CrashSharedMagic || _shared->Version != CrashSharedVersion ||
        _shared->ProcessId != processId || !PlatformProcess::Open(processId, _process))
    {
        MARLIN_LOG(LogCore, Error, "Process {0} shares no crash state", processId);
        return false;
    }
    _processId = processId;
    StringBuilder<96> eventName(name.AsView());
    eventName.Append(CrashCrashedEventSuffix);
    _crashed = MakeUnique<Event>(EventReset::Manual, eventName.CStr());
    eventName.Truncate(name.Length());
    eventName.Append(CrashWrittenEventSuffix);
    _written = MakeUnique<Event>(EventReset::Manual, eventName.CStr());
    uint32 expected = 0;
    return _shared->ReporterId.CompareExchange(expected, PlatformProcess::CurrentId());
}

void CrashWatcher::Run()
{
    const uint64 ticksPerSecond = PlatformTime::TicksPerSecond();
    const uint64 attached = PlatformTime::Ticks();
    PendingUpload pending;
    ThreadHandle pendingThread;
    bool hung = false;
    while (true)
    {
        if (_crashed->Wait(PollMilliseconds) &&
            _shared->State.Load(MemoryOrder::Acquire) == static_cast<uint32>(CrashProcessState::Crashed))
        {
            HandleCrash();
            break;
        }
        if (!PlatformProcess::IsRunning(_process))
        {
            int32 exitCode = 0;
            const auto state = static_cast<CrashProcessState>(_shared->State.Load(MemoryOrder::Acquire));
            if (PlatformProcess::GetExitCode(_process, exitCode) && state != CrashProcessState::Exited &&
                state != CrashProcessState::Written && IsAbnormalExit(exitCode))
            {
                HandleAbnormalExit(exitCode);
            }
            break;
        }

        const CrashSharedContext &context = _shared->Context;
        const uint64 now = PlatformTime::Ticks();
        const uint64 heartbeat = _shared->HeartbeatTicks.Load(MemoryOrder::Acquire);
        const bool stopped = context.HangSeconds != 0 && heartbeat != 0 && now > heartbeat &&
                             (now - heartbeat) / ticksPerSecond >= context.HangSeconds &&
                             _shared->DebuggerAttached.Load(MemoryOrder::Relaxed) == 0;
        if (stopped && !hung)
        {
            HandleHang();
        }
        hung = stopped;

        if (!pendingThread.IsValid() && context.ServerHost[0] != '\0' &&
            (now - attached) / ticksPerSecond >= PendingDelaySeconds)
        {
            pending = {.Root = String(context.ReportDirectory),
                       .Host = String(context.ServerHost),
                       .Port = context.ServerPort,
                       .MaxReports = context.MaxLocalReports,
                       .Days = context.LocalRetentionDays};
            pendingThread = PlatformThread::Create(
                {.Name = "CrashReporter.Pending", .Entry = &UploadPending, .Argument = &pending});
        }
    }
    if (pendingThread.IsValid())
    {
        PlatformThread::Join(pendingThread);
    }
}

String CrashWatcher::CreateReportDirectory(int64 time)
{
    // RECOVERY-DECISION D4: local report folders are <UTC yyyyMMdd-HHmmss>-<pid>.
    const String directory = Format("{0}/{1}-{2}", StringView(_shared->Context.ReportDirectory),
                                    TimeText(time, "yyyyMMdd-HHmmss"), _processId);
    if (_shared->Context.ReportDirectory[0] == '\0' || !PlatformFile::CreateDirectory(directory.CStr()))
    {
        MARLIN_LOG(LogCore, Error, "Cannot create {0}", directory);
        return {};
    }
    return directory;
}

CrashReportInfo CrashWatcher::MakeInfo(CrashKind kind)
{
    const CrashSharedContext &context = _shared->Context;
    const CrashSharedRecord &record = _shared->Record;
    CrashReportInfo info;
    info.Kind = kind;
    info.Time = TimeText(record.Time != 0 ? record.Time : PlatformTime::UtcNow(), "s");
    info.ProcessId = _processId;
    info.TargetName = String(context.TargetName);
    info.EngineId = String(context.EngineId);
    info.EngineVersion = String(context.EngineVersion);
    info.ProductName = String(context.ProductName);
    info.ProductVersion = String(context.ProductVersion);
    info.Platform = String(context.Platform);
    info.Configuration = String(context.Configuration);
    info.ProjectName = String(context.ProjectName);
    info.Executable = String(context.Executable);
    info.CommandLine = String(context.CommandLine);
    return info;
}

void CrashWatcher::WriteReport(const String &directory, CrashReportInfo &info, bool withDump)
{
    const CrashSharedContext &context = _shared->Context;
    const CrashSharedRecord &record = _shared->Record;
    List<CrashScrubPattern> patterns;
    if (context.Scrub != 0)
    {
        patterns = CrashPrivacy::MakeLocalPatterns();
        List<StringView> kept;
        for (const StringView argument : StringView(context.KeptArguments).Split(' '))
        {
            if (!argument.IsEmpty())
            {
                kept.Add(argument);
            }
        }
        info.CommandLine = CrashPrivacy::FilterCommandLine(info.CommandLine, kept.AsSpan());
    }
    const auto scrubbed = [&](StringView text) { return CrashPrivacy::Scrub(text, patterns.AsSpan()); };

    const String dump = Format("{0}/{1}", directory, CrashReportFiles::Minidump);
    if (withDump)
    {
        const CrashInfo crash = {
            .ExceptionCode = record.ExceptionCode,
            .Address = reinterpret_cast<void *>(static_cast<uintptr>(record.Address)),
            .ThreadId = record.ThreadId,
            .PlatformContext = reinterpret_cast<void *>(static_cast<uintptr>(record.PlatformContext)),
        };
        const bool crashed = info.Kind == CrashKind::Exception || info.Kind == CrashKind::Failure;
        if (!PlatformCrash::WriteMinidump(dump.CStr(), _processId, crashed ? &crash : nullptr))
        {
            MARLIN_LOG(LogCore, Error, "Cannot write {0}", dump);
        }
    }

    // The log ring holds the last CrashLogCapacity bytes, oldest at the write position.
    const CrashSharedLog &log = _shared->Log;
    const uint64 written = log.Written.Load(MemoryOrder::Acquire);
    const uint64 count = written < CrashLogCapacity ? written : CrashLogCapacity;
    String lines;
    char *target = lines.ResizeUninitialized(static_cast<usize>(count));
    for (uint64 i = 0; i < count; ++i)
    {
        target[i] = log.Data[(written - count + i) % CrashLogCapacity];
    }
    (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Log), scrubbed(lines));

    const uint32 attachments = _shared->Attachments.Count.Load(MemoryOrder::Acquire);
    for (uint32 i = 0; i < attachments && i < CrashMaxAttachments; ++i)
    {
        const StringView source(_shared->Attachments.Paths[i]);
        const StringView name = FileNameOf(source);
        const String target = Format("{0}/{1}", directory, name);
        String text;
        const bool copied = IsText(name) && ReadFile(source, text)
                                ? WriteFile(target, scrubbed(text))
                                : PlatformFile::Copy(String(source).CStr(), target.CStr(), false);
        if (copied)
        {
            info.Attachments.Add(String(name));
        }
    }

    if (withDump)
    {
        CrashAnalysisDesc desc;
        desc.Minidump = dump;
        desc.FocusThread = info.Kind == CrashKind::Hang ? _shared->HeartbeatThread.Load() : record.ThreadId;
        CrashAnalysisResult result;
        String error;
        if (CrashAnalyzer::Analyze(desc, result, error))
        {
            StringBuilder<16384> stack;
            CrashAnalyzer::WriteText(stack, result);
            (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Stack), scrubbed(stack.AsView()));
        }
    }
    (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Report), scrubbed(CrashReport::WriteInfo(info)));
}

void CrashWatcher::HandleCrash()
{
    const CrashSharedRecord &record = _shared->Record;
    const bool failure = record.FailureKind[0] != '\0';
    CrashReportInfo info = MakeInfo(failure ? CrashKind::Failure : CrashKind::Exception);
    info.ThreadId = record.ThreadId;
    info.ExceptionCode = record.ExceptionCode;
    info.Address = record.Address;
    info.FailureKind = String(record.FailureKind);
    info.FailureMessage = String(record.FailureMessage);
    info.FailureFile = String(record.FailureFile);
    info.FailureLine = record.FailureLine;
    const String directory = CreateReportDirectory(record.Time != 0 ? record.Time : PlatformTime::UtcNow());
    if (!directory.IsEmpty())
    {
        WriteReport(directory, info, true);
    }
    // The process waits for this before it ends; the upload can take as long as it needs.
    _shared->State.Store(static_cast<uint32>(CrashProcessState::Written), MemoryOrder::Release);
    _written->Set();
    if (!directory.IsEmpty())
    {
        Submit(directory, info,
               "The program crashed."); // RECOVERY-DECISION D9: dialog/headline text chosen (not recovered).
    }
}

void CrashWatcher::HandleHang()
{
    CrashReportInfo info = MakeInfo(CrashKind::Hang);
    info.Time = TimeText(PlatformTime::UtcNow(), "s");
    info.ThreadId = _shared->HeartbeatThread.Load(MemoryOrder::Relaxed);
    const String directory = CreateReportDirectory(PlatformTime::UtcNow());
    if (directory.IsEmpty())
    {
        return;
    }
    WriteReport(directory, info, true);
    Submit(directory, info,
           "The program stopped responding."); // RECOVERY-DECISION D9: dialog/headline text chosen (not recovered).
}

void CrashWatcher::HandleAbnormalExit(int32 exitCode)
{
    CrashReportInfo info = MakeInfo(CrashKind::AbnormalExit);
    info.Time = TimeText(PlatformTime::UtcNow(), "s");
    info.ExitCode = exitCode;
    const String directory = CreateReportDirectory(PlatformTime::UtcNow());
    if (directory.IsEmpty())
    {
        return;
    }
    WriteReport(directory, info, false);
    Submit(directory, info,
           "The program closed unexpectedly."); // RECOVERY-DECISION D9: dialog/headline text chosen (not recovered).
}

void CrashWatcher::Submit(const String &directory, CrashReportInfo &info, StringView headline)
{
    const CrashSharedContext &context = _shared->Context;
    const String root(StringView(context.ReportDirectory));
    const String host(StringView(context.ServerHost));
    if (!host.IsEmpty())
    {
        bool send = true;
        if (context.AskUser != 0)
        {
            String description;
            CrashDialogDesc desc;
            desc.ProductName = !info.ProductName.IsEmpty() ? info.ProductName.AsView() : info.TargetName.AsView();
            desc.Headline = headline;
            desc.ReportDirectory = directory;
            send = AskToSendReport(desc, description);
            if (send && !description.IsEmpty())
            {
                info.Description = Move(description);
                const String text = CrashReport::WriteInfo(info);
                (void)WriteFile(
                    Format("{0}/{1}", directory, CrashReportFiles::Report),
                    context.Scrub != 0 ? CrashPrivacy::Scrub(text, CrashPrivacy::MakeLocalPatterns().AsSpan()) : text);
            }
        }
        if (!send)
        {
            (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Declined), {});
        }
        else
        {
            (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Consented), {});
            CrashUploadReply reply;
            if (UploadReport(directory, host, context.ServerPort, reply) && reply.Status != CrashUploadStatus::Error)
            {
                (void)WriteFile(Format("{0}/{1}", directory, CrashReportFiles::Uploaded), {});
                MARLIN_LOG(LogCore, Info, "Uploaded {0} as report {1}, bucket {2}", directory, reply.Report,
                           CrashAnalyzer::FormatSignature(reply.Signature));
            }
        }
    }
    PruneReports(root, context.MaxLocalReports, context.LocalRetentionDays);
}
} // namespace Marlin
