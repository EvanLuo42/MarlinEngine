#include "Testing/Testing.h"

#include "Concurrency/Atomic.h"
#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
/// A scratch directory under /Temp, mounted at *point* by the tests that want it, and removed afterwards.
struct ScratchDirectory
{
    String Physical;

    explicit ScratchDirectory(StringView name)
    {
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", path));
        path.Append('/').Append(name);
        Physical = path.AsView();
        (void)PlatformFile::DeleteDirectory(Physical.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(Physical.CStr()));
    }

    ~ScratchDirectory()
    {
        (void)PlatformFile::DeleteDirectory(Physical.CStr(), true);
    }

    MARLIN_NONCOPYABLE(ScratchDirectory);

    void Write(StringView relative, StringView text) const
    {
        StringBuilder<512> path(Physical.AsView());
        path.Append('/').Append(relative);
        StringView parent;
        StringView name;
        if (path.AsView().SplitLast('/', parent, name))
        {
            (void)PlatformFile::CreateDirectory(String(parent).CStr());
        }
        const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
        MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, text.Data(), text.Length()));
        PlatformFile::Close(file);
    }
};

String Read(StringView path)
{
    String text;
    return Vfs::ReadAll(path, text) ? text : String("<missing>");
}

/// Bytes in memory as a region source, counting the reads that reach it.
class MemorySource final : public IRegionSource
{
public:
    explicit MemorySource(List<uint8> bytes) : _bytes(Move(bytes))
    {
    }

    void Read(uint64 offset, uint64 size, uint8 *buffer, Function<void(bool succeeded)> done) const override
    {
        Reads.FetchAdd(1);
        const bool inside = offset <= _bytes.Count() && size <= _bytes.Count() - offset;
        if (inside)
        {
            MemCopy(buffer, _bytes.Data() + offset, static_cast<usize>(size));
        }
        done(inside);
    }

    mutable Atomic<uint32> Reads{0};

private:
    List<uint8> _bytes;
};

/// Blocks of BlockBytes logical bytes, each stored inverted, so every one must go through Decompress.
class InvertedBlocks final : public IRegionCompression
{
public:
    static constexpr uint32 BlockBytes = 64;

    explicit InvertedBlocks(uint64 logicalSize) : _logicalSize(logicalSize)
    {
    }

    [[nodiscard]] uint32 GetBlockSize() const override
    {
        return BlockBytes;
    }

    bool GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const override
    {
        if (index * BlockBytes >= _logicalSize)
        {
            return false;
        }
        offset = index * BlockBytes;
        const uint64 rest = _logicalSize - offset;
        logicalSize = static_cast<uint32>(rest < BlockBytes ? rest : BlockBytes);
        storedSize = logicalSize;
        return true;
    }

    [[nodiscard]] bool IsPlain(uint64 /*index*/) const override
    {
        return false;
    }

    bool Decompress(uint64 /*index*/, Span<const uint8> stored, Span<uint8> output) const override
    {
        Decompressed.FetchAdd(1);
        for (usize i = 0; i < output.Count(); ++i)
        {
            output[i] = static_cast<uint8>(~stored[i]);
        }
        return stored.Count() == output.Count();
    }

    [[nodiscard]] SharedPtr<const CachedRegionBlock> FindCachedBlock(uint64 index) const override
    {
        return _cache.Find(index);
    }

    void CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const override
    {
        _cache.Add(index, Move(block));
    }

    mutable Atomic<uint32> Decompressed{0};

private:
    uint64 _logicalSize;
    mutable RegionBlockCache _cache;
};
} // namespace

MARLIN_TEST(Vfs, Normalize)
{
    const auto normal = [](StringView path) {
        StringBuilder<128> output;
        return Vfs::Normalize(path, output) ? output.ToString() : String("<invalid>");
    };
    MARLIN_EXPECT(normal("/Engine/Config/") == "/Engine/Config");
    MARLIN_EXPECT(normal("\\Engine\\\\Config\\.\\Engine.mtext") == "/Engine/Config/Engine.mtext");
    MARLIN_EXPECT(normal("/") == "/");
    MARLIN_EXPECT(normal("Engine/Config") == "<invalid>");
    MARLIN_EXPECT(normal("/Engine/../Secrets") == "<invalid>");
}

