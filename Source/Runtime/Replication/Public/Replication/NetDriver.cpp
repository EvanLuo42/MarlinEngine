#include "Replication/NetDriver.h"

#include "Concurrency/Jobs.h"
#include "Net/NetSubsystem.h"
#include "World/Hierarchy.h"

namespace Marlin
{
// RECOVERY-DECISION D6: 1200 bytes, a snapshot that fits one Ethernet-sized datagram with room for transport headers.
uint32 NetDriver::SnapshotBudget = 1200;

namespace
{
// RECOVERY-DECISION D5: channel 0 carries snapshots and acknowledgements, channel 1 is kept for RPCs (14 §5.1 table).
constexpr uint32 SnapshotChannel = 0;
} // namespace

NetSessionDesc NetDriver::MakeSessionDesc()
{
    NetSessionDesc desc;
    desc.Channels.Add({NetDelivery::UnreliableSequenced});
    desc.Channels.Add({NetDelivery::ReliableOrdered});
    return desc;
}

void NetDriver::Initialize(SubsystemContext &context)
{
    _net = context.Collection != nullptr ? context.Collection->Get<NetSubsystem>() : nullptr;
}

void NetDriver::Deinitialize()
{
    Disconnect();
    _net = nullptr;
}

NetTransport *NetDriver::FindTransport(StringView name) const
{
    if (_net == nullptr)
    {
        return nullptr;
    }
    NetTransport *transport = name.IsEmpty() ? _net->GetTransports().GetDefault() : _net->GetTransports().Find(name);
    if (transport == nullptr)
    {
        MARLIN_LOG(LogReplication, Error, "No network transport named {0} is linked",
                   name.IsEmpty() ? NetSubsystem::Transport.AsView() : name);
    }
    return transport;
}

void NetDriver::Attach(const SharedPtr<Peer> &peer)
{
    NetSessionHandlers handlers;
    handlers.OnMessage = [peer](NetSession & /*session*/, uint32 channel, Span<const uint8> message) {
        if (channel == SnapshotChannel)
        {
            const ScopeLock lock(peer->Lock);
            peer->Inbox.Emplace(message);
        }
    };
    handlers.OnClose = [peer](NetSession & /*session*/) {
        const ScopeLock lock(peer->Lock);
        peer->Closed = true;
    };
    peer->Session->Start(Move(handlers));
}

bool NetDriver::Listen(uint16 port, StringView transport)
{
    Disconnect();
    NetTransport *found = FindTransport(transport);
    if (found == nullptr)
    {
        return false;
    }
    NetListenDesc desc;
    desc.Port = port;
    // RECOVERY-DECISION D8: a replication server accepts other machines; loopback-only is for tools.
    desc.AllowRemote = true;
    desc.Session = MakeSessionDesc();
    _server = MakeUnique<ServerReplicator>(GetWorld().GetEntities());
    _endpoint = found->Listen(_net->GetLoop(), desc, [this](SharedPtr<NetSession> session) {
        auto peer = MakeShared<Peer>();
        peer->Session = Move(session);
        Attach(peer);
        const ScopeLock lock(_lock);
        _peers.Add(Move(peer));
    });
    if (_endpoint.Get() == nullptr)
    {
        _server.Reset();
        return false;
    }
    return true;
}

bool NetDriver::Connect(StringView host, uint16 port, StringView transport, uint32 timeout)
{
    Disconnect();
    NetTransport *found = FindTransport(transport);
    if (found == nullptr)
    {
        return false;
    }
    SharedPtr<NetSession> session = found->Connect(_net->GetLoop(), host, port, MakeSessionDesc(), timeout);
    if (session.Get() == nullptr)
    {
        return false;
    }
    _client = MakeUnique<ClientReplicator>(GetWorld().GetEntities());
    auto peer = MakeShared<Peer>();
    peer->Session = Move(session);
    Attach(peer);
    const ScopeLock lock(_lock);
    _peers.Add(Move(peer));
    return true;
}

void NetDriver::Disconnect()
{
    if (_endpoint.Get() != nullptr)
    {
        _endpoint->Close();
        _endpoint.Reset();
    }
    List<SharedPtr<Peer>> peers;
    {
        const ScopeLock lock(_lock);
        peers = Move(_peers);
        _peers.Clear();
    }
    for (const SharedPtr<Peer> &peer : peers)
    {
        peer->Session->Close();
    }
    _server.Reset();
    _client.Reset();
    _tick = 0;
}

void NetDriver::BeginFrame(const FrameTime & /*time*/)
{
    List<SharedPtr<Peer>> peers;
    {
        const ScopeLock lock(_lock);
        peers = _peers;
    }
    Events<NetConnectionEvent> &events = GetWorld().GetEvents().Get<NetConnectionEvent>();
    List<List<uint8>> inbox;
    List<uint8> acknowledgement;
    for (const SharedPtr<Peer> &peer : peers)
    {
        if (_server.Get() != nullptr && peer->Connection == 0)
        {
            peer->Connection = _server->AddConnection();
            events.Send({peer->Connection, true});
        }
        bool closed = false;
        {
            const ScopeLock lock(peer->Lock);
            inbox = Move(peer->Inbox);
            peer->Inbox.Clear();
            closed = peer->Closed;
        }
        for (const List<uint8> &message : inbox)
        {
            if (_server.Get() != nullptr)
            {
                _server->Receive(peer->Connection, message.AsSpan());
            }
            else if (_client.Get() != nullptr && _client->Receive(message.AsSpan(), acknowledgement))
            {
                (void)peer->Session->Send(SnapshotChannel, acknowledgement.AsSpan());
            }
        }
        if (closed)
        {
            if (_server.Get() != nullptr)
            {
                _server->RemoveConnection(peer->Connection);
            }
            events.Send({_server.Get() != nullptr ? peer->Connection : 0, false});
            const ScopeLock lock(_lock);
            (void)_peers.Remove(peer);
        }
    }
}

void NetDriver::SendSnapshots()
{
    if (_server.Get() == nullptr)
    {
        return;
    }
    _server->Update();
    ++_tick;
    _sending.Clear();
    {
        const ScopeLock lock(_lock);
        for (const SharedPtr<Peer> &peer : _peers)
        {
            if (peer->Connection != 0 && peer->Session->IsOpen())
            {
                _sending.Add(peer);
            }
        }
    }
    // Update captured the world once; each connection's snapshot reads that and its own history only.
    while (_messages.Count() < _sending.Count())
    {
        _messages.AddDefaulted();
    }
    Jobs::ParallelFor(static_cast<uint32>(_sending.Count()), 1, [&](uint32 begin, uint32 end) {
        for (uint32 i = begin; i < end; ++i)
        {
            Peer &peer = *_sending[i];
            const usize limit = peer.Session->GetMaxMessageSize();
            const uint32 budget = limit < SnapshotBudget ? static_cast<uint32>(limit) : SnapshotBudget;
            _server->WriteSnapshot(peer.Connection, _tick, budget, _messages[i]);
            (void)peer.Session->Send(SnapshotChannel, _messages[i].AsSpan());
        }
    });
}

void NetSnapshotSystem::Configure(SystemBuilder &builder)
{
    builder.WriteComponent<NetworkIdentity>();
    builder.ReadComponent<NetOwner>();
    builder.ReadComponent<NetRelevancy>();
    builder.ReadComponent<WorldTransform>();
    for (const ReplicatedComponent *replicated : ReplicationSchema::GetAll())
    {
        builder.ReadComponent(replicated->Component);
    }
}

void NetSnapshotSystem::Execute(SystemContext &context)
{
    if (NetDriver *driver = context.GetWorld().GetSubsystem<NetDriver>())
    {
        driver->SendSnapshots();
    }
}
} // namespace Marlin
