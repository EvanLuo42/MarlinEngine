#include "Testing/Testing.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "Memory/MemoryOps.h"
#include "Net/NetDatagramSocket.h"
#include "Net/NetListener.h"
#include "Net/NetLoop.h"
#include "Net/NetStream.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
/// Echoes every byte back on the streams it accepts.
struct EchoServer
{
    Atomic<uint32> Open{0};
    Event Idle{EventReset::Manual};
    Mutex Lock;
    List<SharedPtr<NetStream>> Streams;

    NetAcceptHandler Handler()
    {
        return [this](SharedPtr<NetStream> stream) {
            Open.FetchAdd(1);
            Idle.Reset();
            {
                const ScopeLock lock(Lock);
                Streams.Add(stream);
            }
            NetStreamHandlers handlers;
            handlers.OnReceive = [](NetStream &self, Span<const uint8> bytes) {
                self.Send(bytes);
                return bytes.Count();
            };
            handlers.OnClose = [this](NetStream & /*self*/) {
                if (Open.FetchSub(1) == 1)
                {
                    Idle.Set();
                }
            };
            stream->Start(Move(handlers));
        };
    }

    void CloseAll()
    {
        {
            const ScopeLock lock(Lock);
            for (SharedPtr<NetStream> &stream : Streams)
            {
                stream->Abort();
            }
            Streams.Clear();
        }
        if (Open.Load() != 0)
        {
            (void)Idle.Wait(5000);
        }
    }
};

/// Collects what a client stream receives.
struct Collector
{
    Mutex Lock;
    List<uint8> Bytes;
    usize Expected = 0;
    Event Complete{EventReset::Manual};
    Event Closed{EventReset::Manual};

    NetStreamHandlers Handlers()
    {
        NetStreamHandlers handlers;
        handlers.OnReceive = [this](NetStream & /*stream*/, Span<const uint8> bytes) {
            const ScopeLock lock(Lock);
            Bytes.AddRange(bytes);
            if (Bytes.Count() >= Expected)
            {
                Complete.Set();
            }
            return bytes.Count();
        };
        handlers.OnClose = [this](NetStream & /*stream*/) { Closed.Set(); };
        return handlers;
    }
};

List<uint8> Pattern(usize size, uint32 seed)
{
    List<uint8> bytes;
    bytes.ResizeUninitialized(size);
    for (usize i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8>((i * 131 + seed) >> 3);
    }
    return bytes;
}

bool Same(Span<const uint8> a, Span<const uint8> b)
{
    return a.Count() == b.Count() && (a.IsEmpty() || MemCompare(a.Data(), b.Data(), a.Count()) == 0);
}

void Echo(NetLoop &loop, const SharedPtr<NetStream> &client, usize size)
{
    Collector collector;
    collector.Expected = size;
    client->Start(collector.Handlers());
    const List<uint8> sent = Pattern(size, 7);
    for (usize offset = 0; offset < size; offset += 100000)
    {
        const usize length = size - offset < 100000 ? size - offset : 100000;
        client->Send(sent.AsSpan().Slice(offset, length));
    }
    MARLIN_REQUIRE(collector.Complete.Wait(10000));
    {
        const ScopeLock lock(collector.Lock);
        MARLIN_EXPECT(Same(collector.Bytes.AsSpan(), sent.AsSpan()));
    }
    client->Close();
    MARLIN_EXPECT(collector.Closed.Wait(5000));
    MARLIN_EXPECT(!client->IsOpen() && loop.IsRunning());
}
} // namespace

MARLIN_TEST(Net, TcpEcho)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    EchoServer server;
    UniquePtr<NetListener> listener = NetListener::ListenTcp(loop, 0, false, server.Handler());
    MARLIN_REQUIRE(listener != nullptr && listener->GetPort() != 0);
    SharedPtr<NetStream> client = NetStream::ConnectTcp(loop, "127.0.0.1", listener->GetPort(), 2000);
    MARLIN_REQUIRE(client.IsValid() && client->IsNetwork());
    Echo(loop, client, 4 << 20);
    MARLIN_EXPECT(!NetStream::ConnectTcp(loop, "127.0.0.1", 1, 500).IsValid());
    listener->Close();
    server.CloseAll();
    loop.Stop();
}

