#include "Testing/Testing.h"

#include "Concurrency/Atomic.h"
#include "GlobalStore/GlobalStoreClient.h"
#include "GlobalStoreServer/GlobalStoreServer.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "Net/NetLoop.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
String TestDirectory(StringView name)
{
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    StringView path(buffer, length);
    usize slash = path.Length();
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\')
    {
        --slash;
    }
    const String directory = Format("{0}GlobalStoreTests/{1}", path.Left(slash), name);
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    (void)PlatformFile::CreateDirectory(Format("{0}GlobalStoreTests", path.Left(slash)).CStr());
    return directory;
}

Hash128 KeyOf(uint32 index)
{
    return Xxh3Hash128(&index, sizeof(index), 0x5EED);
}

List<uint8> ValueOf(uint32 index, usize size)
{
    List<uint8> value;
    value.ResizeUninitialized(size);
    for (usize i = 0; i < size; ++i)
    {
        value[i] = static_cast<uint8>((i * 31 + index) & 0xFF);
    }
    return value;
}

bool Same(Span<const uint8> a, Span<const uint8> b)
{
    return a.Count() == b.Count() && (a.IsEmpty() || MemCompare(a.Data(), b.Data(), a.Count()) == 0);
}

GlobalStoreServerDesc LocalDesc(const String &directory)
{
    GlobalStoreServerDesc desc;
    desc.DataDirectory = directory;
    return desc;
}
} // namespace

