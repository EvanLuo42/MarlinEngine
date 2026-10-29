#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "GlobalStore/GlobalStoreClient.h"
#include "GlobalStore/GlobalStoreProtocol.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetListener.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/String.h"

namespace Marlin
{
struct GlobalStoreServerDesc
{
    /// Zero takes any free port; GetPort says which.
    uint16 Port = 0;
    /// Listen on every interface rather than loopback, for a studio server.
    bool AllowRemote = false;
    /// Where segment files go; values survive restarts there.
    String DataDirectory;
    /// Bytes of values kept in memory; the rest are read from the segments.
    uint64 MemoryBudget = uint64{1} << 30;
    /// Another GlobalStore this one fronts, such as the studio's from a home workstation: misses are fetched from
    /// it and kept here, and stores are passed on to it. Empty for none.
    String UpstreamHost;
    uint16 UpstreamPort = 0;
};

struct GlobalStoreStats
{
    uint64 Values = 0;
    uint64 StoredBytes = 0;
    uint64 CachedBytes = 0;
    uint64 Requests = 0;
    uint64 Hits = 0;
    uint64 UpstreamHits = 0;
};

/// A GlobalStore server (Documentation/Architecture/06 §3.3): an in-memory key-value store in front of append-only
/// segment files, served over Net (14). Requests are parsed and answered on the IO threads, in order per connection.
/// Nothing waits on an IO thread: a value in memory is sent by reference, a small one on disk is read with overlapped
/// I/O on the loop's port and sent once read, a large one goes from the segment file straight to the socket
/// (TransmitFile), and an upstream miss answers when the upstream does. Values are immutable, so a key is stored once
/// and never rewritten.
class GLOBALSTORESERVER_API GlobalStoreServer
{
public:
    GlobalStoreServer(NetLoop &loop, GlobalStoreServerDesc desc);
    ~GlobalStoreServer();
    MARLIN_NONCOPYABLE(GlobalStoreServer);

    /// Rebuilds the index from the segments and starts listening.
    bool Start();
    /// Closes every connection and waits for the requests in flight.
    void Stop();
    /// Reconnects the upstream when it dropped, at most every few seconds; now and then, not on an IO thread.
    void Tick();

    [[nodiscard]] uint16 GetPort() const
    {
        return _listener != nullptr ? _listener->GetPort() : 0;
    }

    [[nodiscard]] GlobalStoreStats GetStats() const;
    /// Clients connected now; MarlinGlobalStore -ExitWhenIdle leaves after a while without any.
    [[nodiscard]] usize GetConnectionCount();

private:
    struct Value : RefCounted
    {
        List<uint8> Bytes;
    };

    /// A segment file: the writer appends under _segmentLock, the overlapped reader serves cold reads and
    /// TransmitFile. Both close when the last entry, read or send using the segment lets go.
    struct Segment : RefCounted
    {
        FileHandle Writer;
        FileHandle Reader;
        ~Segment() override;
    };

    struct Entry
    {
        SharedPtr<Segment> File;
        uint64 Offset = 0;
        uint64 Size = 0;
        SharedPtr<Value> Cached;
        /// Bumped by lookups under the shared lock, so only through an AtomicRef.
        uint64 LastUse = 0;
    };

    /// A part of the entry table; a key's shard comes from its hash, and each shard has its own lock. Lookups share
    /// it; adding an entry or changing what it caches takes it alone.
    struct Shard
    {
        ReadWriteLock Lock;
        HashMap<Hash128, Entry> Entries;
    };

    static constexpr uint32 ShardBits = 6;

    /// One client: each answer goes into its request's slot and is sent once every earlier slot has gone out.
    struct Connection;
    /// A response header and what follows it: the bytes of a value by reference, or a region of a segment file.
    struct Reply;
    /// A segment read in flight on the loop's port.
    struct ColdRead;

    void Accept(SharedPtr<NetStream> stream);
    /// Handles the complete requests in *bytes*; returns the bytes used.
    usize Receive(const SharedPtr<Connection> &connection, Span<const uint8> bytes);
    void Lookup(const SharedPtr<Connection> &connection, uint64 slot, const GlobalStoreRequest &request);
    void AskUpstream(const SharedPtr<Connection> &connection, uint64 slot, const GlobalStoreRequest &request);
    void StartColdRead(const SharedPtr<Connection> &connection, uint64 slot, const GlobalStoreRequest &request,
                       SharedPtr<Segment> segment, uint64 offset, uint64 size);
    void FinishColdRead(ColdRead &read);
    void Closed(Connection &connection);

    [[nodiscard]] Shard &ShardOf(const Hash128 &key)
    {
        return _shards[key.High >> (64 - ShardBits)];
    }

    bool LoadSegments();
    /// Opens segment *index*'s writer and reader; *create* starts an empty file.
    SharedPtr<Segment> OpenSegment(uint32 index, bool create);
    /// Appends a record to the last segment. Called without the key's shard lock, so lookups of other keys of the
    /// shard need not wait for the disk; two stores of one key at once may both append, and the second record is
    /// unused.
    bool Append(const Hash128 &key, Span<const uint8> bytes, SharedPtr<Segment> &segment, uint64 &offset);
    /// Stores *bytes*; *value*, when given, holds them already and is what the memory cache keeps.
    bool Store(const Hash128 &key, Span<const uint8> bytes, SharedPtr<Value> value, bool forward);
    /// Keeps *value* in memory for *key* unless it is too large or already there; returns the cached value.
    SharedPtr<Value> Cache(const Hash128 &key, SharedPtr<Value> value);
    void Evict();

    NetLoop &_loop;
    GlobalStoreServerDesc _desc;
    FileHandle _lockFile;
    UniquePtr<NetListener> _listener;
    Mutex _connectionsLock;
    List<SharedPtr<Connection>> _connections;

    Shard _shards[uint32{1} << ShardBits];
    Mutex _segmentLock;
    List<SharedPtr<Segment>> _segments;
    uint64 _segmentSize = 0;
    Mutex _evictLock;
    Atomic<uint64> _values{0};
    Atomic<uint64> _cachedBytes{0};
    Atomic<uint64> _storedBytes{0};
    Atomic<uint32> _readsInFlight{0};
    Atomic<uint64> _clock{0};
    Atomic<uint64> _requests{0};
    Atomic<uint64> _hits{0};
    Atomic<uint64> _upstreamHits{0};
    UniquePtr<GlobalStoreClient> _upstream;
    Atomic<uint32> _upstreamInFlight{0};
    uint64 _upstreamRetryAt = 0;
};
} // namespace Marlin