MARLIN_TEST(Net, PipeEcho)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(1));
    EchoServer server;
    const String name = Format("Marlin.NetTest.{0}", PlatformProcess::CurrentId());
    UniquePtr<NetListener> listener = NetListener::ListenPipe(loop, name.AsView(), server.Handler());
    MARLIN_REQUIRE(listener != nullptr);
    SharedPtr<NetStream> client = NetStream::ConnectPipe(loop, name.AsView(), 2000);
    MARLIN_REQUIRE(client.IsValid() && !client->IsNetwork());
    Echo(loop, client, 1 << 20);
    listener->Close();
    server.CloseAll();
    loop.Stop();
}

MARLIN_TEST(Net, ManyConnections)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    EchoServer server;
    UniquePtr<NetListener> listener = NetListener::ListenTcp(loop, 0, false, server.Handler());
    MARLIN_REQUIRE(listener != nullptr);
    constexpr usize Count = 64;
    List<SharedPtr<NetStream>> clients;
    List<UniquePtr<Collector>> collectors;
    for (usize i = 0; i < Count; ++i)
    {
        SharedPtr<NetStream> client = NetStream::ConnectTcp(loop, "127.0.0.1", listener->GetPort(), 2000);
        MARLIN_REQUIRE(client.IsValid());
        UniquePtr<Collector> &collector = collectors.Add(MakeUnique<Collector>());
        collector->Expected = 1000;
        client->Start(collector->Handlers());
        clients.Add(Move(client));
    }
    for (usize i = 0; i < Count; ++i)
    {
        clients[i]->Send(Pattern(1000, static_cast<uint32>(i)).AsSpan());
    }
    bool same = true;
    for (usize i = 0; i < Count; ++i)
    {
        MARLIN_REQUIRE(collectors[i]->Complete.Wait(5000));
        const ScopeLock lock(collectors[i]->Lock);
        same = same && Same(collectors[i]->Bytes.AsSpan(), Pattern(1000, static_cast<uint32>(i)).AsSpan());
    }
    MARLIN_EXPECT(same);
    for (usize i = 0; i < Count; ++i)
    {
        clients[i]->Close();
        MARLIN_EXPECT(collectors[i]->Closed.Wait(5000));
    }
    listener->Close();
    server.CloseAll();
    MARLIN_EXPECT(server.Open.Load() == 0);
    loop.Stop();
}

namespace
{
/// Bytes a referenced send points into; counts its releases.
struct HeldBytes : RefCounted
{
    explicit HeldBytes(List<uint8> bytes, Atomic<uint32> &released) : Bytes(Move(bytes)), Released(released)
    {
    }

    ~HeldBytes() override
    {
        Released.FetchAdd(1);
    }

    List<uint8> Bytes;
    Atomic<uint32> &Released;
};

String TempPath(StringView name)
{
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    StringView path(buffer, length);
    usize slash = path.Length();
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\')
    {
        --slash;
    }
    return Format("{0}{1}.{2}", path.Left(slash), name, PlatformProcess::CurrentId());
}
} // namespace