MARLIN_TEST(GlobalStore, PutGetHasAndPersistence)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("Basic");
    const List<uint8> small = ValueOf(1, 100);
    const List<uint8> large = ValueOf(2, 3 << 20);
    {
        GlobalStoreServer server(loop, LocalDesc(directory));
        MARLIN_REQUIRE(server.Start());
        GlobalStoreClient client(loop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        List<uint8> value;
        MARLIN_EXPECT(!client.Get(KeyOf(1), value) && client.IsConnected());
        MARLIN_EXPECT(!client.Has(KeyOf(1)));
        MARLIN_REQUIRE(client.Put(KeyOf(1), small.AsSpan()) && client.Put(KeyOf(2), large.AsSpan()));
        MARLIN_EXPECT(client.Put(KeyOf(1), small.AsSpan()));
        uint64 size = 0;
        MARLIN_EXPECT(client.Has(KeyOf(2), &size) && size == large.Count());
        MARLIN_REQUIRE(client.Get(KeyOf(2), value));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan()));
        MARLIN_REQUIRE(client.Get(KeyOf(2), value, 1000, 64));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan().Slice(1000, 64)));
        MARLIN_REQUIRE(client.Get(KeyOf(1), value, 90));
        MARLIN_EXPECT(Same(value.AsSpan(), small.AsSpan().Slice(90, 10)));
        const GlobalStoreStats stats = server.GetStats();
        MARLIN_EXPECT(stats.Values == 2 && stats.StoredBytes == small.Count() + large.Count());

        GlobalStoreServer second(loop, LocalDesc(directory));
        MARLIN_EXPECT(!second.Start());
    }
    {
        GlobalStoreServer server(loop, LocalDesc(directory));
        MARLIN_REQUIRE(server.Start());
        MARLIN_EXPECT(server.GetStats().Values == 2);
        GlobalStoreClient client(loop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        List<uint8> value;
        MARLIN_REQUIRE(client.Get(KeyOf(1), value));
        MARLIN_EXPECT(Same(value.AsSpan(), small.AsSpan()));
        MARLIN_REQUIRE(client.Get(KeyOf(2), value));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan()));
    }
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(GlobalStore, PipelinedBatchesAndEviction)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("Batches");
    GlobalStoreServerDesc desc = LocalDesc(directory);
    desc.MemoryBudget = 64 << 10;
    GlobalStoreServer server(loop, Move(desc));
    MARLIN_REQUIRE(server.Start());
    GlobalStoreClient writer(loop);
    GlobalStoreClient reader(loop);
    MARLIN_REQUIRE(writer.Connect("127.0.0.1", server.GetPort()) && reader.Connect("127.0.0.1", server.GetPort()));
    constexpr uint32 Count = 1000;
    List<Hash128> keys;
    for (uint32 i = 0; i < Count; ++i)
    {
        MARLIN_REQUIRE(writer.Put(KeyOf(i), ValueOf(i, 1000 + i).AsSpan()));
        keys.Add(KeyOf(i));
    }
    keys.Add(KeyOf(Count));
    MARLIN_EXPECT(server.GetStats().CachedBytes <= 64 << 10);

    List<List<uint8>> values;
    List<bool> found;
    MARLIN_REQUIRE(reader.GetMany(keys.AsSpan(), values, found));
    bool same = true;
    for (uint32 i = 0; i < Count; ++i)
    {
        same = same && found[i] && Same(values[i].AsSpan(), ValueOf(i, 1000 + i).AsSpan());
    }
    MARLIN_EXPECT(same && !found[Count]);
    MARLIN_REQUIRE(reader.HasMany(keys.AsSpan(), found));
    MARLIN_EXPECT(found[0] && found[Count - 1] && !found[Count]);
    MARLIN_EXPECT(server.GetStats().CachedBytes <= 64 << 10);
    server.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(GlobalStore, ColdReads)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String directory = TestDirectory("Cold");
    constexpr uint32 Count = 200;
    const List<uint8> large = ValueOf(Count, 3 << 20);
    {
        GlobalStoreServer server(loop, LocalDesc(directory));
        MARLIN_REQUIRE(server.Start());
        GlobalStoreClient client(loop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        for (uint32 i = 0; i < Count; ++i)
        {
            MARLIN_REQUIRE(client.Put(KeyOf(i), ValueOf(i, 100 + i * 37).AsSpan()));
        }
        MARLIN_REQUIRE(client.Put(KeyOf(Count), large.AsSpan()));
        MARLIN_REQUIRE(client.Put(KeyOf(Count + 1), {}));
    }

    // After a restart nothing is in memory: small values are read with overlapped I/O, then kept.
    {
        GlobalStoreServer server(loop, LocalDesc(directory));
        MARLIN_REQUIRE(server.Start());
        MARLIN_EXPECT(server.GetStats().CachedBytes == 0);
        GlobalStoreClient client(loop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        uint64 size = 0;
        MARLIN_EXPECT(client.Has(KeyOf(5), &size) && size == 100 + 5 * 37);
        MARLIN_EXPECT(server.GetStats().CachedBytes == 0);
        List<uint8> value;
        MARLIN_REQUIRE(client.Get(KeyOf(5), value, 10, 20));
        MARLIN_EXPECT(Same(value.AsSpan(), ValueOf(5, 100 + 5 * 37).AsSpan().Slice(10, 20)));
        MARLIN_EXPECT(server.GetStats().CachedBytes == 100 + 5 * 37);
        MARLIN_REQUIRE(client.Get(KeyOf(5), value));
        MARLIN_EXPECT(Same(value.AsSpan(), ValueOf(5, 100 + 5 * 37).AsSpan()));
        MARLIN_REQUIRE(client.Get(KeyOf(Count + 1), value) && value.IsEmpty());

        // A large value goes from the segment file to the socket and stays out of memory.
        MARLIN_REQUIRE(client.Get(KeyOf(Count), value));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan()));
        MARLIN_REQUIRE(client.Get(KeyOf(Count), value, (2 << 20) + 3, 65536));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan().Slice((2 << 20) + 3, 65536)));
        MARLIN_REQUIRE(client.Get(KeyOf(Count), value, (3 << 20) - 5));
        MARLIN_EXPECT(Same(value.AsSpan(), large.AsSpan().Slice((3 << 20) - 5, 5)));
        MARLIN_EXPECT(server.GetStats().CachedBytes == 100 + 5 * 37);
    }

    // With almost no memory every read is cold; pipelined, the answers still come back in order.
    {
        GlobalStoreServerDesc desc = LocalDesc(directory);
        desc.MemoryBudget = 256;
        GlobalStoreServer server(loop, Move(desc));
        MARLIN_REQUIRE(server.Start());
        GlobalStoreClient client(loop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        List<Hash128> keys;
        for (uint32 i = 0; i <= Count; ++i)
        {
            keys.Add(KeyOf(i));
        }
        keys.Add(KeyOf(Count + 2));
        for (uint32 round = 0; round < 2; ++round)
        {
            List<List<uint8>> values;
            List<bool> found;
            MARLIN_REQUIRE(client.GetMany(keys.AsSpan(), values, found));
            bool same = true;
            for (uint32 i = 0; i < Count; ++i)
            {
                same = same && found[i] && Same(values[i].AsSpan(), ValueOf(i, 100 + i * 37).AsSpan());
            }
            MARLIN_EXPECT(same && found[Count] && Same(values[Count].AsSpan(), large.AsSpan()) && !found[Count + 1]);
        }
        MARLIN_EXPECT(server.GetStats().CachedBytes <= 256);
    }
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

namespace
{
usize SizeOf(uint32 index)
{
    return 200 + (index % 50) * 61;
}

struct PutGetWorker
{
    uint16 Port = 0;
    uint32 Seed = 0;
    NetLoop *Loop = nullptr;
    Atomic<uint32> *Failures = nullptr;

    static void Run(void *argument)
    {
        auto &self = *static_cast<PutGetWorker *>(argument);
        GlobalStoreClient client(*self.Loop);
        if (!client.Connect("127.0.0.1", self.Port))
        {
            self.Failures->FetchAdd(1);
            return;
        }
        // Every worker stores the same keys in its own order, so Puts of one key race and Gets race with them.
        List<uint8> value;
        for (uint32 n = 0; n < 1000; ++n)
        {
            const uint32 i = (n * 7919 + self.Seed * 131) % 1000;
            const List<uint8> expected = ValueOf(i, SizeOf(i));
            if (!client.Put(KeyOf(i), expected.AsSpan()) || !client.Get(KeyOf(i), value) ||
                !Same(value.AsSpan(), expected.AsSpan()))
            {
                self.Failures->FetchAdd(1);
            }
            const uint32 other = (i + 500) % 1000;
            if (client.Get(KeyOf(other), value) && !Same(value.AsSpan(), ValueOf(other, SizeOf(other)).AsSpan()))
            {
                self.Failures->FetchAdd(1);
            }
        }
    }
};
} // namespace

MARLIN_TEST(GlobalStore, ConcurrentPutGet)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(4));
    const String directory = TestDirectory("Concurrent");
    GlobalStoreServerDesc desc = LocalDesc(directory);
    desc.MemoryBudget = 256 << 10;
    GlobalStoreServer server(loop, Move(desc));
    MARLIN_REQUIRE(server.Start());

    constexpr uint32 Workers = 8;
    Atomic<uint32> failures{0};
    PutGetWorker workers[Workers];
    List<ThreadHandle> threads;
    for (uint32 i = 0; i < Workers; ++i)
    {
        workers[i].Port = server.GetPort();
        workers[i].Seed = i;
        workers[i].Loop = &loop;
        workers[i].Failures = &failures;
        ThreadDesc thread;
        thread.Name = "GlobalStore Test";
        thread.Entry = &PutGetWorker::Run;
        thread.Argument = &workers[i];
        threads.Add(PlatformThread::Create(thread));
        MARLIN_REQUIRE(threads.Last().IsValid());
    }
    for (const ThreadHandle &thread : threads)
    {
        PlatformThread::Join(thread);
    }
    MARLIN_EXPECT(failures.Load() == 0);
    const GlobalStoreStats stats = server.GetStats();
    MARLIN_EXPECT(stats.Values == 1000 && stats.CachedBytes <= 256 << 10);

    // Each key was written to the segment once, whichever Put won: one 40-byte record header per value.
    server.Stop();
    uint64 stored = 0;
    for (uint32 i = 0; i < 1000; ++i)
    {
        stored += 40 + SizeOf(i);
    }
    FileInfo info;
    MARLIN_REQUIRE(PlatformFile::GetInfo(Format("{0}/Segment.0.mgs", directory).CStr(), info));
    MARLIN_EXPECT(info.Size == stored);
    GlobalStoreServer restarted(loop, LocalDesc(directory));
    MARLIN_REQUIRE(restarted.Start());
    MARLIN_EXPECT(restarted.GetStats().Values == 1000);
    restarted.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(GlobalStore, Upstream)
{
    NetLoop loop;
    MARLIN_REQUIRE(loop.Start(2));
    const String studioDirectory = TestDirectory("Studio");
    const String localDirectory = TestDirectory("Local");
    GlobalStoreServer studio(loop, LocalDesc(studioDirectory));
    MARLIN_REQUIRE(studio.Start());
    GlobalStoreServerDesc desc = LocalDesc(localDirectory);
    desc.UpstreamHost = "127.0.0.1";
    desc.UpstreamPort = studio.GetPort();
    GlobalStoreServer local(loop, Move(desc));
    MARLIN_REQUIRE(local.Start());

    GlobalStoreClient studioClient(loop);
    GlobalStoreClient localClient(loop);
    MARLIN_REQUIRE(studioClient.Connect("127.0.0.1", studio.GetPort()) &&
                   localClient.Connect("127.0.0.1", local.GetPort()));
    MARLIN_REQUIRE(studioClient.Put(KeyOf(10), ValueOf(10, 5000).AsSpan()));
    List<uint8> value;
    MARLIN_REQUIRE(localClient.Get(KeyOf(10), value));
    MARLIN_EXPECT(Same(value.AsSpan(), ValueOf(10, 5000).AsSpan()));
    MARLIN_EXPECT(local.GetStats().UpstreamHits == 1 && local.GetStats().Values == 1);
    MARLIN_REQUIRE(localClient.Get(KeyOf(10), value, 10, 10));
    MARLIN_EXPECT(local.GetStats().UpstreamHits == 1);

    MARLIN_REQUIRE(localClient.Put(KeyOf(11), ValueOf(11, 700).AsSpan()));
    for (uint32 waited = 0; waited < 2000 && !studioClient.Has(KeyOf(11)); ++waited)
    {
        PlatformThread::Sleep(1);
    }
    MARLIN_REQUIRE(studioClient.Get(KeyOf(11), value));
    MARLIN_EXPECT(Same(value.AsSpan(), ValueOf(11, 700).AsSpan()));
    MARLIN_EXPECT(!localClient.Get(KeyOf(12), value));
    local.Stop();
    studio.Stop();
    (void)PlatformFile::DeleteDirectory(studioDirectory.CStr(), true);
    (void)PlatformFile::DeleteDirectory(localDirectory.CStr(), true);
}
} // namespace Marlin::Tests
