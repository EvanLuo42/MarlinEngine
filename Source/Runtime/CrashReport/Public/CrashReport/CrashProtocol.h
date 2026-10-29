#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"

namespace Marlin
{
// Between a process and its MarlinCrashReporter (Documentation/Architecture/03 §7.1): a named shared memory region
// and two named events, all named after the process id.

inline constexpr const char *CrashSharedMemoryPrefix = "Marlin.Crash.";
inline constexpr const char *CrashCrashedEventSuffix = ".Crashed";
inline constexpr const char *CrashWrittenEventSuffix = ".Written";
inline constexpr uint32 CrashSharedMagic =
    0x5243524Du; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr uint32 CrashSharedVersion =
    1; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
/// Milliseconds the crashed process waits for the reporter to write the dump before dumping itself.
inline constexpr uint32 CrashWriteTimeout =
    60000; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
/// Milliseconds a starting process waits for a reporter it launched to attach.
inline constexpr uint32 CrashAttachTimeout =
    5000; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).

inline constexpr usize CrashTextCapacity =
    256; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr usize CrashPathCapacity = 260;
inline constexpr usize CrashMessageCapacity = 2048;
inline constexpr usize CrashCommandLineCapacity =
    4096; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr usize CrashMaxAttachments =
    16; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr usize CrashLogCapacity =
    64 * 1024; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr uint32 CrashDefaultMaxLocalReports =
    20; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).
inline constexpr uint32 CrashDefaultRetentionDays =
    30; // RECOVERY-DECISION D1: shared-state/protocol constant chosen (value not recovered).

enum class CrashProcessState : uint32
{
    Starting,
    Running,
    /// The process crashed and waits for the reporter.
    Crashed,
    /// The reporter wrote the dump; the process may end.
    Written,
    /// The process shut down normally.
    Exited,
};

/// What the report says about the process; written as the engine starts, read after a crash.
struct CrashSharedContext
{
    char TargetName[64];
    char EngineId[64];
    char EngineVersion[32];
    char ProductName[CrashTextCapacity];
    char ProductVersion[64];
    char Platform[16];
    char Configuration[16];
    char ProjectName[CrashTextCapacity];
    char Executable[CrashPathCapacity];
    char CommandLine[CrashCommandLineCapacity];
    /// Where report folders go: /User/Crashes once the VFS is up.
    char ReportDirectory[CrashPathCapacity];
    /// The MarlinCrashServer reports are uploaded to; empty to keep them local.
    char ServerHost[CrashTextCapacity];
    uint16 ServerPort;
    /// Nonzero to show the crash dialog before uploading.
    uint8 AskUser;
    /// Nonzero to remove user, computer and home folder names from what is uploaded.
    uint8 Scrub;
    /// Seconds without a heartbeat before the main loop counts as hung; zero turns hang detection off.
    uint32 HangSeconds;
    /// Space-separated command-line switches whose values survive scrubbing.
    char KeptArguments[CrashTextCapacity];
    /// Local report folders kept; older ones are deleted.
    uint32 MaxLocalReports;
    /// Days local report folders are kept.
    uint32 LocalRetentionDays;
};

/// The crash, filled by the crashed thread.
struct CrashSharedRecord
{
    uint32 ExceptionCode;
    uint32 ThreadId;
    uint64 Address;
    /// EXCEPTION_POINTERS on Windows, an address in the crashed process.
    uint64 PlatformContext;
    /// DateTime ticks, UTC.
    int64 Time;
    char FailureKind[64];
    char FailureMessage[CrashMessageCapacity];
    char FailureFile[CrashPathCapacity];
    int32 FailureLine;
    uint32 Padding;
};

/// Files to copy into the report folder, such as GPU crash dumps.
struct CrashSharedAttachments
{
    Atomic<uint32> Count;
    uint32 Padding;
    char Paths[CrashMaxAttachments][CrashPathCapacity];
};

