#include "CrashServer/CrashServer.h"

#include "Concurrency/ScopeLock.h"
#include "CrashAnalysis/CrashAnalysis.h"
#include "CrashReport/CrashReportInfo.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/MemoryOps.h"
#include "Misc/DateTime.h"
#include "Reflection/TypeOf.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr uint64 PruneIntervalSeconds = 3600; // RECOVERY-DECISION D7: crash server setting chosen (not recovered).

String ReportDirectory(StringView data, uint64 number)
{
    return Format("{0}/Reports/{1:D8}", data, number);
}

String BucketPath(StringView data, const Hash128 &signature)
{
    return Format("{0}/Buckets/{1}.mtext", data, CrashAnalyzer::FormatSignature(signature));
}

bool ReadText(StringView path, String &text)
{
    const String file(path);
    const FileHandle handle = PlatformFile::Open(file.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!handle.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    const bool sized = PlatformFile::GetSize(handle, size) && size < (uint64{1} << 31);
    const bool succeeded = sized &&
                           PlatformFile::Read(handle, text.ResizeUninitialized(static_cast<usize>(size)),
                                              static_cast<usize>(size), read) &&
                           read == size;
    PlatformFile::Close(handle);
    return succeeded;
}

bool WriteText(StringView path, StringView text)
{
    const String file(path);
    const FileHandle handle = PlatformFile::Open(file.CStr(), FileAccess::Write, FileMode::CreateAlways);
    const bool succeeded = handle.IsValid() && PlatformFile::Write(handle, text.Data(), text.Length());
    PlatformFile::Close(handle);
    return succeeded;
}

String Now()
{
    StringBuilder<32> text;
    DateTime::UtcNow().FormatTo(text, "s");
    return text.ToString();
}

// RECOVERY-DECISION D20: upload file names must be plain names without path or reserved characters.
/// A plain file name: no folders, nothing hidden, nothing a file system treats specially.
bool IsValidName(StringView name)
{
    if (name.IsEmpty() || name.Length() > CrashUploadMaxNameLength || name.StartsWith('.') || name.EndsWith('.') ||
        name.EndsWith(' '))
    {
        return false;
    }
    for (const char c : name)
    {
        if (static_cast<uint8>(c) < 0x20 || StringView("/\\:*?\"<>|").Contains(c))
        {
            return false;
        }
    }
    return true;
}
} // namespace

struct CrashServer::Connection : RefCounted
{
    enum class Phase : uint8
    {
        Header,
        FileHeader,
        FileName,
        FileData,
        Done,
        Failed,
    };

    SharedPtr<NetStream> Stream;
    Phase Step = Phase::Header;
    CrashUploadHeader Header;
    uint64 Remaining = 0;
    uint32 FilesLeft = 0;
    CrashUploadFile File;
    uint64 DataLeft = 0;
    FileHandle Output;
    uint64 Number = 0;
    String Directory;

    ~Connection() override
    {
        if (Output.IsValid())
        {
            PlatformFile::Close(Output);
        }
    }
};

CrashServer::CrashServer(NetLoop &loop, CrashServerDesc desc) : _loop(loop), _desc(Move(desc))
{
}

CrashServer::~CrashServer()
{
    Stop();
}

bool CrashServer::Start()
{
    const String reports = Format("{0}/Reports", _desc.DataDirectory);
    const String buckets = Format("{0}/Buckets", _desc.DataDirectory);
    if (_desc.DataDirectory.IsEmpty() || !PlatformFile::CreateDirectory(reports.CStr()) ||
        !PlatformFile::CreateDirectory(buckets.CStr()))
    {
        MARLIN_LOG(LogCore, Error, "MarlinCrashServer cannot create {0}", _desc.DataDirectory);
        return false;
    }
    const String lockPath = Format("{0}/CrashServer.lock", _desc.DataDirectory);
    _lockFile = PlatformFile::Open(lockPath.CStr(), FileAccess::Write, FileMode::CreateAlways,
                                   FileFlags::Exclusive | FileFlags::DeleteOnClose);
    if (!_lockFile.IsValid())
    {
        MARLIN_LOG(LogCore, Error, "Another MarlinCrashServer uses {0}", _desc.DataDirectory);
        return false;
    }
    if (!LoadBuckets())
    {
        return false;
    }
    _listener = NetListener::ListenTcp(_loop, _desc.Port, _desc.AllowRemote,
                                       [this](SharedPtr<NetStream> stream) { Accept(Move(stream)); });
    if (_listener == nullptr)
    {
        MARLIN_LOG(LogCore, Error, "MarlinCrashServer cannot listen on port {0}", _desc.Port);
        return false;
    }
    return true;
}