MARLIN_TEST(Net, ReferencedAndFileSends)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String path = TempPath("NetSendFile");
    const List<uint8> contents = Pattern(3 << 20, 5);
    {
        const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
        MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, contents.Data(), contents.Count()));
        PlatformFile::Close(file);
    }
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting, FileFlags::Async);
    MARLIN_REQUIRE(file.IsValid());

    // Owned, referenced and file parts in one queue, with a short buffer right before a file region as its head.
    Atomic<uint32> released{0};
    const List<uint8> owned = Pattern(1000, 1);
    const List<uint8> referenced = Pattern(300000, 2);
    const List<uint8> head = Pattern(32, 3);
    const List<uint8> tail = Pattern(10, 4);
    List<uint8> expected;
    expected.AddRange(owned.AsSpan());
    expected.AddRange(referenced.AsSpan());
    expected.AddRange(contents.AsSpan().Slice(1234, 2 << 20));
    expected.AddRange(head.AsSpan());
    expected.AddRange(contents.AsSpan().Slice(7, 4096));
    expected.AddRange(tail.AsSpan());
    UniquePtr<NetListener> listener = NetListener::ListenTcp(loop, 0, false, [&](SharedPtr<NetStream> stream) {
        stream->Start({});
        stream->Send(owned.AsSpan());
        SharedPtr<HeldBytes> first(new HeldBytes(referenced, released));
        stream->Send(first, first->Bytes.AsSpan());
        MARLIN_EXPECT(stream->SendFile({}, file, 1234, 2 << 20));
        stream->Send(head.AsSpan());
        MARLIN_EXPECT(stream->SendFile({}, file, 7, 4096));
        SharedPtr<HeldBytes> second(new HeldBytes(tail, released));
        stream->Send(second, second->Bytes.AsSpan());
        stream->Close();
    });
    MARLIN_REQUIRE(listener != nullptr);
    SharedPtr<NetStream> client = NetStream::ConnectTcp(loop, "127.0.0.1", listener->GetPort(), 2000);
    MARLIN_REQUIRE(client.IsValid());
    Collector collector;
    collector.Expected = expected.Count();
    client->Start(collector.Handlers());
    MARLIN_REQUIRE(collector.Complete.Wait(10000));
    {
        const ScopeLock lock(collector.Lock);
        MARLIN_EXPECT(Same(collector.Bytes.AsSpan(), expected.AsSpan()));
    }
    MARLIN_EXPECT(collector.Closed.Wait(5000));
    // The referenced bytes are let go once their send completed, not before.
    for (uint32 waited = 0; waited < 2000 && released.Load() != 2; ++waited)
    {
        PlatformThread::Sleep(1);
    }
    MARLIN_EXPECT(released.Load() == 2);
    listener->Close();

    const String name = Format("Marlin.NetFileTest.{0}", PlatformProcess::CurrentId());
    UniquePtr<NetListener> pipes =
        NetListener::ListenPipe(loop, name.AsView(), [](SharedPtr<NetStream> stream) { stream->Start({}); });
    MARLIN_REQUIRE(pipes != nullptr);
    SharedPtr<NetStream> pipe = NetStream::ConnectPipe(loop, name.AsView(), 2000);
    MARLIN_REQUIRE(pipe.IsValid());
    MARLIN_EXPECT(!pipe->SendFile({}, file, 0, 100));
    pipe->Abort();
    pipes->Close();
    PlatformFile::Close(file);
    (void)PlatformFile::Delete(path.CStr());
    loop.Stop();
}

MARLIN_TEST(Net, DatagramsAndPost)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    Event posted;
    loop.Post([&posted, &loop] {
        MARLIN_EXPECT(loop.IsIoThread());
        posted.Set();
    });
    MARLIN_EXPECT(posted.Wait(2000) && !loop.IsIoThread());

    Mutex lock;
    List<uint8> received;
    Event arrived{EventReset::Manual};
    UniquePtr<NetDatagramSocket> listener =
        NetDatagramSocket::Bind(loop, 0, false, [&](const SocketAddress & /*from*/, Span<const uint8> bytes) {
            const ScopeLock scope(lock);
            received.AddRange(bytes);
            if (received.Count() >= 3000)
            {
                arrived.Set();
            }
        });
    MARLIN_REQUIRE(listener != nullptr);
    Event echoed{EventReset::Manual};
    UniquePtr<NetDatagramSocket> sender =
        NetDatagramSocket::Bind(loop, 0, false, [&](const SocketAddress & /*from*/, Span<const uint8> bytes) {
            if (bytes.Count() == 4)
            {
                echoed.Set();
            }
        });
    MARLIN_REQUIRE(sender != nullptr);
    const SocketAddress target = PlatformSocket::LoopbackAddress(listener->GetPort());
    for (uint32 i = 0; i < 3; ++i)
    {
        MARLIN_EXPECT(sender->SendTo(target, Pattern(1000, i).AsSpan()));
    }
    MARLIN_EXPECT(arrived.Wait(2000));
    const uint8 ping[4] = {1, 2, 3, 4};
    MARLIN_EXPECT(listener->SendTo(PlatformSocket::LoopbackAddress(sender->GetPort()), Span<const uint8>(ping, 4)));
    MARLIN_EXPECT(echoed.Wait(2000));
    listener->Close();
    sender->Close();
    loop.Stop();
}
} // namespace Marlin::Tests