/// The latest log lines: a ring that the log sink writes and the reporter reads. Written counts every byte ever
/// written, so the ring holds the last min(Written, CrashLogCapacity) of them.
struct CrashSharedLog
{
    Atomic<uint64> Written;
    char Data[CrashLogCapacity];
};

struct CrashSharedState
{
    uint32 Magic;
    uint32 Version;
    uint32 ProcessId;
    Atomic<uint32> ReporterId;
    Atomic<uint32> State;
    Atomic<uint32> DebuggerAttached;
    /// The thread that calls CrashReport::Heartbeat: the main thread.
    Atomic<uint32> HeartbeatThread;
    uint32 Padding;
    /// The frame and PlatformTime::Ticks of the latest heartbeat.
    Atomic<uint64> HeartbeatFrame;
    Atomic<uint64> HeartbeatTicks;
    CrashSharedContext Context;
    CrashSharedRecord Record;
    CrashSharedAttachments Attachments;
    CrashSharedLog Log;
};

// Between MarlinCrashReporter and MarlinCrashServer: one TCP connection per report.

inline constexpr uint32 CrashUploadMagic = 0x5255434Du; // "MCUR"
inline constexpr uint32 CrashUploadReplyMagic =
    0x5253434Du; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).
inline constexpr uint32 CrashUploadVersion =
    1; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).
inline constexpr uint16 CrashServerDefaultPort =
    7492; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).
/// Bytes after the header.
inline constexpr uint64 CrashUploadMaxSize =
    uint64{1} << 30; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).
inline constexpr uint32 CrashUploadMaxFiles =
    64; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).
inline constexpr uint32 CrashUploadMaxNameLength =
    128; // RECOVERY-DECISION D2: upload protocol constant chosen (value not recovered).

enum class CrashUploadStatus : uint8
{
    Ok,
    /// The upload was malformed; sending it again does not help.
    Rejected,
    Error,
};

/// Followed by FileCount files, each a CrashUploadFile, its name and its bytes.
struct CrashUploadHeader
{
    uint32 Magic = CrashUploadMagic;
    uint32 Version = CrashUploadVersion;
    uint32 FileCount = 0;
    uint32 Reserved = 0;
    /// Bytes after the header.
    uint64 Size = 0;
};

struct CrashUploadFile
{
    uint32 NameLength = 0;
    uint32 Reserved = 0;
    uint64 Size = 0;
};

struct CrashUploadReply
{
    uint32 Magic = CrashUploadReplyMagic;
    CrashUploadStatus Status = CrashUploadStatus::Error;
    uint8 Padding[3] = {};
    /// The server's number for the report.
    uint64 Report = 0;
    /// The bucket the report went into.
    Hash128 Signature;
};

/// The files of a report folder. A local folder is `<ReportDirectory>/<time>`; the server keeps each report in
/// `Reports/<number>`.
namespace CrashReportFiles
{
inline constexpr const char *Report = "Report.mtext";
inline constexpr const char *Minidump = "Minidump.dmp";
inline constexpr const char *Log = "Log.txt";
/// The stacks as the reporter could symbolize them on the machine that crashed.
inline constexpr const char *Stack = "Stack.txt"; // RECOVERY-DECISION D3: report file name chosen (not recovered).
/// The server's analysis with symbols from its symbol store.
inline constexpr const char *Analysis = "Analysis.txt";
/// Marks a folder the server accepted.
inline constexpr const char *Uploaded = "Uploaded"; // RECOVERY-DECISION D3: report file name chosen (not recovered).
/// Marks a folder the user chose not to send.
inline constexpr const char *Declined = "Declined"; // RECOVERY-DECISION D3: report file name chosen (not recovered).
/// Marks a folder that may be sent: the user agreed, or AskUser was off. Only these are retried, so a report whose
/// question was never answered (the reporter was killed, the machine shut down) is never sent.
inline constexpr const char *Consented = "Consented";
} // namespace CrashReportFiles
} // namespace Marlin
