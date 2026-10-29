#include "Testing/Testing.h"

#include "GlobalStoreAssetStore/GlobalStoreAssetStore.h"
#include "GlobalStoreServer/GlobalStoreServer.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSync.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"

namespace Marlin::Tests
{
namespace
{
List<uint8> Pattern(usize size, uint32 seed)
{
    List<uint8> bytes;
    bytes.ResizeUninitialized(size);
    for (usize i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8>((i * 31 + seed) ^ (i >> 9));
    }
    return bytes;
}

/// A GlobalStore server in this process on its own data directory under /Temp, and a connection to it.
struct StoreFixture
{
    NetLoop Loop;
    UniquePtr<GlobalStoreServer> Server;
    SharedPtr<GlobalStoreConnection> Connection;
    String Directory;

    StoreFixture()
    {
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/GlobalStoreAssetStoreTest", physical));
        Directory = physical.AsView();
        (void)PlatformFile::DeleteDirectory(Directory.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(Directory.CStr()));
        MARLIN_REQUIRE(Loop.Start(2));
        GlobalStoreServerDesc desc;
        desc.DataDirectory = Directory;
        Server = MakeUnique<GlobalStoreServer>(Loop, desc);
        MARLIN_REQUIRE(Server->Start());
        Connection = SharedPtr<GlobalStoreConnection>(new GlobalStoreConnection(Loop));
        MARLIN_REQUIRE(Connection->Client.Connect("127.0.0.1", Server->GetPort()));
    }

    ~StoreFixture()
    {
        Connection->Client.Disconnect();
        Connection.Reset();
        Server->Stop();
        Server.Reset();
        Loop.Stop();
        (void)PlatformFile::DeleteDirectory(Directory.CStr(), true);
    }

    MARLIN_NONCOPYABLE(StoreFixture);

    /// Opens *key* and waits for the answer; an invalid region if it could not be opened.
    FileRegion Open(const Hash128 &key, const CompressionCodecSet &codecs)
    {
        Event done(EventReset::Manual);
        FileRegion opened;
        OpenConvertedResult(Connection, key, codecs, [&](const FileRegion *stream) {
            if (stream != nullptr)
            {
                opened = *stream;
            }
            done.Set();
        });
        MARLIN_REQUIRE(done.Wait(10000));
        return opened;
    }
};
} // namespace

MARLIN_TEST(GlobalStoreAssetStore, ReadsResultsFromGlobalStore)
{
    StoreFixture fixture;
    CompressionCodecSet codecs;
    codecs.Build({});
    // A small asset block and bulk larger than one Get segment, so its reads are cut and pipelined.
    const List<uint8> block = Pattern(3000, 1);
    const List<uint8> bulk = Pattern(static_cast<usize>(GlobalStoreRegionSource::SegmentSize * 3 / 2), 7);
    const Hash128 key{0x1234, 0x5678};
    List<uint8> value;
    ConvertedResultHeader header;
    ConvertedResultWriter::Write(key, block.AsSpan(), bulk.AsSpan(), nullptr, 0, value, &header);
    MARLIN_REQUIRE(fixture.Connection->Client.Put(key, value.AsSpan()));

    const FileRegion stream = fixture.Open(key, codecs);
    MARLIN_REQUIRE(stream.Source.IsValid() && stream.Compression.IsValid() && !stream.File.IsValid());
    MARLIN_EXPECT(stream.Size == header.LogicalSize);

    ConvertedResultLocation located;
    located.Key = FormatResultKey(key);
    located.BlockOffset = header.AssetBlockOffset;
    located.BlockSize = header.AssetBlockSize;
    located.BulkOffset = header.BulkOffset;
    located.BulkSize = header.BulkSize;
    AssetLocation location;
    MARLIN_REQUIRE(MakeAssetLocation(stream, located, location));

    // The asset block came with the header and table, so its read needs nothing more from the server.
    const GlobalStoreStats before = fixture.Server->GetStats();
    const AsyncReadRequest readBlock = Vfs::ReadAsync(location.Block, IoPriority::Critical);
    MARLIN_REQUIRE(readBlock.Wait(10000) && readBlock.Succeeded());
    MARLIN_EXPECT(readBlock.GetData().Count() == block.Count() &&
                  MemCompare(readBlock.GetData().Data(), block.Data(), block.Count()) == 0);
    MARLIN_EXPECT(fixture.Server->GetStats().Requests == before.Requests);

    // A part of the bulk, and all of it.
    const AsyncReadRequest part = Vfs::ReadAsync(location.Bulk, IoPriority::Streaming, 70000, 5000);
    MARLIN_REQUIRE(part.Wait(10000) && part.Succeeded());
    MARLIN_EXPECT(MemCompare(part.GetData().Data(), bulk.Data() + 70000, 5000) == 0);
    const AsyncReadRequest whole = Vfs::ReadAsync(location.Bulk, IoPriority::Streaming);
    MARLIN_REQUIRE(whole.Wait(10000) && whole.Succeeded() && whole.GetData().Count() == bulk.Count());
    MARLIN_EXPECT(MemCompare(whole.GetData().Data(), bulk.Data(), bulk.Count()) == 0);

    // A key GlobalStore does not have, and a value that is not a converted result, open to nothing.
    MARLIN_EXPECT(!fixture.Open(Hash128{1, 2}, codecs).IsValid());
    const Hash128 junk{3, 4};
    MARLIN_REQUIRE(fixture.Connection->Client.Put(junk, block.AsSpan()));
    MARLIN_EXPECT(!fixture.Open(junk, codecs).IsValid());

    // The result key's text round trip, as the indexer writes it.
    Hash128 parsed;
    MARLIN_EXPECT(ParseResultKey(FormatResultKey(key).AsView(), parsed) && parsed == key);
    MARLIN_EXPECT(!ParseResultKey("not a key", parsed));
}
} // namespace Marlin::Tests
