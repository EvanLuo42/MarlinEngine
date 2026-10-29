#pragma once

#include "CoreTypes.h"
#include "HAL/PlatformCrash.h"
#include "String/StringView.h"

namespace Marlin
{
struct CrashReportStartDesc
{
    StringView TargetName;
    StringView EngineId;
    StringView EngineVersion;
    StringView Platform;
    StringView Configuration;
    /// Where MarlinCrashReporter is.
    StringView ExecutableDirectory;
    /// Where report folders go; empty keeps the directory Install was given.
    StringView ReportDirectory;
    /// Starts a MarlinCrashReporter for this process; without one a crash is dumped from inside the process.
    bool LaunchReporter = false;
};

/// Crash reporting from inside the process (Documentation/Architecture/03 §7.1): the shared state a
/// MarlinCrashReporter watches, the crash handler that hands a crash over to it, and a dump written from inside the
/// process when there is no reporter. Engine startup calls these; the setters can be called again at any time.
namespace CrashReport
{
/// Installs the crash handler as early as possible, before anything else can crash; until Start, a crash is dumped
/// from inside the process into *directory*.
CRASHREPORT_API void Install(StringView directory);
/// Shares this process's crash state and, if asked, launches and waits for a MarlinCrashReporter.
CRASHREPORT_API void Start(const CrashReportStartDesc &desc);
/// Marks a normal exit, so the reporter does not report it.
CRASHREPORT_API void Stop();
/// True once a MarlinCrashReporter watches this process.
[[nodiscard]] CRASHREPORT_API bool IsReporterAttached();

CRASHREPORT_API void SetReportDirectory(StringView directory);
CRASHREPORT_API void SetProduct(StringView projectName, StringView productName, StringView productVersion);
/// An empty *host* keeps reports local; *askUser* shows the crash dialog before uploading.
CRASHREPORT_API void SetUpload(StringView host, uint16 port, bool askUser);
/// Zero turns hang detection off.
CRASHREPORT_API void SetHangSeconds(uint32 seconds);
/// *scrub* removes user, computer and home folder names from what is uploaded; *keptArguments*, space-separated,
/// are the command-line switches whose values are kept.
CRASHREPORT_API void SetPrivacy(bool scrub, StringView keptArguments);
/// How many local report folders are kept, and for how many days.
CRASHREPORT_API void SetRetention(uint32 maxReports, uint32 days);
/// Copies the file at *path* into the report of a crash, such as a GPU crash dump; false if there is no room.
CRASHREPORT_API bool AddAttachment(StringView path);
/// Runs on the crashed thread before the crash is handed over, to log what only this process can still see, such
/// as the GPU's state when its driver crashed. A hook should be quick and must not crash; if one does, the first
/// crash is reported without the remaining hooks.
using CrashHook = void (*)(const CrashInfo &info, void *context);
/// False if all hook slots are taken.
CRASHREPORT_API bool AddCrashHook(CrashHook hook, void *context);
CRASHREPORT_API void RemoveCrashHook(CrashHook hook, void *context);
/// Called by the main loop every frame; a reporter reports a hang when the heartbeat stops.
CRASHREPORT_API void Heartbeat(uint64 frame);
} // namespace CrashReport
} // namespace Marlin