bool CrashServer::LoadBuckets()
{
    _buckets.Clear();
    _reportBuckets.Clear();
    DirectoryIterator iterator;
    DirectoryEntry entry;
    const String buckets = Format("{0}/Buckets", _desc.DataDirectory);
    if (iterator.Open(buckets.CStr()))
    {
        while (iterator.Next(entry))
        {
            const StringView name(entry.Name);
            StringView stem;
            StringView extension;
            if (entry.Info.IsDirectory || !name.SplitLast('.', stem, extension) || extension != "mtext")
            {
                continue;
            }
            Hash128 signature;
            String text;
            MarlinTextDocument document;
            MarlinTextDiagnostics diagnostics;
            CrashBucket bucket;
            const MarlinTextObject *object = nullptr;
            const bool valid = stem.Length() == 32 && TryParse(stem.Left(16), signature.High, 16) &&
                               TryParse(stem.Right(16), signature.Low, 16) &&
                               ReadText(Format("{0}/{1}", buckets, name), text) && document.Parse(text, diagnostics) &&
                               document.GetObjectCount() == 1 &&
                               (object = document.GetObject(0, diagnostics)) != nullptr &&
                               MarlinTextSerializer::ReadObject(*object, TypeOf<CrashBucket>(), &bucket, diagnostics);
            if (!valid)
            {
                MARLIN_LOG(LogCore, Warning, "{0}/{1} is not a crash bucket", buckets, name);
                continue;
            }
            for (const uint64 report : bucket.Reports)
            {
                _reportBuckets.Set(report, signature);
            }
            _buckets.Set(signature, Move(bucket));
        }
        iterator.Close();
    }

    // Report folders are numbered; one that is in no bucket arrived but was never analyzed.
    uint64 last = 0;
    uint64 reports = 0;
    _unprocessed.Clear();
    const String root = Format("{0}/Reports", _desc.DataDirectory);
    if (iterator.Open(root.CStr()))
    {
        while (iterator.Next(entry))
        {
            uint64 number = 0;
            if (!entry.Info.IsDirectory || !TryParse(StringView(entry.Name), number) || number == 0)
            {
                continue;
            }
            ++reports;
            last = number > last ? number : last;
            if (!_reportBuckets.Contains(number))
            {
                _unprocessed.Add(number);
            }
        }
        iterator.Close();
    }
    _nextReport.Store(last + 1);
    _reports.Store(reports);
    return true;
}

void CrashServer::Stop()
{
    if (_listener != nullptr)
    {
        _listener->Close();
        _listener.Reset();
    }
    List<SharedPtr<Connection>> connections;
    {
        const ScopeLock lock(_connectionsLock);
        connections = _connections;
    }
    for (SharedPtr<Connection> &connection : connections)
    {
        connection->Stream->Abort();
    }
    connections.Clear();
    while (true)
    {
        {
            const ScopeLock lock(_connectionsLock);
            if (_connections.IsEmpty())
            {
                break;
            }
        }
        PlatformThread::Sleep(1);
    }
    {
        const ScopeLock lock(_queueLock);
        _finished.Clear();
    }
    _buckets.Clear();
    _reportBuckets.Clear();
    if (_lockFile.IsValid())
    {
        PlatformFile::Close(_lockFile);
        _lockFile = {};
    }
}

CrashServerStats CrashServer::GetStats() const
{
    return {.Reports = _reports.Load(), .Buckets = _buckets.Count(), .Rejected = _rejected.Load()};
}