MARLIN_TEST(Vfs, LayersAndPriorities)
{
    const ScratchDirectory base("VfsBase");
    const ScratchDirectory overlay("VfsOverlay");
    const ScratchDirectory inner("VfsInner");
    base.Write("a.txt", "base a");
    base.Write("b.txt", "base b");
    base.Write("Sub/c.txt", "base c");
    overlay.Write("a.txt", "overlay a");
    inner.Write("c.txt", "inner c");

    const MountId baseId = Vfs::Mount("/VfsTest", MakeUnique<DirectoryMount>(base.Physical, false), 0);
    const MountId overlayId = Vfs::Mount("/VfsTest", MakeUnique<DirectoryMount>(overlay.Physical, true), 10);
    const MountId innerId = Vfs::Mount("/VfsTest/Sub", MakeUnique<DirectoryMount>(inner.Physical, false), -5);
    MARLIN_REQUIRE(baseId != 0 && overlayId != 0 && innerId != 0 && Vfs::IsMounted("/VfsTest/"));

    MARLIN_EXPECT(Read("/VfsTest/a.txt") == "overlay a");
    MARLIN_EXPECT(Read("/VfsTest/b.txt") == "base b");
    MARLIN_EXPECT(Read("//VfsTest\\Sub/c.txt") == "inner c");
    MARLIN_EXPECT(!Vfs::Exists("/VfsTest/missing.txt") && !Vfs::Exists("/VfsTesting/a.txt"));

    String layers;
    Vfs::ReadEachLayer("/VfsTest/a.txt",
                       [&](StringView text, StringView /*source*/) { layers.Append(text).Append(';'); });
    MARLIN_EXPECT(layers == "base a;overlay a;");

    int32 entries = 0;
    bool sawSub = false;
    Vfs::Enumerate("/VfsTest", [&](StringView name, const FileInfo &info) {
        ++entries;
        sawSub = sawSub || (name == "Sub" && info.IsDirectory);
    });
    MARLIN_EXPECT(entries == 3 && sawSub);

    MARLIN_EXPECT(Vfs::WriteAll("/VfsTest/New/d.txt", "written"));
    MARLIN_EXPECT(Read("/VfsTest/New/d.txt") == "written");
    MARLIN_EXPECT(PlatformFile::FileExists((overlay.Physical + "/New/d.txt").CStr()));
    MARLIN_EXPECT(!Vfs::WriteAll("/VfsTest/Sub/e.txt", "read-only"));
    MARLIN_EXPECT(Vfs::Delete("/VfsTest/New/d.txt") && !Vfs::Exists("/VfsTest/New/d.txt"));
    MARLIN_EXPECT(!Vfs::Delete("/VfsTest") && !Vfs::Delete("/VfsTest/") &&
                  PlatformFile::DirectoryExists(overlay.Physical.CStr()));

    StringBuilder<512> physical;
    MARLIN_EXPECT(Vfs::GetPhysicalPath("/VfsTest/b.txt", physical) && physical.AsView().EndsWith("VfsBase/b.txt"));

    MARLIN_EXPECT(Vfs::Unmount(overlayId) && Read("/VfsTest/a.txt") == "base a");
    MARLIN_EXPECT(Vfs::Unmount(baseId) && Vfs::Unmount(innerId) && !Vfs::IsMounted("/VfsTest"));
}

