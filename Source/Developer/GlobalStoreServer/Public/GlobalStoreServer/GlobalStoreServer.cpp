#include "GlobalStoreServer/GlobalStoreServer.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Assert.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
constexpr uint32 RecordMagic = 0x5253474Du;
constexpr uint64 MaxSegmentSize = uint64{1} << 30;
constexpr uint64 UpstreamRetrySeconds = 5;
/// A value on disk at least this large is not read into memory: a Get sends its range straight from the segment.
constexpr uint64 TransmitFileSize = uint64{1} << 20;

struct RecordHeader
{
    uint32 Magic = RecordMagic;
    uint32 Padding = 0;
    Hash128 Key;
    uint64 Size = 0;
    uint64 Checksum = 0;
};

String SegmentPath(StringView directory, uint32 index)
{
    return Format("{0}/Segment.{1}.mgs", directory, index);
}

uint64 Seconds()
{
    return PlatformTime::Ticks() / PlatformTime::TicksPerSecond();
}

/// The bytes of a *totalSize* value that *request* gets back, from *offset*: zero for anything but a Get.
uint64 Range(const GlobalStoreRequest &request, uint64 totalSize, uint64 &offset)
{
    offset = 0;
    if (request.Op != GlobalStoreOp::Get || request.Offset > totalSize)
    {
        return 0;
    }
    offset = request.Offset;
    const uint64 available = totalSize - request.Offset;
    return request.Size < available ? request.Size : available;
}
} // namespace

GlobalStoreServer::Segment::~Segment()
{
    if (Writer.IsValid())
    {
        PlatformFile::Close(Writer);
    }
    if (Reader.IsValid())
    {
        PlatformFile::Close(Reader);
    }
}

struct GlobalStoreServer::Reply
{
    GlobalStoreResponse Header;
    /// Keeps Bytes, or File, alive until they are sent.
    SharedPtr<RefCounted> Owner;
    const uint8 *Bytes = nullptr;
    FileHandle File;
    uint64 FileOffset = 0;

    /// An answer with nothing after the header.
    static Reply Status(const GlobalStoreRequest &request, GlobalStoreStatus status, uint64 totalSize = 0)
    {
        Reply reply;
        reply.Header.Id = request.Id;
        reply.Header.Status = status;
        reply.Header.TotalSize = totalSize;
        return reply;
    }

    /// The part of *value* the request asks for, by reference.
    static Reply FromMemory(const GlobalStoreRequest &request, SharedPtr<Value> value)
    {
        Reply reply = Status(request, GlobalStoreStatus::Ok, value->Bytes.Count());
        uint64 offset = 0;
        reply.Header.Size = Range(request, value->Bytes.Count(), offset);
        if (reply.Header.Size != 0)
        {
            reply.Bytes = value->Bytes.Data() + offset;
            reply.Owner = Move(value);
        }
        return reply;
    }

    /// The part of the *size* bytes at *offset* of *segment* the request asks for, sent from the file.
    static Reply FromFile(const GlobalStoreRequest &request, SharedPtr<Segment> segment, uint64 offset, uint64 size)
    {
        Reply reply = Status(request, GlobalStoreStatus::Ok, size);
        uint64 start = 0;
        reply.Header.Size = Range(request, size, start);
        if (reply.Header.Size != 0)
        {
            reply.File = segment->Reader;
            reply.FileOffset = offset + start;
            reply.Owner = Move(segment);
        }
        return reply;
    }
};

struct GlobalStoreServer::Connection : RefCounted
{
    struct Slot
    {
        bool Ready = false;
        Reply Answer;
    };

    SharedPtr<NetStream> Stream;
    Mutex Lock;
    List<Slot> Slots;
    usize Head = 0;
    uint64 Base = 0;
    uint64 Next = 0;

    uint64 Reserve()
    {
        const ScopeLock lock(Lock);
        Slots.AddDefaulted();
        return Next++;
    }

