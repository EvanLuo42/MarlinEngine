#pragma once

#include "CoreTypes.h"
#include "CrashReport/CrashProtocol.h"
#include "CrashReport/CrashReportInfo.h"
#include "HAL/PlatformIpc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSync.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"

namespace Marlin
{
/// Watches one process through the state it shares (Documentation/Architecture/03 §7.1): writes its dump from
/// outside when it crashes, a snapshot when its main loop hangs and a report without a dump when it dies without
/// its crash handler running, then uploads what it wrote.
class CrashWatcher
{
public:
    CrashWatcher() = default;
    ~CrashWatcher();
    MARLIN_NONCOPYABLE(CrashWatcher);

    /// Opens the shared state of process *processId* and tells it a reporter is attached.
    bool Attach(uint32 processId);
    /// Returns once the process has ended and its crash, if any, is reported.
    void Run();

private:
    /// `<ReportDirectory>/<time>`, created.
    String CreateReportDirectory(int64 time);
    void WriteReport(const String &directory, CrashReportInfo &info, bool withDump);
    void HandleCrash();
    void HandleHang();
    void HandleAbnormalExit(int32 exitCode);
    /// Asks the user if the settings say so, then uploads.
    void Submit(const String &directory, CrashReportInfo &info, StringView headline);
    [[nodiscard]] CrashReportInfo MakeInfo(CrashKind kind);

    uint32 _processId = 0;
    SharedMemory _memory;
    CrashSharedState *_shared = nullptr;
    ProcessHandle _process;
    UniquePtr<Event> _crashed;
    UniquePtr<Event> _written;
};
} // namespace Marlin