MARLIN_TEST(Vfs, WritesIntoAChosenLayer)
{
    const ScratchDirectory base("VfsLayerBase");
    const ScratchDirectory overlay("VfsLayerOverlay");
    base.Write("Content/Icon.png", "base icon");
    const MountId baseId = Vfs::Mount("/VfsLayers", MakeUnique<DirectoryMount>(base.Physical, true), 0);
    const MountId overlayId = Vfs::Mount("/VfsLayers", MakeUnique<DirectoryMount>(overlay.Physical, true), 10);
    MARLIN_REQUIRE(baseId != 0 && overlayId != 0);

    List<MountId> layers;
    Vfs::GetLayers("/VfsLayers/Content/Icon.png", layers);
    MARLIN_EXPECT(layers.Count() == 2 && layers[0] == overlayId && layers[1] == baseId);
    MARLIN_EXPECT(Vfs::FindLayer("/VfsLayers/Content/Icon.png") == baseId);
    MARLIN_EXPECT(Vfs::FindLayer("/VfsLayers/Content/Missing.png") == 0);

    // Beside the files it belongs with, not in the highest writable layer.
    MARLIN_EXPECT(Vfs::WriteAll(baseId, "/VfsLayers/Content/Font.ttf", "font"));
    MARLIN_EXPECT(PlatformFile::FileExists((base.Physical + "/Content/Font.ttf").CStr()));
    MARLIN_EXPECT(!PlatformFile::FileExists((overlay.Physical + "/Content/Font.ttf").CStr()));
    String text;
    MARLIN_EXPECT(Vfs::ReadAll(baseId, "/VfsLayers/Content/Font.ttf", text) && text == "font");
    MARLIN_EXPECT(!Vfs::ReadAll(overlayId, "/VfsLayers/Content/Font.ttf", text));
    FileInfo info;
    MARLIN_EXPECT(Vfs::GetInfo(baseId, "/VfsLayers/Content/Icon.png", info) &&
                  !Vfs::GetInfo(overlayId, "/VfsLayers/Content/Icon.png", info));
    StringBuilder<512> physical;
    MARLIN_EXPECT(Vfs::GetPhysicalPath(overlayId, "/VfsLayers/Content/Icon.png", physical) &&
                  physical.AsView().EndsWith("VfsLayerOverlay/Content/Icon.png"));
    // Opening in a layer that lacks the file does not fall through to the one that has it.
    const FileHandle icon = Vfs::Open(baseId, "/VfsLayers/Content/Icon.png", FileAccess::Read);
    MARLIN_EXPECT(icon.IsValid() && !Vfs::Open(overlayId, "/VfsLayers/Content/Icon.png", FileAccess::Read).IsValid());
    PlatformFile::Close(icon);
    MARLIN_EXPECT(!Vfs::WriteAll(MountId{0xFFFF}, "/VfsLayers/Content/Wrong.txt", "wrong"));
    MARLIN_EXPECT(Vfs::Unmount(overlayId) && Vfs::Unmount(baseId));
}

MARLIN_TEST(Vfs, AsyncReads)
{
    const ScratchDirectory scratch("VfsAsync");
    String small;
    for (int32 i = 0; i < 1000; ++i)
    {
        small.Append(static_cast<char>('a' + i % 26));
    }
    scratch.Write("small.txt", small);
    String large;
    char *data = large.ResizeUninitialized((usize{33} << 20) + 123);
    for (usize i = 0; i < large.Length(); ++i)
    {
        data[i] = static_cast<char>(i * 7 + (i >> 20));
    }
    scratch.Write("large.bin", large);
    const MountId id = Vfs::Mount("/AsyncTest", MakeUnique<DirectoryMount>(scratch.Physical, false));

    int32 callbacks = 0;
    const AsyncReadRequest whole =
        Vfs::ReadAsync("/AsyncTest/small.txt", IoPriority::Critical, 0, Vfs::WholeFile,
                       [&](const AsyncReadRequest &request) { callbacks += request.Succeeded() ? 1 : 100; });
    const AsyncReadRequest part = Vfs::ReadAsync("/AsyncTest/small.txt", IoPriority::Background, 26, 10);
    const AsyncReadRequest big = Vfs::ReadAsync("/AsyncTest/large.bin");
    const AsyncReadRequest missing = Vfs::ReadAsync("/AsyncTest/missing.bin");
    MARLIN_REQUIRE(whole.Wait(10000) && part.Wait(10000) && big.Wait(10000) && missing.Wait(0));

    MARLIN_EXPECT(whole.Succeeded() && whole.GetData().Count() == small.Length() && callbacks == 1);
    MARLIN_EXPECT(StringView(reinterpret_cast<const char *>(whole.GetData().Data()), 26) == small.AsView().Left(26));
    MARLIN_EXPECT(part.Succeeded() && StringView(reinterpret_cast<const char *>(part.GetData().Data()), 10) ==
                                          small.AsView().Substring(26, 10));
    MARLIN_REQUIRE(big.Succeeded() && big.GetData().Count() == large.Length());
    bool same = true;
    for (usize i = 0; i < large.Length(); i += 4093)
    {
        same = same && big.GetData()[i] == static_cast<uint8>(large[i]);
    }
    MARLIN_EXPECT(same && big.GetData()[large.Length() - 1] == static_cast<uint8>(large[large.Length() - 1]));
    MARLIN_EXPECT(missing.IsDone() && !missing.Succeeded() && missing.GetData().IsEmpty());
    MARLIN_EXPECT(Vfs::Unmount(id));
}