    void Fill(uint64 slot, Reply reply)
    {
        const ScopeLock lock(Lock);
        Slot &target = Slots[static_cast<usize>(slot - Base)];
        target.Ready = true;
        target.Answer = Move(reply);
        while (Head < Slots.Count() && Slots[Head].Ready)
        {
            Send(Slots[Head].Answer);
            ++Head;
        }
        if (Head == Slots.Count())
        {
            Base += Head;
            Slots.Clear();
            Head = 0;
        }
    }

    /// Copies the header; what follows it is queued by reference, so its owner lives until the send completes.
    void Send(Reply &reply)
    {
        List<uint8> header;
        header.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&reply.Header), sizeof(reply.Header)));
        Stream->Send(Move(header));
        if (reply.File.IsValid())
        {
            if (!Stream->SendFile(Move(reply.Owner), reply.File, reply.FileOffset, reply.Header.Size))
            {
                Stream->Abort();
            }
        }
        else if (reply.Header.Size != 0)
        {
            Stream->Send(Move(reply.Owner), Span<const uint8>(reply.Bytes, static_cast<usize>(reply.Header.Size)));
        }
    }
};

struct GlobalStoreServer::ColdRead final : INetCompletion
{
    ColdRead()
    {
        Operation.Context = static_cast<INetCompletion *>(this);
    }

    void Complete(IoOperation & /*operation*/) override
    {
        Server->FinishColdRead(*this);
    }

    GlobalStoreServer *Server = nullptr;
    SharedPtr<Connection> Owner;
    uint64 Slot = 0;
    GlobalStoreRequest Request;
    /// Keeps the reader open while the read is in flight.
    SharedPtr<Segment> File;
    SharedPtr<Value> Read;
    IoOperation Operation;
};

GlobalStoreServer::GlobalStoreServer(NetLoop &loop, GlobalStoreServerDesc desc) : _loop(loop), _desc(Move(desc))
{
}

GlobalStoreServer::~GlobalStoreServer()
{
    Stop();
}

SharedPtr<GlobalStoreServer::Segment> GlobalStoreServer::OpenSegment(uint32 index, bool create)
{
    const String path = SegmentPath(_desc.DataDirectory.AsView(), index);
    SharedPtr<Segment> segment(new Segment());
    segment->Writer = PlatformFile::Open(path.CStr(), FileAccess::ReadWrite,
                                         create ? FileMode::CreateAlways : FileMode::OpenExisting);
    if (!segment->Writer.IsValid())
    {
        return {};
    }
    segment->Reader = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting,
                                         FileFlags::Async | FileFlags::RandomAccess);
    if (!segment->Reader.IsValid() || !_loop.GetPort().Associate(segment->Reader))
    {
        return {};
    }
    return segment;
}

bool GlobalStoreServer::LoadSegments()
{
    for (uint32 index = 0;; ++index)
    {
        if (!PlatformFile::FileExists(SegmentPath(_desc.DataDirectory.AsView(), index).CStr()))
        {
            break;
        }
        SharedPtr<Segment> segment = OpenSegment(index, false);
        if (!segment.IsValid())
        {
            return false;
        }
        uint64 size = 0;
        (void)PlatformFile::GetSize(segment->Writer, size);
        uint64 offset = 0;
        while (offset + sizeof(RecordHeader) <= size)
        {
            RecordHeader header;
            usize read = 0;
            if (!PlatformFile::ReadAt(segment->Writer, offset, &header, sizeof(header), read) ||
                read != sizeof(header) || header.Magic != RecordMagic || offset + sizeof(header) + header.Size > size)
            {
                break;
            }
            Shard &shard = ShardOf(header.Key);
            if (!shard.Entries.Contains(header.Key))
            {
                _values.FetchAdd(1, MemoryOrder::Relaxed);
                _storedBytes.FetchAdd(header.Size, MemoryOrder::Relaxed);
            }
            Entry &entry = shard.Entries.FindOrAdd(header.Key);
            entry.File = segment;
            entry.Offset = offset + sizeof(header);
            entry.Size = header.Size;
            offset += sizeof(header) + header.Size;
        }
        if (offset < size)
        {
            (void)PlatformFile::SetSize(segment->Writer, offset);
        }
        _segments.Add(Move(segment));
        _segmentSize = offset;
    }
    if (_segments.IsEmpty())
    {
        SharedPtr<Segment> segment = OpenSegment(0, true);
        if (!segment.IsValid())
        {
            return false;
        }
        _segments.Add(Move(segment));
        _segmentSize = 0;
    }
    return true;
}

