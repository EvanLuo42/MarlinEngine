#include "CrashUploader.h"

#include "Containers/List.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSync.h"
#include "Memory/MemoryOps.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/Format.h"
#include "String/String.h"

namespace Marlin
{
namespace
{
constexpr uint32 ConnectTimeout = 10000; // RECOVERY-DECISION D6: reporter timing chosen (not recovered).
constexpr uint32 ReplyTimeout = 120000;  // RECOVERY-DECISION D6: reporter timing chosen (not recovered).

template <typename T> void AppendBytes(List<uint8> &bytes, const T &value)
{
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&value), sizeof(T)));
}

bool AppendFile(List<uint8> &payload, StringView directory, StringView name)
{
    const String path = Format("{0}/{1}", directory, name);
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    uint64 size = 0;
    if (!file.IsValid() || !PlatformFile::GetSize(file, size) || size > CrashUploadMaxSize)
    {
        PlatformFile::Close(file);
        return false;
    }
    CrashUploadFile header;
    header.NameLength = static_cast<uint32>(name.Length());
    header.Size = size;
    AppendBytes(payload, header);
    payload.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(name.Data()), name.Length()));
    const usize start = payload.Count();
    payload.ResizeUninitialized(start + static_cast<usize>(size));
    usize read = 0;
    const bool succeeded =
        PlatformFile::Read(file, payload.Data() + start, static_cast<usize>(size), read) && read == size;
    PlatformFile::Close(file);
    return succeeded;
}

/// What the IO thread hands back to the waiting uploader.
struct UploadState : RefCounted
{
    Event Answered{EventReset::Manual};
    Event Closed{EventReset::Manual};
    Mutex Lock;
    CrashUploadReply Reply;
    bool HasReply = false;
};

bool IsMarker(StringView name)
{
    return name == CrashReportFiles::Uploaded || name == CrashReportFiles::Declined;
}
} // namespace

bool UploadReport(StringView directory, StringView host, uint16 port, CrashUploadReply &reply)
{
    List<uint8> payload;
    AppendBytes(payload, CrashUploadHeader());
    uint32 fileCount = 0;
    DirectoryIterator iterator;
    DirectoryEntry entry;
    const String folder(directory);
    if (!iterator.Open(folder.CStr()))
    {
        return false;
    }
    while (iterator.Next(entry))
    {
        const StringView name(entry.Name);
        if (entry.Info.IsDirectory || IsMarker(name) || name.Length() > CrashUploadMaxNameLength)
        {
            continue;
        }
        if (fileCount == CrashUploadMaxFiles || !AppendFile(payload, directory, name))
        {
            MARLIN_LOG(LogCore, Warning, "Cannot upload {0}/{1}", directory, name);
            continue;
        }
        ++fileCount;
    }
    iterator.Close();
    CrashUploadHeader header;
    header.FileCount = fileCount;
    header.Size = payload.Count() - sizeof(CrashUploadHeader);
    MemCopy(payload.Data(), &header, sizeof(header));

    NetLoop loop;
    if (!loop.Start(1))
    {
        return false;
    }
    bool answered = false;
    SharedPtr<NetStream> stream = NetStream::ConnectTcp(loop, host, port, ConnectTimeout);
    if (stream != nullptr)
    {
        SharedPtr<UploadState> state(new UploadState());
        NetStreamHandlers handlers;
        handlers.OnReceive = [state](NetStream & /*self*/, Span<const uint8> bytes) -> usize {
            if (bytes.Count() < sizeof(CrashUploadReply))
            {
                return 0;
            }
            {
                const ScopeLock lock(state->Lock);
                MemCopy(&state->Reply, bytes.Data(), sizeof(CrashUploadReply));
                state->HasReply = state->Reply.Magic == CrashUploadReplyMagic;
            }
            state->Answered.Set();
            return bytes.Count();
        };
        handlers.OnClose = [state](NetStream & /*self*/) {
            state->Answered.Set();
            state->Closed.Set();
        };
        stream->Start(Move(handlers));
        stream->Send(Move(payload));
        (void)state->Answered.Wait(ReplyTimeout);
        stream->Close();
        (void)state->Closed.Wait(ReplyTimeout);
        stream->Abort();
        const ScopeLock lock(state->Lock);
        answered = state->HasReply;
        reply = state->Reply;
        stream.Reset();
    }
    else
    {
        MARLIN_LOG(LogCore, Warning, "Cannot reach MarlinCrashServer at {0}:{1}", host, port);
    }
    loop.Stop();
    return answered;
}
} // namespace Marlin