MARLIN_TEST(Vfs, SourceReadsWithCompression)
{
    // 1000 logical bytes in blocks of 64, stored inverted, served from memory instead of a file.
    List<uint8> logical;
    List<uint8> stored;
    for (usize i = 0; i < 1000; ++i)
    {
        logical.Add(static_cast<uint8>(i * 13 + 5));
        stored.Add(static_cast<uint8>(~logical[i]));
    }
    const SharedPtr<MemorySource> source(new MemorySource(Move(stored)));
    const SharedPtr<InvertedBlocks> blocks(new InvertedBlocks(logical.Count()));
    FileRegion region;
    region.Size = logical.Count();
    region.Source = SharedPtr<const IRegionSource>(source.Get());
    region.Compression = SharedPtr<const IRegionCompression>(blocks.Get());

    // Starts and ends inside a block: the first and last are wanted only in part and are kept.
    const AsyncReadRequest middle = Vfs::ReadAsync(region, IoPriority::Critical, 100, 300);
    MARLIN_REQUIRE(middle.Wait(5000) && middle.Succeeded() && middle.GetData().Count() == 300);
    MARLIN_EXPECT(MemCompare(middle.GetData().Data(), logical.Data() + 100, 300) == 0);
    MARLIN_EXPECT(source->Reads.Load() == 1);
    MARLIN_EXPECT(blocks->FindCachedBlock(1).IsValid() && blocks->FindCachedBlock(6).IsValid());

    // A neighbour inside the kept block reads nothing and decompresses nothing.
    const uint32 decompressed = blocks->Decompressed.Load();
    const AsyncReadRequest neighbour = Vfs::ReadAsync(region, IoPriority::Critical, 70, 20);
    MARLIN_REQUIRE(neighbour.Wait(5000) && neighbour.Succeeded());
    MARLIN_EXPECT(MemCompare(neighbour.GetData().Data(), logical.Data() + 70, 20) == 0);
    MARLIN_EXPECT(source->Reads.Load() == 1 && blocks->Decompressed.Load() == decompressed);

    // The whole stream, enough blocks to decompress on several workers.
    const AsyncReadRequest whole = Vfs::ReadAsync(region, IoPriority::Streaming);
    MARLIN_REQUIRE(whole.Wait(5000) && whole.Succeeded() && whole.GetData().Count() == logical.Count());
    MARLIN_EXPECT(MemCompare(whole.GetData().Data(), logical.Data(), logical.Count()) == 0);

    // A source that fails fails the read.
    FileRegion outside = region;
    outside.Compression.Reset();
    const AsyncReadRequest failed = Vfs::ReadAsync(outside, IoPriority::Critical, 990, 20);
    MARLIN_REQUIRE(failed.Wait(5000));
    MARLIN_EXPECT(failed.Succeeded() && failed.GetData().Count() == 10);
    outside.Size = 2000;
    const AsyncReadRequest beyond = Vfs::ReadAsync(outside, IoPriority::Critical, 990, 20);
    MARLIN_REQUIRE(beyond.Wait(5000));
    MARLIN_EXPECT(!beyond.Succeeded());
}
} // namespace Marlin::Tests
