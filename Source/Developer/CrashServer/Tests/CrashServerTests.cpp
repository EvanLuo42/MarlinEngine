#include "Testing/Testing.h"

#include "Concurrency/ScopeLock.h"
#include "CrashReport/CrashProtocol.h"
#include "CrashReport/CrashReportInfo.h"
#include "CrashServer/CrashServer.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
constexpr uint32 ReplyTimeoutSeconds = 60;

String ExecutableDirectory()
{
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    StringView path(buffer, length < sizeof(buffer) ? length : 0);
    usize slash = path.Length();
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\')
    {
        --slash;
    }
    return String(path.Left(slash > 0 ? slash - 1 : 0));
}

// RECOVERY-DECISION D19: test folders live under %TEMP% (recovery safety rule), not beside the executable.
/// A fresh `%TEMP%/MarlinTests/CrashServerTests/<name>`; only that folder is ever deleted.
String TestDirectory(StringView name)
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(KnownFolder::Temp, buffer, sizeof(buffer));
    MARLIN_ASSERT(length > 3 && length < sizeof(buffer) && !name.IsEmpty());
    const String base = Format("{0}/MarlinTests", StringView(buffer, length));
    const String directory = Format("{0}/CrashServerTests/{1}", base, name);
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    (void)PlatformFile::CreateDirectory(directory.CStr());
    return directory;
}

struct UploadFile
{
    String Name;
    List<uint8> Bytes;
};

List<uint8> BytesOf(StringView text)
{
    return List<uint8>(Span<const uint8>(reinterpret_cast<const uint8 *>(text.Data()), text.Length()));
}

template <typename T> void Append(List<uint8> &bytes, const T &value)
{
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&value), sizeof(T)));
}

List<uint8> Payload(Span<const UploadFile> files, uint32 magic = CrashUploadMagic)
{
    List<uint8> body;
    for (const UploadFile &file : files)
    {
        CrashUploadFile header;
        header.NameLength = static_cast<uint32>(file.Name.Length());
        header.Size = file.Bytes.Count();
        Append(body, header);
        body.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(file.Name.Data()), file.Name.Length()));
        body.AddRange(file.Bytes.AsSpan());
    }
    CrashUploadHeader header;
    header.Magic = magic;
    header.FileCount = static_cast<uint32>(files.Count());
    header.Size = body.Count();
    List<uint8> payload;
    Append(payload, header);
    payload.AddRange(body.AsSpan());
    return payload;
}

struct ReplyState : RefCounted
{
    Mutex Lock;
    CrashUploadReply Reply;
    bool Answered = false;
    bool Closed = false;
};

/// Uploads *payload* and ticks *server* until it answers; false without an answer. *closeEarly* hangs up right after
/// sending.
bool Send(NetLoop &loop, CrashServer &server, Span<const uint8> payload, CrashUploadReply &reply,
          bool closeEarly = false)
{
    const SharedPtr<NetStream> stream = NetStream::ConnectTcp(loop, "127.0.0.1", server.GetPort(), 5000);
    if (stream == nullptr)
    {
        return false;
    }
    SharedPtr<ReplyState> state(new ReplyState());
    NetStreamHandlers handlers;
    handlers.OnReceive = [state](NetStream & /*self*/, Span<const uint8> bytes) -> usize {
        if (bytes.Count() < sizeof(CrashUploadReply))
        {
            return 0;
        }
        const ScopeLock lock(state->Lock);
        MemCopy(&state->Reply, bytes.Data(), sizeof(CrashUploadReply));
        state->Answered = true;
        return bytes.Count();
    };
    handlers.OnClose = [state](NetStream & /*self*/) {
        const ScopeLock lock(state->Lock);
        state->Closed = true;
    };
    stream->Start(Move(handlers));
    stream->Send(payload);
    if (closeEarly)
    {
        stream->Close();
    }
    const uint64 deadline = PlatformTime::Ticks() + PlatformTime::TicksPerSecond() * ReplyTimeoutSeconds;
    while (PlatformTime::Ticks() < deadline)
    {
        server.Tick();
        {
            const ScopeLock lock(state->Lock);
            if (state->Closed)
            {
                break;
            }
        }
        PlatformThread::Sleep(1);
    }
    stream->Abort();
    const ScopeLock lock(state->Lock);
    reply = state->Reply;
    return state->Answered;
}