bool GlobalStoreServer::Start()
{
    (void)PlatformFile::CreateDirectory(_desc.DataDirectory.CStr());
    const String lockPath = Format("{0}/GlobalStore.lock", _desc.DataDirectory);
    _lockFile = PlatformFile::Open(lockPath.CStr(), FileAccess::Write, FileMode::CreateAlways,
                                   FileFlags::Exclusive | FileFlags::DeleteOnClose);
    if (!_lockFile.IsValid())
    {
        MARLIN_LOG(LogCore, Error, "Another GlobalStore uses {0}", _desc.DataDirectory);
        return false;
    }
    if (!LoadSegments())
    {
        MARLIN_LOG(LogCore, Error, "GlobalStore cannot open its segments in {0}", _desc.DataDirectory);
        return false;
    }
    if (!_desc.UpstreamHost.IsEmpty())
    {
        _upstream = MakeUnique<GlobalStoreClient>(_loop);
        Tick();
    }
    _listener = NetListener::ListenTcp(_loop, _desc.Port, _desc.AllowRemote,
                                       [this](SharedPtr<NetStream> stream) { Accept(Move(stream)); });
    if (_listener == nullptr)
    {
        MARLIN_LOG(LogCore, Error, "GlobalStore cannot listen on port {0}", _desc.Port);
        return false;
    }
    return true;
}

void GlobalStoreServer::Tick()
{
    if (_upstream == nullptr || _upstream->IsConnected() || Seconds() < _upstreamRetryAt)
    {
        return;
    }
    if (!_upstream->Connect(_desc.UpstreamHost.AsView(), _desc.UpstreamPort))
    {
        _upstreamRetryAt = Seconds() + UpstreamRetrySeconds;
        MARLIN_LOG(LogCore, Warning, "GlobalStore cannot reach upstream {0}:{1}", _desc.UpstreamHost,
                   _desc.UpstreamPort);
    }
}

void GlobalStoreServer::Stop()
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
    while (_readsInFlight.Load(MemoryOrder::Acquire) != 0)
    {
        PlatformThread::Sleep(1);
    }
    if (_upstream != nullptr)
    {
        _upstream->Disconnect();
        while (_upstreamInFlight.Load(MemoryOrder::Acquire) != 0)
        {
            PlatformThread::Sleep(1);
        }
        _upstream.Reset();
    }
    // A send still in flight keeps its value or segment until it completes.
    for (Shard &shard : _shards)
    {
        const ScopeLock lock(shard.Lock);
        shard.Entries.Clear();
    }
    {
        const ScopeLock lock(_segmentLock);
        _segments.Clear();
        _segmentSize = 0;
    }
    _values.Store(0);
    _cachedBytes.Store(0);
    _storedBytes.Store(0);
    if (_lockFile.IsValid())
    {
        PlatformFile::Close(_lockFile);
        _lockFile = {};
    }
}

void GlobalStoreServer::Accept(SharedPtr<NetStream> stream)
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

