#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "CrashReport/CrashProtocol.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetListener.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/String.h"
#include "CrashServer/CrashServer.gen.h"

namespace Marlin
{
struct CrashServerDesc
{
    /// Zero takes any free port; GetPort says which.
    uint16 Port = 0;
    /// Listen on every interface rather than loopback, for a studio server.
    bool AllowRemote = false;
    /// Where Reports/ and Buckets/ go.
    String DataDirectory;
    /// A symbol store `marlin symbols` wrote; empty to analyze without symbols.
    String SymbolStore;
    /// Days reports are kept; zero keeps them until their bucket drops them.
    uint32 RetentionDays = 0; // RECOVERY-DECISION D7: crash server setting chosen (not recovered).
};

/// Buckets/<signature>.mtext: the reports that share a signature, most likely the same bug.
MSTRUCT()
struct CrashBucket
{
    MGENERATED_BODY()

    MPROPERTY()
    String Signature;
    /// The frames the signature hashes, one per line.
    MPROPERTY()
    String Frames;
    MPROPERTY()
    String Kind;
    /// Of the first report: the failure message, or the exception code.
    MPROPERTY()
    String Summary;
    MPROPERTY()
    uint64 Count = 0;
    MPROPERTY()
    String FirstSeen;
    MPROPERTY()
    String LastSeen;
    /// Engine and product versions seen, such as "0.1 / 1.2.0".
    MPROPERTY()
    List<String> Versions;
    /// The latest reports, oldest first, at most MaxReports; older ones are deleted.
    MPROPERTY()
    List<uint64> Reports;

    static constexpr usize MaxReports = 20; // RECOVERY-DECISION D7: crash server setting chosen (not recovered).
};

struct CrashServerStats
{
    uint64 Reports = 0;
    uint64 Buckets = 0;
    uint64 Rejected = 0;
};

/// The crash report server (Documentation/Architecture/03 §7.1): receives the report folders MarlinCrashReporter
/// uploads over Net (14), analyzes their minidumps with the symbol store and groups them into buckets by signature.
/// Uploads are received on the IO threads; analysis runs in Tick, one report at a time, and each upload is answered
/// once its report is in a bucket.
class CRASHSERVER_API CrashServer
{
public:
    CrashServer(NetLoop &loop, CrashServerDesc desc);
    ~CrashServer();
    MARLIN_NONCOPYABLE(CrashServer);

    /// Loads the buckets and starts listening.
    bool Start();
    /// Closes every connection; reports not analyzed yet are analyzed on the next Start.
    void Stop();
    /// Analyzes the uploads that finished and answers them, and deletes expired reports now and then.
    void Tick();

    [[nodiscard]] uint16 GetPort() const
    {
        return _listener != nullptr ? _listener->GetPort() : 0;
    }

    [[nodiscard]] CrashServerStats GetStats() const;

private:
    struct Connection;

    void Accept(SharedPtr<NetStream> stream);
    usize Receive(const SharedPtr<Connection> &connection, Span<const uint8> bytes);
    void Reject(Connection &connection);
    void Closed(Connection &connection);

    Hash128 Process(uint64 number);
    bool LoadBuckets();
    void SaveBucket(const Hash128 &signature, const CrashBucket &bucket) const;
    /// Deletes the reports older than RetentionDays.
    void Prune();

    NetLoop &_loop;
    CrashServerDesc _desc;
    FileHandle _lockFile;
    UniquePtr<NetListener> _listener;
    Mutex _connectionsLock;
    List<SharedPtr<Connection>> _connections;
    Mutex _queueLock;
    List<SharedPtr<Connection>> _finished;
    List<uint64> _unprocessed;
    Atomic<uint64> _nextReport{1};
    HashMap<Hash128, CrashBucket> _buckets;
    /// The bucket of every report kept.
    HashMap<uint64, Hash128> _reportBuckets;
    uint64 _nextPrune = 0;
    Atomic<uint64> _reports{0};
    Atomic<uint64> _rejected{0};
};
} // namespace Marlin
