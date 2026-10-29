#include "Testing/Testing.h"

#include "GlobalStore/GlobalStoreClient.h"
#include "GlobalStoreServer/GlobalStoreServer.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Hash/Xxh3.h"
#include "Net/NetLoop.h"
#include "String/Format.h"

namespace Marlin::Tests
{
namespace
{
constexpr uint32 SmallCount = 4096;
constexpr usize SmallSize = 4096;
constexpr uint32 LargeCount = 64;
constexpr usize LargeSize = usize{4} << 20;
constexpr usize BlockSize = usize{64} << 10;
constexpr usize Batch = 64;

String BenchmarkDirectory()
{
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    StringView path(buffer, length);
    usize slash = path.Length();
    while (slash > 0 && path[slash - 1] != '/' && path[slash - 1] != '\\')
    {
        --slash;
    }
    const String directory = Format("{0}GlobalStoreBenchmark", path.Left(slash));
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    return directory;
}

Hash128 SmallKey(uint32 index)
{
    return Xxh3Hash128(&index, sizeof(index), 0xB5);
}

Hash128 LargeKey(uint32 index)
{
    return Xxh3Hash128(&index, sizeof(index), 0xB6);
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

void Report(StringView name, float64 seconds, float64 count, StringView unit)
{
    Testing::Print(Format("{0,-52} {1,10:F2} ms {2,12:F0} {3}\n", name, seconds * 1000, count / seconds, unit));
}

void ReportBytes(StringView name, float64 seconds, float64 bytes)
{
    Testing::Print(Format("{0,-52} {1,10:F2} ms {2,12:F2} GB/s\n", name, seconds * 1000, bytes / seconds / 1e9));
}

/// Gets 4 KiB values on its own connection, one at a time or in pipelined batches.
struct GetWorker
{
    NetLoop *Loop = nullptr;
    uint16 Port = 0;
    uint32 Seed = 0;
    uint32 Gets = 0;
    bool Pipelined = false;
    uint32 Failures = 0;

    static void Run(void *argument)
    {
        auto &self = *static_cast<GetWorker *>(argument);
        GlobalStoreClient client(*self.Loop);
        if (!client.Connect("127.0.0.1", self.Port))
        {
            self.Failures = self.Gets;
            return;
        }
        List<uint8> value;
        List<Hash128> keys;
        List<List<uint8>> values;
        List<bool> found;
        for (uint32 n = 0; n < self.Gets;)
        {
            if (!self.Pipelined)
            {
                self.Failures += client.Get(SmallKey((n * 7919 + self.Seed * 613) % SmallCount), value) ? 0 : 1;
                ++n;
                continue;
            }
            keys.Clear();
            for (usize i = 0; i < Batch && n < self.Gets; ++i, ++n)
            {
                keys.Add(SmallKey((n * 7919 + self.Seed * 613) % SmallCount));
            }
            self.Failures += client.GetMany(keys.AsSpan(), values, found) ? 0 : 1;
        }
    }
};

/// Runs *connections* GetWorkers at once; returns the seconds they took together.
float64 RunGets(NetLoop &loop, uint16 port, uint32 connections, uint32 getsEach, bool pipelined, uint32 &failures)
{
    List<GetWorker> workers;
    workers.Resize(connections);
    List<ThreadHandle> threads;
    const float64 start = PlatformTime::Seconds();
    for (uint32 i = 0; i < connections; ++i)
    {
        workers[i].Loop = &loop;
        workers[i].Port = port;
        workers[i].Seed = i;
        workers[i].Gets = getsEach;
        workers[i].Pipelined = pipelined;
        ThreadDesc desc;
        desc.Name = "GlobalStore Benchmark";
        desc.Entry = &GetWorker::Run;
        desc.Argument = &workers[i];
        threads.Add(PlatformThread::Create(desc));
    }
    for (const ThreadHandle &thread : threads)
    {
        PlatformThread::Join(thread);
    }
    const float64 seconds = PlatformTime::Seconds() - start;
    for (const GetWorker &worker : workers)
    {
        failures += worker.Failures;
    }
    return seconds;
}
} // namespace

/// Documentation/Architecture/06 §3.3: single-thread 4 KiB Gets per second against Guerrilla's ~75,000, throughput
/// over many connections, and reads of values that are only in the segment files. Server and clients each have
/// their own NetLoop, over loopback; "cold" means not in the server's memory, the OS file cache still has it.
MARLIN_TEST(Benchmark, GlobalStore, Benchmark)
{
    const String directory = BenchmarkDirectory();
    NetLoop serverLoop;
    NetLoop clientLoop;
    MARLIN_REQUIRE(serverLoop.Start(2) && clientLoop.Start(4));
    {
        GlobalStoreServerDesc desc;
        desc.DataDirectory = directory;
        GlobalStoreServer server(serverLoop, Move(desc));
        MARLIN_REQUIRE(server.Start());
        GlobalStoreClient client(clientLoop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        float64 start = PlatformTime::Seconds();
        for (uint32 i = 0; i < SmallCount; ++i)
        {
            MARLIN_REQUIRE(client.Put(SmallKey(i), ValueOf(i, SmallSize).AsSpan()));
        }
        Report("4 KiB Put, one connection", PlatformTime::Seconds() - start, SmallCount, "/s");
        for (uint32 i = 0; i < LargeCount; ++i)
        {
            MARLIN_REQUIRE(client.Put(LargeKey(i), ValueOf(i, LargeSize).AsSpan()));
        }

        constexpr uint32 Gets = 50000;
        List<uint8> value;
        start = PlatformTime::Seconds();
        uint32 failures = 0;
        for (uint32 n = 0; n < Gets; ++n)
        {
            failures += client.Get(SmallKey((n * 7919) % SmallCount), value) ? 0 : 1;
        }
        Report("4 KiB Get, one thread, one at a time", PlatformTime::Seconds() - start, Gets, "/s");
        uint32 failed = 0;
        Report("4 KiB Get, one thread, pipelined by 64",
               RunGets(clientLoop, server.GetPort(), 1, Gets * 4, true, failed), Gets * 4.0, "/s");
        for (const uint32 connections : {8u, 32u})
        {
            const float64 single = RunGets(clientLoop, server.GetPort(), connections, Gets / 4, false, failed);
            Report(Format("4 KiB Get, {0} connections, one at a time", connections).AsView(), single,
                   Gets / 4.0 * connections, "/s");
            const float64 piped = RunGets(clientLoop, server.GetPort(), connections, Gets, true, failed);
            Report(Format("4 KiB Get, {0} connections, pipelined by 64", connections).AsView(), piped,
                   static_cast<float64>(Gets) * connections, "/s");
            ReportBytes(Format("4 KiB Get, {0} connections, pipelined by 64", connections).AsView(), piped,
                        static_cast<float64>(Gets) * connections * SmallSize);
        }
        start = PlatformTime::Seconds();
        for (uint32 i = 0; i < LargeCount; ++i)
        {
            failures += client.Get(LargeKey(i), value) && value.Count() == LargeSize ? 0 : 1;
        }
        ReportBytes("4 MiB Get from memory, one thread", PlatformTime::Seconds() - start,
                    static_cast<float64>(LargeCount) * LargeSize);
        MARLIN_EXPECT(failures == 0 && failed == 0);
        server.Stop();
    }

    // Nothing kept in memory: small values are read with overlapped I/O, large ones sent with TransmitFile.
    {
        GlobalStoreServerDesc desc;
        desc.DataDirectory = directory;
        desc.MemoryBudget = 0;
        GlobalStoreServer server(serverLoop, Move(desc));
        MARLIN_REQUIRE(server.Start());
        GlobalStoreClient client(clientLoop);
        MARLIN_REQUIRE(client.Connect("127.0.0.1", server.GetPort()));
        constexpr uint32 Gets = 20000;
        List<uint8> value;
        uint32 failures = 0;
        float64 start = PlatformTime::Seconds();
        for (uint32 n = 0; n < Gets; ++n)
        {
            failures += client.Get(SmallKey((n * 7919) % SmallCount), value) ? 0 : 1;
        }
        Report("Cold 4 KiB Get, one thread, one at a time", PlatformTime::Seconds() - start, Gets, "/s");
        uint32 failed = 0;
        Report("Cold 4 KiB Get, one thread, pipelined by 64",
               RunGets(clientLoop, server.GetPort(), 1, Gets * 4, true, failed), Gets * 4.0, "/s");
        const float64 piped = RunGets(clientLoop, server.GetPort(), 8, Gets, true, failed);
        Report("Cold 4 KiB Get, 8 connections, pipelined by 64", piped, Gets * 8.0, "/s");

        start = PlatformTime::Seconds();
        for (uint32 i = 0; i < LargeCount; ++i)
        {
            failures += client.Get(LargeKey(i), value) && value.Count() == LargeSize ? 0 : 1;
        }
        ReportBytes("Cold 4 MiB Get, one thread (TransmitFile)", PlatformTime::Seconds() - start,
                    static_cast<float64>(LargeCount) * LargeSize);
        constexpr uint32 Blocks = 4096;
        start = PlatformTime::Seconds();
        for (uint32 n = 0; n < Blocks; ++n)
        {
            const uint64 offset = (n * 7919 % (LargeSize / BlockSize)) * BlockSize;
            failures += client.Get(LargeKey(n % LargeCount), value, offset, BlockSize) ? 0 : 1;
        }
        Report("Cold 64 KiB block of a 4 MiB value (TransmitFile)", PlatformTime::Seconds() - start, Blocks, "/s");
        MARLIN_EXPECT(failures == 0 && failed == 0);
        server.Stop();
    }
    clientLoop.Stop();
    serverLoop.Stop();
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}
} // namespace Marlin::Tests