String ReportText(CrashKind kind, StringView message)
{
    CrashReportInfo info;
    info.Kind = kind;
    info.FailureMessage = String(message);
    info.TargetName = "Game";
    info.EngineVersion = "0.1";
    info.ProductVersion = "1.2.0";
    return CrashReport::WriteInfo(info);
}

bool Exists(StringView path)
{
    const String file(path);
    return PlatformFile::FileExists(file.CStr()) || PlatformFile::DirectoryExists(file.CStr());
}

CrashServerDesc Desc(const String &directory)
{
    CrashServerDesc desc;
    desc.DataDirectory = Format("{0}/Data", directory);
    return desc;
}
} // namespace

MARLIN_TEST(CrashServer, BucketsReportsBySignature)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("Buckets");
    const String dump = Format("{0}/Self.dmp", directory);
    MARLIN_REQUIRE(PlatformCrash::WriteMinidump(dump.CStr(), PlatformProcess::CurrentId(), nullptr));
    UploadFile minidump{.Name = String(CrashReportFiles::Minidump), .Bytes = {}};
    const FileHandle file = PlatformFile::Open(dump.CStr(), FileAccess::Read, FileMode::OpenExisting);
    uint64 size = 0;
    usize read = 0;
    MARLIN_REQUIRE(file.IsValid() && PlatformFile::GetSize(file, size));
    minidump.Bytes.ResizeUninitialized(static_cast<usize>(size));
    MARLIN_REQUIRE(PlatformFile::Read(file, minidump.Bytes.Data(), minidump.Bytes.Count(), read));
    PlatformFile::Close(file);

    const UploadFile log{.Name = String(CrashReportFiles::Log), .Bytes = BytesOf("the last lines\n")};
    const UploadFile withDump[] = {
        {.Name = String(CrashReportFiles::Report), .Bytes = BytesOf(ReportText(CrashKind::Exception, ""))},
        minidump,
        log,
    };
    const UploadFile noDump[] = {
        {.Name = String(CrashReportFiles::Report), .Bytes = BytesOf(ReportText(CrashKind::Hang, "stuck"))},
        log,
    };
    CrashUploadReply first;
    {
        CrashServer server(loop, Desc(directory));
        MARLIN_REQUIRE(server.Start() && server.GetPort() != 0);
        CrashUploadReply second;
        CrashUploadReply third;
        MARLIN_REQUIRE(Send(loop, server, Payload(withDump), first));
        MARLIN_REQUIRE(Send(loop, server, Payload(withDump), second));
        MARLIN_REQUIRE(Send(loop, server, Payload(noDump), third));
        MARLIN_EXPECT(first.Status == CrashUploadStatus::Ok && second.Status == CrashUploadStatus::Ok);
        MARLIN_EXPECT(first.Report == 1 && second.Report == 2);
        MARLIN_EXPECT(first.Signature == second.Signature);
        MARLIN_EXPECT(third.Status == CrashUploadStatus::Ok && third.Signature != first.Signature);
        MARLIN_EXPECT(server.GetStats().Reports == 3 && server.GetStats().Buckets == 2);
        MARLIN_EXPECT(Exists(Format("{0}/Data/Reports/00000001/{1}", directory, CrashReportFiles::Analysis)));
        MARLIN_EXPECT(Exists(Format("{0}/Data/Reports/00000002/Log.txt", directory)));
        server.Stop();
    }

    CrashServer restarted(loop, Desc(directory));
    MARLIN_REQUIRE(restarted.Start());
    MARLIN_EXPECT(restarted.GetStats().Reports == 3 && restarted.GetStats().Buckets == 2);
    CrashUploadReply reply;
    MARLIN_REQUIRE(Send(loop, restarted, Payload(withDump), reply));
    MARLIN_EXPECT(reply.Report == 4 && reply.Signature == first.Signature);
    restarted.Stop();
    loop.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(CrashServer, RejectsMalformedUploads)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("Rejects");
    const String data = Format("{0}/Data", directory);
    CrashServer server(loop, Desc(directory));
    MARLIN_REQUIRE(server.Start());

    const UploadFile report[] = {
        {.Name = String(CrashReportFiles::Report), .Bytes = BytesOf(ReportText(CrashKind::Exception, ""))},
    };
    const UploadFile withoutReport[] = {{.Name = String(CrashReportFiles::Log), .Bytes = BytesOf("log\n")}};
    const UploadFile escaping[] = {report[0], {.Name = String("../Escaped.txt"), .Bytes = BytesOf("out")}};
    CrashUploadReply reply;
    MARLIN_EXPECT(Send(loop, server, Payload(report, 0x12345678u), reply));
    MARLIN_EXPECT(reply.Status == CrashUploadStatus::Rejected);
    MARLIN_EXPECT(Send(loop, server, Payload(withoutReport), reply));
    MARLIN_EXPECT(reply.Status == CrashUploadStatus::Rejected);
    MARLIN_EXPECT(Send(loop, server, Payload(escaping), reply));
    MARLIN_EXPECT(reply.Status == CrashUploadStatus::Rejected);
    const uint64 number = reply.Report;
    MARLIN_EXPECT(!Exists(Format("{0}/Reports/Escaped.txt", data)));
    MARLIN_EXPECT(!Exists(Format("{0}/Reports/{1:D8}", data, number)));

    List<uint8> truncated = Payload(report);
    truncated.Resize(truncated.Count() - 4);
    MARLIN_EXPECT(!Send(loop, server, truncated, reply, true));
    MARLIN_EXPECT(server.GetStats().Reports == 0 && server.GetStats().Rejected == 3);
    server.Stop();
    loop.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(CrashServer, GameCrashEndToEnd, Slow)
{
    const StringView suffix = MARLIN_PLATFORM_WINDOWS ? ".exe" : "";
    const String binaries = ExecutableDirectory();
    const String game = Format("{0}/Game{1}", binaries, suffix);
    if (!PlatformFile::FileExists(game.CStr()) ||
        !PlatformFile::FileExists(Format("{0}/MarlinCrashReporter{1}", binaries, suffix).CStr()))
    {
        MARLIN_LOG(LogCore, Warning, "Game and MarlinCrashReporter are not built; nothing to crash");
        return;
    }
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("EndToEnd");
    CrashServer server(loop, Desc(directory));
    MARLIN_REQUIRE(server.Start());

    const String arguments =
        Format(R"(-ExecCommands="Debug.Crash Fatal" -Config:Engine.CrashReport.Server.Host=\"127.0.0.1\")"
               R"( -Config:Engine.CrashReport.Server.Port={0})",
               server.GetPort());
    ProcessDesc desc;
    desc.Executable = game.CStr();
    desc.Arguments = arguments.CStr();
    desc.WorkingDirectory = binaries.CStr();
    desc.Flags = ProcessFlags::Hidden;
    ProcessHandle process;
    MARLIN_REQUIRE(PlatformProcess::Create(desc, process));
    const uint64 deadline = PlatformTime::Ticks() + PlatformTime::TicksPerSecond() * ReplyTimeoutSeconds * 2;
    while (server.GetStats().Reports == 0 && PlatformTime::Ticks() < deadline)
    {
        server.Tick();
        PlatformThread::Sleep(10);
    }
    if (PlatformProcess::IsRunning(process))
    {
        PlatformProcess::Terminate(process, 1);
    }
    PlatformProcess::Close(process);
    MARLIN_REQUIRE(server.GetStats().Reports == 1);

    String report;
    {
        const String path = Format("{0}/Data/Reports/00000001/{1}", directory, CrashReportFiles::Report);
        const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
        uint64 size = 0;
        usize read = 0;
        MARLIN_REQUIRE(file.IsValid() && PlatformFile::GetSize(file, size));
        MARLIN_REQUIRE(PlatformFile::Read(file, report.ResizeUninitialized(static_cast<usize>(size)), size, read));
        PlatformFile::Close(file);
    }
    CrashReportInfo info;
    String errors;
    MARLIN_EXPECT(CrashReport::ReadInfo(report, info, errors) && info.Kind == CrashKind::Failure &&
                  info.FailureMessage == "Debug.Crash Fatal");
    MARLIN_EXPECT(Exists(Format("{0}/Data/Reports/00000001/{1}", directory, CrashReportFiles::Minidump)));
    server.Stop();
    loop.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}
} // namespace Marlin::Tests
