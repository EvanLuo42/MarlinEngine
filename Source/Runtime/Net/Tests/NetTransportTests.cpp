#include "Testing/Testing.h"

#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformSync.h"
#include "Memory/MemoryOps.h"
#include "Net/NetListener.h"
#include "Net/NetStream.h"
#include "NetTestTypes.h"
#include "String/Format.h"

namespace Marlin::Tests
{
bool TestCountingTransport::Enabled = false;
const NetTransportSet *TestCountingTransport::Owner = nullptr;
Atomic<uint32> TestCountingTransport::Connects{0};

namespace
{
constexpr usize MaxMessage = 16 << 20;

struct FrameHeader
{
    uint32 Channel = 0;
    uint32 Size = 0;
};

class StreamSession final : public NetSession
{
public:
    StreamSession(SharedPtr<NetStream> stream, const NetSessionDesc &desc)
        : NetSession(Format("test {0}", stream->Describe()).AsView()), _stream(Move(stream)),
          _channels(static_cast<uint32>(desc.Channels.Count()))
    {
    }

    bool Send(uint32 channel, Span<const uint8> message) override
    {
        if (!IsOpen() || channel >= _channels || message.Count() > MaxMessage)
        {
            return false;
        }
        FrameHeader header{channel, static_cast<uint32>(message.Count())};
        List<uint8> bytes;
        bytes.Reserve(sizeof(header) + message.Count());
        bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&header), sizeof(header)));
        bytes.AddRange(message);
        _stream->Send(Move(bytes));
        return true;
    }

    void Close() override
    {
        _stream->Close();
    }

    [[nodiscard]] NetSessionStats GetStats() const override
    {
        NetSessionStats stats;
        stats.QueuedBytes = _stream->GetQueuedBytes();
        return stats;
    }

    [[nodiscard]] usize GetMaxMessageSize() const override
    {
        return MaxMessage;
    }

protected:
    void OnStart() override
    {
        const SharedPtr<StreamSession> self(this);
        NetStreamHandlers handlers;
        handlers.OnReceive = [self](NetStream &stream, Span<const uint8> bytes) {
            usize consumed = 0;
            while (bytes.Count() - consumed >= sizeof(FrameHeader))
            {
                FrameHeader header;
                MemCopy(&header, bytes.Data() + consumed, sizeof(header));
                if (header.Channel >= self->_channels || header.Size > MaxMessage)
                {
                    stream.Abort();
                    return bytes.Count();
                }
                if (bytes.Count() - consumed - sizeof(header) < header.Size)
                {
                    break;
                }
                self->Deliver(header.Channel, bytes.Slice(consumed + sizeof(header), header.Size));
                consumed += sizeof(header) + header.Size;
            }
            return consumed;
        };
        handlers.OnClose = [self](NetStream & /*stream*/) { self->Finish(); };
        _stream->Start(Move(handlers));
    }

private:
    SharedPtr<NetStream> _stream;
    uint32 _channels;
};

class StreamEndpoint final : public NetEndpoint
{
public:
    explicit StreamEndpoint(UniquePtr<NetListener> listener) : _listener(Move(listener))
    {
    }

    [[nodiscard]] uint16 GetPort() const override
    {
        return _listener->GetPort();
    }

    void Close() override
    {
        _listener->Close();
    }

private:
    UniquePtr<NetListener> _listener;
};

bool SupportsAll(const NetTransport &transport, const NetSessionDesc &desc)
{
    for (const NetChannelDesc &channel : desc.Channels)
    {
        if (!transport.Supports(channel.Delivery))
        {
            return false;
        }
    }
    return !desc.Channels.IsEmpty();
}

NetSessionDesc TwoChannels()
{
    NetSessionDesc desc;
    desc.Channels.Add({NetDelivery::UnreliableSequenced});
    desc.Channels.Add({NetDelivery::ReliableOrdered});
    return desc;
}

/// What one end of a session received.
struct Inbox
{
    Mutex Lock;
    List<uint32> Channels;
    List<String> Messages;
    Event Arrived{EventReset::Auto};
    Event Closed{EventReset::Manual};

    NetSessionHandlers Handlers()
    {
        NetSessionHandlers handlers;
        handlers.OnMessage = [this](NetSession & /*session*/, uint32 channel, Span<const uint8> message) {
            {
                const ScopeLock lock(Lock);
                Channels.Add(channel);
                Messages.Add(StringView(reinterpret_cast<const char *>(message.Data()), message.Count()));
            }
            Arrived.Set();
        };
        handlers.OnClose = [this](NetSession & /*session*/) { Closed.Set(); };
        return handlers;
    }

    bool WaitFor(usize count)
    {
        while (true)
        {
            {
                const ScopeLock lock(Lock);
                if (Messages.Count() >= count)
                {
                    return true;
                }
            }
            if (!Arrived.Wait(5000))
            {
                return false;
            }
        }
    }
};

Span<const uint8> Bytes(StringView text)
{
    return {reinterpret_cast<const uint8 *>(text.Data()), text.Length()};
}
} // namespace

bool TestStreamTransport::Supports(NetDelivery /*delivery*/) const
{
    return true;
}

UniquePtr<NetEndpoint> TestStreamTransport::Listen(NetLoop &loop, const NetListenDesc &desc,
                                                   NetSessionAcceptHandler onAccept)
{
    if (!SupportsAll(*this, desc.Session))
    {
        return nullptr;
    }
    UniquePtr<NetListener> listener =
        NetListener::ListenTcp(loop, desc.Port, desc.AllowRemote,
                               [session = desc.Session, onAccept = Move(onAccept)](SharedPtr<NetStream> stream) {
                                   onAccept(SharedPtr<NetSession>(new StreamSession(Move(stream), session)));
                               });
    return listener != nullptr ? UniquePtr<NetEndpoint>(new StreamEndpoint(Move(listener))) : nullptr;
}

