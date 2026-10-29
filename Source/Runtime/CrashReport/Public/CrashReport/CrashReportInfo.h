#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"
#include "CrashReport/CrashReportInfo.gen.h"

namespace Marlin
{
MENUM()
enum class CrashKind : uint8
{
    /// An unhandled exception, such as an access violation.
    Exception,
    /// An assertion, a fatal error or a runtime-library failure ended the process on purpose.
    Failure,
    /// The main loop stopped beating; the process kept running.
    Hang,
    /// The process ended without its crash handler running, so there is no dump.
    AbnormalExit,
};

/// Report.mtext of a report folder: what happened and to which build.
MSTRUCT()
struct CrashReportInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    CrashKind Kind = CrashKind::Exception;
    /// UTC, such as 2026-10-27T20:15:03.
    MPROPERTY()
    String Time;
    MPROPERTY()
    uint32 ProcessId = 0;
    MPROPERTY()
    uint32 ThreadId = 0;
    MPROPERTY()
    uint32 ExceptionCode = 0;
    MPROPERTY()
    uint64 Address = 0;
    /// For AbnormalExit.
    MPROPERTY()
    int32 ExitCode = 0;
    MPROPERTY()
    String FailureKind;
    MPROPERTY()
    String FailureMessage;
    MPROPERTY()
    String FailureFile;
    MPROPERTY()
    int32 FailureLine = 0;

    MPROPERTY()
    String TargetName;
    MPROPERTY()
    String EngineId;
    MPROPERTY()
    String EngineVersion;
    MPROPERTY()
    String ProductName;
    MPROPERTY()
    String ProductVersion;
    MPROPERTY()
    String Platform;
    MPROPERTY()
    String Configuration;
    MPROPERTY()
    String ProjectName;
    MPROPERTY()
    String Executable;
    MPROPERTY()
    String CommandLine;

    /// What the user wrote in the crash dialog.
    MPROPERTY()
    String Description;
    /// File names in the report folder besides the standard ones.
    MPROPERTY()
    List<String> Attachments;
};

namespace CrashReport
{
[[nodiscard]] CRASHREPORT_API String WriteInfo(const CrashReportInfo &info);
/// False, with what went wrong in *errors*, if *text* holds no readable CrashReportInfo.
CRASHREPORT_API bool ReadInfo(StringView text, CrashReportInfo &info, String &errors);
/// Such as "Exception".
[[nodiscard]] CRASHREPORT_API StringView GetKindName(CrashKind kind);
} // namespace CrashReport
} // namespace Marlin