void GlobalStoreServer::Closed(Connection &connection)
{
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

usize GlobalStoreServer::Receive(const SharedPtr<Connection> &connection, Span<const uint8> bytes)
{
    usize consumed = 0;
    while (bytes.Count() - consumed >= sizeof(GlobalStoreRequest))
    {
        GlobalStoreRequest request;
        MemCopy(&request, bytes.Data() + consumed, sizeof(request));
        const uint64 payload = request.Op == GlobalStoreOp::Put ? request.Size : 0;
        if (request.Magic != GlobalStoreRequestMagic || payload > GlobalStoreMaxValue)
        {
            connection->Stream->Abort();
            return bytes.Count();
        }
        if (bytes.Count() - consumed - sizeof(request) < payload)
        {
            break;
        }
        _requests.FetchAdd(1, MemoryOrder::Relaxed);
        const uint64 slot = connection->Reserve();
        switch (request.Op)
        {
        case GlobalStoreOp::Get:
        case GlobalStoreOp::Has:
            Lookup(connection, slot, request);
            break;
        case GlobalStoreOp::Put: {
            const Span<const uint8> value = bytes.Slice(consumed + sizeof(request), static_cast<usize>(payload));
            const bool stored = Store(request.Key, value, {}, true);
            connection->Fill(slot, Reply::Status(request, stored ? GlobalStoreStatus::Ok : GlobalStoreStatus::Error));
            break;
        }
        default:
            connection->Fill(slot, Reply::Status(request, GlobalStoreStatus::Error));
            break;
        }
        consumed += sizeof(request) + static_cast<usize>(payload);
    }
    return consumed;
}

void GlobalStoreServer::Lookup(const SharedPtr<Connection> &connection, uint64 slot, const GlobalStoreRequest &request)
{
    SharedPtr<Value> cached;
    SharedPtr<Segment> segment;
    uint64 offset = 0;
    uint64 size = 0;
    {
        Shard &shard = ShardOf(request.Key);
        const SharedScopeLock lock(shard.Lock);
        if (Entry *entry = shard.Entries.Find(request.Key); entry != nullptr)
        {
            AtomicRef<uint64>(entry->LastUse).Store(_clock.FetchAdd(1, MemoryOrder::Relaxed), MemoryOrder::Relaxed);
            cached = entry->Cached;
            segment = entry->File;
            offset = entry->Offset;
            size = entry->Size;
        }
    }
    if (!segment.IsValid())
    {
        AskUpstream(connection, slot, request);
        return;
    }
    _hits.FetchAdd(1, MemoryOrder::Relaxed);
    uint64 start = 0;
    if (cached.IsValid())
    {
        connection->Fill(slot, Reply::FromMemory(request, Move(cached)));
    }
    else if (Range(request, size, start) == 0)
    {
        // A Has, or a Get of nothing, needs only the size.
        connection->Fill(slot, Reply::Status(request, GlobalStoreStatus::Ok, size));
    }
    else if (size >= TransmitFileSize)
    {
        connection->Fill(slot, Reply::FromFile(request, Move(segment), offset, size));
    }
    else
    {
        StartColdRead(connection, slot, request, Move(segment), offset, size);
    }
}

void GlobalStoreServer::AskUpstream(const SharedPtr<Connection> &connection, uint64 slot,
                                    const GlobalStoreRequest &request)
{
    if (_upstream == nullptr || !_upstream->IsConnected())
    {
        connection->Fill(slot, Reply::Status(request, GlobalStoreStatus::Missing));
        return;
    }
    _upstreamInFlight.FetchAdd(1, MemoryOrder::Relaxed);
    _upstream->GetAsync(request.Key, 0, GlobalStoreWholeValue,
                        [this, connection, slot, request](GlobalStoreReply &reply) {
                            if (reply.Status == GlobalStoreStatus::Ok)
                            {
                                _upstreamHits.FetchAdd(1, MemoryOrder::Relaxed);
                                SharedPtr<Value> value(new Value());
                                value->Bytes = Move(reply.Bytes);
                                (void)Store(request.Key, value->Bytes.AsSpan(), value, false);
                                connection->Fill(slot, Reply::FromMemory(request, Move(value)));
                            }
                            else
                            {
                                connection->Fill(slot, Reply::Status(request, GlobalStoreStatus::Missing));
                            }
                            _upstreamInFlight.FetchSub(1, MemoryOrder::Release);
                        });
}

void GlobalStoreServer::StartColdRead(const SharedPtr<Connection> &connection, uint64 slot,
                                      const GlobalStoreRequest &request, SharedPtr<Segment> segment, uint64 offset,
                                      uint64 size)
{
    auto *read = new ColdRead();
    read->Server = this;
    read->Owner = connection;
    read->Slot = slot;
    read->Request = request;
    read->File = Move(segment);
    read->Read = SharedPtr<Value>(new Value());
    read->Read->Bytes.ResizeUninitialized(static_cast<usize>(size));
    _readsInFlight.FetchAdd(1, MemoryOrder::Relaxed);
    if (!_loop.GetPort().Read(read->File->Reader, offset, read->Read->Bytes.Data(), read->Read->Bytes.Count(),
                              read->Operation))
    {
        read->Operation.Succeeded = false;
        FinishColdRead(*read);
    }
}

void GlobalStoreServer::FinishColdRead(ColdRead &read)
{
    if (read.Operation.Succeeded && read.Operation.Bytes == read.Read->Bytes.Count())
    {
        SharedPtr<Value> value = Cache(read.Request.Key, Move(read.Read));
        read.Owner->Fill(read.Slot, Reply::FromMemory(read.Request, Move(value)));
    }
    else
    {
        MARLIN_LOG(LogCore, Warning, "GlobalStore cannot read a value from its segments in {0}", _desc.DataDirectory);
        AskUpstream(read.Owner, read.Slot, read.Request);
    }
    _readsInFlight.FetchSub(1, MemoryOrder::Release);
    delete &read;
}

SharedPtr<GlobalStoreServer::Value> GlobalStoreServer::Cache(const Hash128 &key, SharedPtr<Value> value)
{
    const uint64 size = value->Bytes.Count();
    if (size > _desc.MemoryBudget / 4)
    {
        return value;
    }
    bool over = false;
    {
        Shard &shard = ShardOf(key);
        const ScopeLock lock(shard.Lock);
        Entry *entry = shard.Entries.Find(key);
        if (entry == nullptr)
        {
            return value;
        }
        if (entry->Cached.IsValid())
        {
            return entry->Cached;
        }
        entry->Cached = value;
        // Counted under the shard lock, so an eviction of this entry always sees the bytes counted first.
        over = _cachedBytes.FetchAdd(size, MemoryOrder::Relaxed) + size > _desc.MemoryBudget;
    }
    if (over)
    {
        Evict();
    }
    return value;
}

bool GlobalStoreServer::Append(const Hash128 &key, Span<const uint8> bytes, SharedPtr<Segment> &segment, uint64 &offset)
{
    const ScopeLock lock(_segmentLock);
    if (_segments.IsEmpty())
    {
        return false;
    }
    if (_segmentSize + sizeof(RecordHeader) + bytes.Count() > MaxSegmentSize && _segmentSize != 0)
    {
        SharedPtr<Segment> next = OpenSegment(static_cast<uint32>(_segments.Count()), true);
        if (!next.IsValid())
        {
            return false;
        }
        _segments.Add(Move(next));
        _segmentSize = 0;
    }
    RecordHeader header;
    header.Key = key;
    header.Size = bytes.Count();
    header.Checksum = Xxh3Hash64(bytes.Data(), bytes.Count());
    const FileHandle file = _segments.Last()->Writer;
    if (!PlatformFile::WriteAt(file, _segmentSize, &header, sizeof(header)) ||
        !PlatformFile::WriteAt(file, _segmentSize + sizeof(header), bytes.Data(), bytes.Count()))
    {
        return false;
    }
    segment = _segments.Last();
    offset = _segmentSize + sizeof(header);
    _segmentSize += sizeof(header) + bytes.Count();
    return true;
}

bool GlobalStoreServer::Store(const Hash128 &key, Span<const uint8> bytes, SharedPtr<Value> value, bool forward)
{
    Shard &shard = ShardOf(key);
    {
        const SharedScopeLock lock(shard.Lock);
        if (shard.Entries.Contains(key))
        {
            return true;
        }
    }
    // The write happens outside the shard lock, so lookups do not wait for it.
    SharedPtr<Segment> segment;
    uint64 offset = 0;
    if (!Append(key, bytes, segment, offset))
    {
        return false;
    }
    const bool cache = bytes.Count() <= _desc.MemoryBudget / 4;
    if (cache && !value.IsValid())
    {
        value = SharedPtr<Value>(new Value());
        value->Bytes.AddRange(bytes);
    }
    bool over = false;
    {
        const ScopeLock lock(shard.Lock);
        if (shard.Entries.Contains(key))
        {
            // Another store of the key got there first; its record is the one the index uses.
            return true;
        }
        Entry &entry = shard.Entries.FindOrAdd(key);
        entry.File = Move(segment);
        entry.Offset = offset;
        entry.Size = bytes.Count();
        entry.LastUse = _clock.FetchAdd(1, MemoryOrder::Relaxed);
        if (cache)
        {
            entry.Cached = Move(value);
            over = _cachedBytes.FetchAdd(bytes.Count(), MemoryOrder::Relaxed) + bytes.Count() > _desc.MemoryBudget;
        }
    }
    _values.FetchAdd(1, MemoryOrder::Relaxed);
    _storedBytes.FetchAdd(bytes.Count(), MemoryOrder::Relaxed);
    if (over)
    {
        Evict();
    }
    if (forward && _upstream != nullptr && _upstream->IsConnected())
    {
        _upstreamInFlight.FetchAdd(1, MemoryOrder::Relaxed);
        _upstream->PutAsync(
            key, bytes, [this](GlobalStoreReply & /*reply*/) { _upstreamInFlight.FetchSub(1, MemoryOrder::Release); });
    }
    return true;
}

void GlobalStoreServer::Evict()
{
    // One thread evicts at a time; the others carry on, since what it frees is enough for them too.
    if (!_evictLock.TryLock())
    {
        return;
    }
    struct Candidate
    {
        uint64 LastUse;
        Hash128 Key;
    };
    List<Candidate> candidates;
    for (Shard &shard : _shards)
    {
        const SharedScopeLock lock(shard.Lock);
        for (auto &entry : shard.Entries)
        {
            if (entry.Value.Cached.IsValid())
            {
                candidates.Add({AtomicRef<uint64>(entry.Value.LastUse).Load(MemoryOrder::Relaxed), entry.Key});
            }
        }
    }
    candidates.Sort([](const Candidate &a, const Candidate &b) { return a.LastUse < b.LastUse; });
    for (const Candidate &candidate : candidates)
    {
        if (_cachedBytes.Load(MemoryOrder::Relaxed) <= _desc.MemoryBudget * 3 / 4)
        {
            break;
        }
        // A value being sent by reference lives until its send completes; the cache only lets go of it.
        Shard &shard = ShardOf(candidate.Key);
        const ScopeLock lock(shard.Lock);
        if (Entry *entry = shard.Entries.Find(candidate.Key); entry != nullptr && entry->Cached.IsValid())
        {
            _cachedBytes.FetchSub(entry->Size, MemoryOrder::Relaxed);
            entry->Cached.Reset();
        }
    }
    _evictLock.Unlock();
}

usize GlobalStoreServer::GetConnectionCount()
{
    const ScopeLock lock(_connectionsLock);
    return _connections.Count();
}

GlobalStoreStats GlobalStoreServer::GetStats() const
{
    GlobalStoreStats stats;
    stats.Values = _values.Load(MemoryOrder::Relaxed);
    stats.StoredBytes = _storedBytes.Load(MemoryOrder::Relaxed);
    stats.CachedBytes = _cachedBytes.Load(MemoryOrder::Relaxed);
    stats.Requests = _requests.Load(MemoryOrder::Relaxed);
    stats.Hits = _hits.Load(MemoryOrder::Relaxed);
    stats.UpstreamHits = _upstreamHits.Load(MemoryOrder::Relaxed);
    return stats;
}
} // namespace Marlin