SharedPtr<NetSession> TestStreamTransport::Connect(NetLoop &loop, StringView host, uint16 port,
                                                   const NetSessionDesc &desc, uint32 timeout)
{
    if (!SupportsAll(*this, desc))
    {
        return {};
    }
    SharedPtr<NetStream> stream = NetStream::ConnectTcp(loop, host, port, timeout);
    return stream.IsValid() ? SharedPtr<NetSession>(new StreamSession(Move(stream), desc)) : SharedPtr<NetSession>();
}

bool TestCountingTransport::ShouldCreate(const ExtensionContext & /*context*/)
{
    return Enabled;
}

bool TestCountingTransport::Supports(NetDelivery delivery) const
{
    return Owner->GetReplaced(*this)->Supports(delivery);
}

UniquePtr<NetEndpoint> TestCountingTransport::Listen(NetLoop &loop, const NetListenDesc &desc,
                                                     NetSessionAcceptHandler onAccept)
{
    return Owner->GetReplaced(*this)->Listen(loop, desc, Move(onAccept));
}

SharedPtr<NetSession> TestCountingTransport::Connect(NetLoop &loop, StringView host, uint16 port,
                                                     const NetSessionDesc &desc, uint32 timeout)
{
    Connects.FetchAdd(1);
    return Owner->GetReplaced(*this)->Connect(loop, host, port, desc, timeout);
}

MARLIN_TEST(Net, TransportSelection)
{
    TestCountingTransport::Enabled = false;
    NetTransportSet transports;
    transports.Build({}, "test");
    MARLIN_REQUIRE(transports.GetDefault() != nullptr);
    MARLIN_EXPECT(&transports.GetDefault()->GetType() == &TypeOf<TestStreamTransport>());
    MARLIN_EXPECT(transports.Find("ReliableOnly") != nullptr && transports.Find("Kcp") == nullptr);

    const String disabled[] = {String("TestReliableOnlyTransport")};
    ExtensionContext context;
    context.Disabled = Span<const String>(disabled, 1);
    NetTransportSet trimmed;
    trimmed.Build(context, "Udp");
    MARLIN_EXPECT(trimmed.Find("ReliableOnly") == nullptr && trimmed.GetDefault() == nullptr);

    TestCountingTransport::Enabled = true;
    NetTransportSet wrapped;
    wrapped.Build({}, "Test");
    MARLIN_REQUIRE(wrapped.GetDefault() != nullptr);
    MARLIN_EXPECT(&wrapped.GetDefault()->GetType() == &TypeOf<TestCountingTransport>());
    MARLIN_EXPECT(wrapped.GetReplaced(*wrapped.GetDefault()) != nullptr);
    TestCountingTransport::Enabled = false;
}

MARLIN_TEST(Net, TransportSessions)
{
    TestCountingTransport::Enabled = true;
    NetTransportSet transports;
    transports.Build({}, "Test");
    TestCountingTransport::Owner = &transports;
    TestCountingTransport::Connects.Store(0);
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    NetTransport &transport = *transports.GetDefault();

    Mutex lock;
    List<SharedPtr<NetSession>> accepted;
    Inbox server;
    NetListenDesc listen;
    listen.Session = TwoChannels();
    UniquePtr<NetEndpoint> endpoint = transport.Listen(loop, listen, [&](SharedPtr<NetSession> session) {
        session->Start(server.Handlers());
        const ScopeLock scope(lock);
        accepted.Add(Move(session));
    });
    MARLIN_REQUIRE(endpoint != nullptr);

    SharedPtr<NetSession> client = transport.Connect(loop, "127.0.0.1", endpoint->GetPort(), TwoChannels(), 2000);
    MARLIN_REQUIRE(client.IsValid());
    MARLIN_EXPECT(TestCountingTransport::Connects.Load() == 1);
    Inbox replies;
    client->Start(replies.Handlers());
    MARLIN_EXPECT(client->Send(0, Bytes("snapshot")) && client->Send(1, Bytes("rpc")));
    MARLIN_EXPECT(!client->Send(2, Bytes("no such channel")));
    MARLIN_REQUIRE(server.WaitFor(2));
    {
        const ScopeLock scope(server.Lock);
        MARLIN_EXPECT(server.Channels[0] == 0 && server.Messages[0] == "snapshot");
        MARLIN_EXPECT(server.Channels[1] == 1 && server.Messages[1] == "rpc");
    }
    {
        const ScopeLock scope(lock);
        MARLIN_REQUIRE(accepted.Count() == 1);
        MARLIN_EXPECT(accepted[0]->Send(1, Bytes("answer")));
    }
    MARLIN_REQUIRE(replies.WaitFor(1));

    NetSessionDesc unreliable;
    unreliable.Channels.Add({NetDelivery::Unreliable});
    NetTransport *reliableOnly = transports.Find("ReliableOnly");
    MARLIN_REQUIRE(reliableOnly != nullptr);
    MARLIN_EXPECT(!reliableOnly->Connect(loop, "127.0.0.1", endpoint->GetPort(), unreliable, 500).IsValid());

    client->Close();
    MARLIN_EXPECT(replies.Closed.Wait(5000) && server.Closed.Wait(5000));
    MARLIN_EXPECT(!client->IsOpen());
    endpoint->Close();
    accepted.Clear();
    client.Reset();
    loop.Stop();
    TestCountingTransport::Owner = nullptr;
    TestCountingTransport::Enabled = false;
}
} // namespace Marlin::Tests