void CrashServer::Accept(SharedPtr<NetStream> stream)
{
    SharedPtr<Connection> connection(new Connection());
    connection->Stream = stream;
    {
        const ScopeLock lock(_connectionsLock);
        _connections.Add(connection);
    }
    NetStreamHandlers handlers;
    handlers.OnReceive = [this, connection](NetStream & /*self*/, Span<const uint8> bytes) {
        return Receive(connection, bytes);
    };
    handlers.OnClose = [this, connection](NetStream & /*self*/) { Closed(*connection); };
    stream->Start(Move(handlers));
}

void CrashServer::Closed(Connection &connection)
{
    if (connection.Step != Connection::Phase::Done && connection.Step != Connection::Phase::Failed &&
        !connection.Directory.IsEmpty())
    {
        // The upload broke off: drop what arrived.
        if (connection.Output.IsValid())
        {
            PlatformFile::Close(connection.Output);
            connection.Output = {};
        }
        (void)PlatformFile::DeleteDirectory(connection.Directory.CStr(), true);
    }
    const ScopeLock lock(_connectionsLock);
    for (usize i = 0; i < _connections.Count(); ++i)
    {
        if (_connections[i].Get() == &connection)
        {
            _connections.RemoveAtSwap(i);
            break;
        }
    }
}

void CrashServer::Reject(Connection &connection)
{
    connection.Step = Connection::Phase::Failed;
    _rejected.FetchAdd(1);
    if (connection.Output.IsValid())
    {
        PlatformFile::Close(connection.Output);
        connection.Output = {};
    }
    if (!connection.Directory.IsEmpty())
    {
        (void)PlatformFile::DeleteDirectory(connection.Directory.CStr(), true);
    }
    CrashUploadReply reply;
    reply.Status = CrashUploadStatus::Rejected;
    reply.Report = connection.Number;
    connection.Stream->Send(Span<const uint8>(reinterpret_cast<const uint8 *>(&reply), sizeof(reply)));
    connection.Stream->Close();
}

usize CrashServer::Receive(const SharedPtr<Connection> &connection, Span<const uint8> bytes)
{
    Connection &upload = *connection;
    usize used = 0;
    while (upload.Step != Connection::Phase::Done && upload.Step != Connection::Phase::Failed)
    {
        const Span<const uint8> rest = bytes.Slice(used);
        switch (upload.Step)
        {
        case Connection::Phase::Header:
            if (rest.Count() < sizeof(CrashUploadHeader))
            {
                return used;
            }
            MemCopy(&upload.Header, rest.Data(), sizeof(CrashUploadHeader));
            used += sizeof(CrashUploadHeader);
            if (upload.Header.Magic != CrashUploadMagic || upload.Header.Version != CrashUploadVersion ||
                upload.Header.FileCount == 0 || upload.Header.FileCount > CrashUploadMaxFiles ||
                upload.Header.Size > CrashUploadMaxSize)
            {
                Reject(upload);
                return bytes.Count();
            }
            upload.Remaining = upload.Header.Size;
            upload.FilesLeft = upload.Header.FileCount;
            upload.Number = _nextReport.FetchAdd(1);
            upload.Directory = ReportDirectory(_desc.DataDirectory, upload.Number);
            if (!PlatformFile::CreateDirectory(upload.Directory.CStr()))
            {
                Reject(upload);
                return bytes.Count();
            }
            upload.Step = Connection::Phase::FileHeader;
            break;
        case Connection::Phase::FileHeader: {
            if (rest.Count() < sizeof(CrashUploadFile))
            {
                return used;
            }
            MemCopy(&upload.File, rest.Data(), sizeof(CrashUploadFile));
            used += sizeof(CrashUploadFile);
            const uint64 overhead = sizeof(CrashUploadFile) + upload.File.NameLength;
            if (upload.File.NameLength == 0 || upload.File.NameLength > CrashUploadMaxNameLength ||
                upload.Remaining < overhead || upload.File.Size > upload.Remaining - overhead)
            {
                Reject(upload);
                return bytes.Count();
            }
            upload.Remaining -= sizeof(CrashUploadFile);
            upload.Step = Connection::Phase::FileName;
            break;
        }
        case Connection::Phase::FileName: {
            if (rest.Count() < upload.File.NameLength)
            {
                return used;
            }
            const StringView name(reinterpret_cast<const char *>(rest.Data()), upload.File.NameLength);
            used += upload.File.NameLength;
            upload.Remaining -= upload.File.NameLength;
            const String path = Format("{0}/{1}", upload.Directory, name);
            if (IsValidName(name))
            {
                upload.Output = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateNew);
            }
            if (!upload.Output.IsValid())
            {
                Reject(upload);
                return bytes.Count();
            }
            upload.DataLeft = upload.File.Size;
            upload.Step = Connection::Phase::FileData;
            break;
        }
        case Connection::Phase::FileData: {
            const usize count = rest.Count() < upload.DataLeft ? rest.Count() : static_cast<usize>(upload.DataLeft);
            if (count > 0 && !PlatformFile::Write(upload.Output, rest.Data(), count))
            {
                Reject(upload);
                return bytes.Count();
            }
            used += count;
            upload.DataLeft -= count;
            upload.Remaining -= count;
            if (upload.DataLeft != 0)
            {
                return used;
            }
            PlatformFile::Close(upload.Output);
            upload.Output = {};
            if (--upload.FilesLeft != 0)
            {
                upload.Step = Connection::Phase::FileHeader;
                break;
            }
            if (upload.Remaining != 0 ||
                !PlatformFile::FileExists(Format("{0}/{1}", upload.Directory, CrashReportFiles::Report).CStr()))
            {
                Reject(upload);
                return bytes.Count();
            }
            upload.Step = Connection::Phase::Done;
            const ScopeLock lock(_queueLock);
            _finished.Add(connection);
            break;
        }
        default:
            break;
        }
    }
    // Anything after a complete upload is not part of the protocol.
    return bytes.Count();
}

