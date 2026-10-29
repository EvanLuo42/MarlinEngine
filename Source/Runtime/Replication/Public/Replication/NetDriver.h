#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetTransport.h"
#include "Replication/Replicator.h"
#include "String/String.h"
#include "World/System.h"
#include "World/World.h"
#include "Replication/NetDriver.gen.h"

namespace Marlin
{
class NetSubsystem;

/// Sent through the world's Events when a client connects to or leaves a server, so gameplay can spawn or remove its
/// player and give the connection a viewer.
MSTRUCT()
struct NetConnectionEvent
{
    MGENERATED_BODY()

    /// The ServerReplicator connection id; zero on a client.
    MPROPERTY()
    uint32 Connection = 0;

    MPROPERTY()
    bool Connected = false;
};

/// A world's end of replication (Documentation/Architecture/05 §7): as a server it listens through a NetTransport and
/// sends each connection a snapshot every fixed tick on an UnreliableSequenced channel; as a client it connects,
/// applies the snapshots and acknowledges them. Messages arrive on IO threads and are handled at the start of the
/// world's frame. Created when NetServerSet or NetClientSet is active.
MCLASS(Config = "Engine.Replication", Dependencies = "NetSubsystem", Sets = (NetServerSet, NetClientSet))
class REPLICATION_API NetDriver final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    /// The most a snapshot may take, in bytes, before the session's own limit.
    MPROPERTY()
    static uint32 SnapshotBudget;

    /// The channels of a replication session: snapshots and acknowledgements, then RPCs.
    static NetSessionDesc MakeSessionDesc();

    /// Serves on *port* through *transport*, or Engine.Net.Transport if empty.
    bool Listen(uint16 port, StringView transport = {});
    /// Connects to a server; blocks for at most *timeout* milliseconds.
    bool Connect(StringView host, uint16 port, StringView transport = {}, uint32 timeout = 5000);
    void Disconnect();

    [[nodiscard]] bool IsServer() const
    {
        return _server.Get() != nullptr;
    }

    [[nodiscard]] ServerReplicator *GetServer() const
    {
        return _server.Get();
    }

    [[nodiscard]] ClientReplicator *GetClient() const
    {
        return _client.Get();
    }

    /// Writes and sends this tick's snapshot to every connection; NetSnapshotSystem calls it.
    void SendSnapshots();

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

private:
    /// A session and what its IO thread received for the world's next frame.
    struct Peer
    {
        SpinLock Lock;
        uint32 Connection = 0;
        SharedPtr<NetSession> Session;
        List<List<uint8>> Inbox;
        bool Closed = false;
    };

    [[nodiscard]] NetTransport *FindTransport(StringView name) const;
    static void Attach(const SharedPtr<Peer> &peer);

    NetSubsystem *_net = nullptr;
    UniquePtr<NetEndpoint> _endpoint;
    UniquePtr<ServerReplicator> _server;
    UniquePtr<ClientReplicator> _client;
    SpinLock _lock;
    List<SharedPtr<Peer>> _peers;
    uint32 _tick = 0;
    // Scratch for SendSnapshots, kept for its memory.
    List<SharedPtr<Peer>> _sending;
    List<List<uint8>> _messages;
};

/// Sends the server's snapshots once per fixed tick, after gameplay.
MCLASS(Phase = NetPhase, Sets = (NetServerSet))
class REPLICATION_API NetSnapshotSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;
};
} // namespace Marlin
