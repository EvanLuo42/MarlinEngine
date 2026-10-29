#pragma once

#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Net/NetLoop.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"
#include "Net/NetTransport.gen.h"

namespace Marlin
{
/// What a channel guarantees (Documentation/Architecture/14 §5.1). A fixed set of semantics: transports differ in how
/// they provide them, not in which exist.
enum class NetDelivery : uint8
{
    /// May be lost, duplicated or reordered.
    Unreliable,
    /// May be lost; a message older than one already delivered is dropped. For snapshots.
    UnreliableSequenced,
    /// Arrives exactly once, in any order.
    Reliable,
    /// Arrives exactly once, in the order sent. For RPCs.
    ReliableOrdered,
};

struct NetChannelDesc
{
    NetDelivery Delivery = NetDelivery::ReliableOrdered;
};

/// The channels of a session, numbered by their index; both ends declare the same ones.
struct NetSessionDesc
{
    List<NetChannelDesc> Channels;
    /// A session that hears nothing from its peer for this long closes.
    uint32 TimeoutMilliseconds = 10000;
};

struct NetListenDesc
{
    /// Zero takes any free port; NetEndpoint::GetPort says which.
    uint16 Port = 0;
    /// Listen on every interface instead of loopback.
    bool AllowRemote = false;
    NetSessionDesc Session;
};

struct NetSessionStats
{
    float64 RoundTripMilliseconds = 0;
    /// The fraction of packets lost lately, from 0 to 1.
    float64 LossRate = 0;
    uint64 BytesSent = 0;
    uint64 BytesReceived = 0;
    /// Bytes accepted by Send and not yet acknowledged or sent.
    uint64 QueuedBytes = 0;
};

class NetSession;

struct NetSessionHandlers
{
    /// One whole message on *channel*; a session's messages never arrive concurrently.
    Function<void(NetSession &session, uint32 channel, Span<const uint8> message)> OnMessage;
    /// Once, after the last OnMessage, when the session closed for any reason.
    Function<void(NetSession &session)> OnClose;
};

/// A connection made by a NetTransport, carrying whole messages on the channels it was created with. Handlers run on
/// IO threads; Send works from any thread. A transport derives from it and calls Deliver for each message received
/// and Finish once when the session ends.
class NET_API NetSession : public RefCounted
{
public:
    ~NetSession() override = default;
    MARLIN_NONCOPYABLE(NetSession);

    /// Starts delivering messages; nothing is delivered before.
    void Start(NetSessionHandlers handlers);

    /// Queues *message* on *channel*; false if the session is closed, the channel unknown or the message too big.
    virtual bool Send(uint32 channel, Span<const uint8> message) = 0;
    /// Sends what reliable channels hold, then closes.
    virtual void Close() = 0;
    [[nodiscard]] virtual NetSessionStats GetStats() const = 0;
    [[nodiscard]] virtual usize GetMaxMessageSize() const = 0;

    [[nodiscard]] bool IsOpen() const
    {
        return !_finished.Load(MemoryOrder::Acquire);
    }

    /// Such as "kcp 127.0.0.1:50000", for logs.
    [[nodiscard]] StringView Describe() const
    {
        return _description;
    }

protected:
    explicit NetSession(StringView description);

    /// Called by Start: begin receiving, so that Deliver may follow.
    virtual void OnStart() = 0;
    /// Hands a message to the handler. Deliver and Finish come from the transport's receive path, one at a time.
    void Deliver(uint32 channel, Span<const uint8> message);
    /// Ends the session: the close handler runs once, then the handlers are released. Later calls do nothing; Close
    /// makes the receive path end rather than calling this itself.
    void Finish();

private:
    String _description;
    NetSessionHandlers _handlers;
    Mutex _lock;
    Atomic<bool> _finished{false};
};

using NetSessionAcceptHandler = Function<void(SharedPtr<NetSession> session)>;

/// Where a transport accepts sessions: each new one goes to the accept handler, not yet started, on an IO thread.
class NET_API NetEndpoint
{
public:
    NetEndpoint() = default;
    virtual ~NetEndpoint() = default;
    MARLIN_NONCOPYABLE(NetEndpoint);

    [[nodiscard]] virtual uint16 GetPort() const = 0;
    /// Stops accepting and waits for the work in flight, so the accept handler never runs after it returns. Sessions
    /// already accepted stay open. Not from an IO thread.
    virtual void Close() = 0;
};

/// How sessions get between machines (Documentation/Architecture/14 §5.1): the engine's reliable UDP, KCP, Steam
/// Networking Sockets, a console relay. An extension point chosen by name; implementations are stateless factories
/// and must refuse channels whose delivery they cannot provide rather than weaken it.
MCLASS(Abstract, ExtensionPoint)
class NET_API NetTransport
{
    MGENERATED_BODY()
public:
    NetTransport() = default;
    virtual ~NetTransport() = default;
    MARLIN_NONCOPYABLE(NetTransport);

    /// The name Engine.Net.Transport and NetTransportSet::Find use, such as "Udp" or "Kcp".
    [[nodiscard]] virtual StringView GetName() const = 0;
    [[nodiscard]] virtual bool Supports(NetDelivery delivery) const = 0;
    /// Null if it cannot listen as asked.
    [[nodiscard]] virtual UniquePtr<NetEndpoint> Listen(NetLoop &loop, const NetListenDesc &desc,
                                                        NetSessionAcceptHandler onAccept) = 0;
    /// Connects in the calling thread, which must not be an IO thread; null on failure or after *timeout*
    /// milliseconds. The session is not yet started.
    [[nodiscard]] virtual SharedPtr<NetSession> Connect(NetLoop &loop, StringView host, uint16 port,
                                                        const NetSessionDesc &desc, uint32 timeout) = 0;
};

/// The transports linked into the process, by name. Two with the same name and Priority are a fatal error at Build;
/// otherwise the higher Priority wins. Immutable once built.
class NET_API NetTransportSet
{
public:
    NetTransportSet() = default;
    MARLIN_NONCOPYABLE(NetTransportSet);

    /// Creates the transports; *defaultName* is what GetDefault returns, normally Engine.Net.Transport.
    void Build(const ExtensionContext &context, StringView defaultName);

    /// The transport named *name*, case-insensitively, or null.
    [[nodiscard]] NetTransport *Find(StringView name) const;
    /// The default transport, or null if none by that name is linked.
    [[nodiscard]] NetTransport *GetDefault() const
    {
        return _default;
    }

    [[nodiscard]] usize Count() const
    {
        return _transports.Count();
    }

    [[nodiscard]] NetTransport *const *begin() const
    {
        return _transports.begin();
    }

    [[nodiscard]] NetTransport *const *end() const
    {
        return _transports.end();
    }

    /// The transport *transport* replaces through `MCLASS(Replaces = "Other")`, to wrap it; or null.
    [[nodiscard]] NetTransport *GetReplaced(const NetTransport &transport) const
    {
        return _transports.GetReplaced(transport);
    }

private:
    ExtensionSet<NetTransport> _transports;
    HashMap<String, NetTransport *> _byName;
    NetTransport *_default = nullptr;
};
} // namespace Marlin