void CrashServer::Tick()
{
    List<SharedPtr<Connection>> finished;
    List<uint64> unprocessed;
    {
        const ScopeLock lock(_queueLock);
        finished = Move(_finished);
        _finished.Clear();
        unprocessed = Move(_unprocessed);
        _unprocessed.Clear();
    }
    for (const uint64 number : unprocessed)
    {
        (void)Process(number);
    }
    for (SharedPtr<Connection> &connection : finished)
    {
        _reports.FetchAdd(1);
        CrashUploadReply reply;
        reply.Status = CrashUploadStatus::Ok;
        reply.Report = connection->Number;
        reply.Signature = Process(connection->Number);
        connection->Stream->Send(Span<const uint8>(reinterpret_cast<const uint8 *>(&reply), sizeof(reply)));
        connection->Stream->Close();
    }
    const uint64 now = PlatformTime::Ticks() / PlatformTime::TicksPerSecond();
    if (_desc.RetentionDays != 0 && now >= _nextPrune)
    {
        _nextPrune = now + PruneIntervalSeconds;
        Prune();
    }
}

void CrashServer::SaveBucket(const Hash128 &signature, const CrashBucket &bucket) const
{
    StringBuilder<4096> text;
    MarlinTextSerializer::WriteObject(text, TypeOf<CrashBucket>(), &bucket);
    text.Append('\n');
    (void)WriteText(BucketPath(_desc.DataDirectory, signature), text.AsView());
}

void CrashServer::Prune()
{
    const int64 cutoff = PlatformTime::UtcNow() - int64{_desc.RetentionDays} * TimeSpan::TicksPerDay;
    List<uint64> expired;
    DirectoryIterator iterator;
    DirectoryEntry entry;
    const String root = Format("{0}/Reports", _desc.DataDirectory);
    if (iterator.Open(root.CStr()))
    {
        while (iterator.Next(entry))
        {
            uint64 number = 0;
            if (entry.Info.IsDirectory && TryParse(StringView(entry.Name), number) && number != 0 &&
                entry.Info.ModifiedTime < cutoff)
            {
                expired.Add(number);
            }
        }
        iterator.Close();
    }
    if (expired.IsEmpty())
    {
        return;
    }
    List<Hash128> changed;
    for (const uint64 number : expired)
    {
        (void)PlatformFile::DeleteDirectory(ReportDirectory(_desc.DataDirectory, number).CStr(), true);
        _reports.FetchAdd(~uint64{0});
        const Hash128 *signature = _reportBuckets.Find(number);
        if (signature == nullptr)
        {
            continue;
        }
        if (CrashBucket *bucket = _buckets.Find(*signature))
        {
            bucket->Reports.Remove(number);
            if (!changed.Contains(*signature))
            {
                changed.Add(*signature);
            }
        }
        _reportBuckets.Remove(number);
    }
    for (const Hash128 &signature : changed)
    {
        SaveBucket(signature, *_buckets.Find(signature));
    }
    MARLIN_LOG(LogCore, Info, "Deleted {0} reports older than {1} days", expired.Count(), _desc.RetentionDays);
}

Hash128 CrashServer::Process(uint64 number)
{
    const String directory = ReportDirectory(_desc.DataDirectory, number);
    CrashReportInfo info;
    String text;
    String errors;
    if (!ReadText(Format("{0}/{1}", directory, CrashReportFiles::Report), text) ||
        !CrashReport::ReadInfo(text, info, errors))
    {
        MARLIN_LOG(LogCore, Warning, "Report {0} has no readable {1}: {2}", number, CrashReportFiles::Report, errors);
    }

    // With a dump the signature is the focus thread's top frames; without one, what the report says.
    Hash128 signature;
    String frames;
    const String dump = Format("{0}/{1}", directory, CrashReportFiles::Minidump);
    if (PlatformFile::FileExists(dump.CStr()))
    {
        CrashAnalysisDesc desc;
        desc.Minidump = dump;
        desc.SymbolStore = _desc.SymbolStore;
        CrashAnalysisResult result;
        String error;
        if (CrashAnalyzer::Analyze(desc, result, error))
        {
            StringBuilder<16384> analysis;
            CrashAnalyzer::WriteText(analysis, result);
            (void)WriteText(Format("{0}/{1}", directory, CrashReportFiles::Analysis), analysis.AsView());
            signature = result.Signature;
            frames = Move(result.SignatureText);
        }
        else
        {
            MARLIN_LOG(LogCore, Warning, "Report {0} has no readable {1}: {2}", number, CrashReportFiles::Minidump,
                       error);
        }
    }
    if (frames.IsEmpty())
    {
        // RECOVERY-DECISION D8: reports without a usable dump are bucketed by kind, target, exception code and message.
        frames = Format("{0} {1} 0x{2:X8} {3}", CrashReport::GetKindName(info.Kind), info.TargetName,
                        info.ExceptionCode, info.FailureMessage);
        signature = Xxh3Hash128(frames.Data(), frames.Length());
    }

    const bool added = !_buckets.Contains(signature);
    CrashBucket &bucket = _buckets.FindOrAdd(signature);
    const String now = Now();
    if (added)
    {
        bucket.Signature = CrashAnalyzer::FormatSignature(signature);
        bucket.Frames = frames;
        bucket.Kind = String(CrashReport::GetKindName(info.Kind));
        bucket.Summary =
            !info.FailureMessage.IsEmpty() ? info.FailureMessage : Format("Exception 0x{0:X8}", info.ExceptionCode);
        bucket.FirstSeen = now;
    }
    ++bucket.Count;
    bucket.LastSeen = now;
    const String version = Format("{0} / {1}", info.EngineVersion, info.ProductVersion);
    if (!bucket.Versions.Contains(version))
    {
        bucket.Versions.Add(version);
    }
    bucket.Reports.Add(number);
    _reportBuckets.Set(number, signature);
    while (bucket.Reports.Count() > CrashBucket::MaxReports)
    {
        const uint64 oldest = bucket.Reports[0];
        (void)PlatformFile::DeleteDirectory(ReportDirectory(_desc.DataDirectory, oldest).CStr(), true);
        _reportBuckets.Remove(oldest);
        _reports.FetchAdd(~uint64{0});
        bucket.Reports.RemoveAt(0);
    }
    SaveBucket(signature, bucket);
    MARLIN_LOG(LogCore, Info, "Report {0} ({1} {2}) is in bucket {3}, seen {4} times", number, info.TargetName,
               bucket.Kind, bucket.Signature, bucket.Count);
    return signature;
}
} // namespace Marlin
