#include "Testing/Testing.h"

#include <cstddef>

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Asset/PakAssetStore.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetScanner.h"
#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"
#include "Packaging/Packager.h"
#include "Packaging/PakWriter.h"
#include "PackagingTestTypes.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/PakCipher.h"
#include "Vfs/PakFile.h"
#include "Vfs/PakSignature.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
bool TestRleCodec::Compress(Span<const uint8> input, int32 /*level*/, List<uint8> &output) const
{
    // Runs of up to 255 equal bytes, each as its length and the byte.
    output.Clear();
    usize i = 0;
    while (i < input.Count())
    {
        usize run = 1;
        while (i + run < input.Count() && run < 255 && input[i + run] == input[i])
        {
            ++run;
        }
        output.Add(static_cast<uint8>(run));
        output.Add(input[i]);
        i += run;
    }
    return true;
}

bool TestRleCodec::Decompress(Span<const uint8> input, Span<uint8> output) const
{
    usize written = 0;
    for (usize i = 0; i + 1 < input.Count(); i += 2)
    {
        if (input[i] == 0 || written + input[i] > output.Count())
        {
            return false;
        }
        MemSet(output.Data() + written, input[i + 1], input[i]);
        written += input[i];
    }
    return input.Count() % 2 == 0 && written == output.Count();
}

namespace
{
constexpr StringView Content = R"(!Version "1.0"

TestPackagedAsset
{
    !UUID = "9a0c0000-0000-0000-0000-000000000001"
    !Name = "A"
    Value = 7
    Next = <B#9a0c0000-0000-0000-0000-000000000002>
}

TestPackagedAsset
{
    !UUID = "9a0c0000-0000-0000-0000-000000000002"
    !Name = "B"
    Value = 8
}

TestPackagedAsset
{
    !UUID = "9a0c0000-0000-0000-0000-000000000004"
    !Name = "Unused"
    Value = 9
}
)";

constexpr StringView GpuContent = R"(!Version "1.0"

TestGpuAsset
{
    !UUID = "9a0c0000-0000-0000-0000-000000000003"
    !Name = "Gpu"
    Texels = [1, 2, 3, 4, 5]
}
)";

const Guid A = {0x9a0c0000, 0, 0, 1};
const Guid B = {0x9a0c0000, 0, 0, 2};
const Guid Gpu = {0x9a0c0000, 0, 0, 3};
const Guid Unused = {0x9a0c0000, 0, 0, 4};

/// Text-like bytes that compress well, changing every 5000 bytes.
List<uint8> Pattern(usize size)
{
    List<uint8> bytes;
    for (usize i = 0; i < size; ++i)
    {
        bytes.Add(static_cast<uint8>("config "[i % 7] + (i / 5000) % 3));
    }
    return bytes;
}

void WriteNative(const String &path, Span<const uint8> bytes)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
    MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, bytes.Data(), bytes.Count()));
    PlatformFile::Close(file);
}

/// A project of its own in /Temp/PackagingTest, mounted at /PackagingTest. It is created and removed only through
/// the VFS, by its virtual path, so a failed lookup can never turn into a native path to delete.
struct Fixture
{
    String Physical;
    MountId Mount = 0;
    AssetIndex Index;
    UniquePtr<AssetScanner> Scanner;
    CompressionCodecSet Codecs;
    /// A GlobalStore in this process, with its data in the project folder.
    UniquePtr<ConversionStore> Store;

    Fixture()
    {
        (void)Vfs::Delete("/Temp/PackagingTest");
        for (const StringView folder :
             {StringView(""), StringView("/Content"), StringView("/Content/Gpu"), StringView("/Engine"),
              StringView("/Engine/Config"), StringView("/Engine/Platforms"), StringView("/Engine/Platforms/Windows"),
              StringView("/Engine/Platforms/Windows/Config")})
        {
            MARLIN_REQUIRE(Vfs::CreateDirectory(Format("/Temp/PackagingTest{0}", folder).AsView()));
        }
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/PackagingTest", path) && !path.IsEmpty());
        Physical = path.AsView();
        Mount = Vfs::Mount("/PackagingTest", MakeUnique<DirectoryMount>(Physical, true), 0);
        MARLIN_REQUIRE(Vfs::WriteAll("/PackagingTest/Content/Objects.masset", Content));
        MARLIN_REQUIRE(Vfs::WriteAll("/PackagingTest/Content/Gpu/Gpu.masset", GpuContent));
        WriteNative(Format("{0}/Engine/Config/Big.mtext", Physical), Pattern(200000).AsSpan());
        WriteNative(Format("{0}/Engine/Config/Small.mtext", Physical), Pattern(100).AsSpan());
        WriteNative(Format("{0}/Engine/Platforms/Windows/Config/Small.mtext", Physical), Pattern(50).AsSpan());
        List<String> folders;
        folders.Add("/PackagingTest/Content");
        Scanner = MakeUnique<AssetScanner>(Index, folders);
        List<String> changed;
        Scanner->ScanAll(changed);
        Codecs.Build({});
        ConversionStoreDesc store;
        store.InProcessDirectory = Format("{0}/GlobalStore", Physical);
        Store = MakeUnique<ConversionStore>(Move(store));
    }

    ~Fixture()
    {
        Store.Reset();
        Scanner.Reset();
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/PackagingTest");
    }

    MARLIN_NONCOPYABLE(Fixture);

    PackageDesc Describe(StringView output) const
    {
        PackageDesc desc;
        desc.Index = &Index;
        desc.Roots.Add(A);
        desc.RootFolders.Add("/PackagingTest/Content/Gpu");
        desc.Target.Platform = "Windows";
        desc.Store = Store.Get();
        desc.Threads = 2;
        desc.Settings.Compression.FindOrAdd(String("Package")) = {String("TestRle"), 9};
        desc.Settings.Compression.FindOrAdd(String("GpuTexture")) = {String(), 0};
        desc.Files.Add({String("/Engine"), String(), Format("{0}/Engine", Physical), String("Config")});
        desc.Files.Add(
            {String("/Engine"), String("Windows"), Format("{0}/Engine/Platforms/Windows", Physical), String("Config")});
        desc.OutputDirectory = Format("{0}/{1}", Physical, output);
        return desc;
    }
};
} // namespace

MARLIN_TEST(Packaging, WriterAndMount)
{
    CompressionCodecSet codecs;
    codecs.Build({});
    StringBuilder<512> temp;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", temp) && !temp.IsEmpty());
    const String path = Format("{0}/WriterTest.mpak", temp.AsView());
    const List<uint8> big = Pattern(300000);
    StringBuilder<128> error;
    {
        PakWriter writer("Base", codecs.Find("TestRle"), 0, 4096);
        writer.AddFile("/Data", "", "Folder/Big.bin", big.AsSpan());
        writer.AddFile("/Data", "", "Small.bin", Pattern(10).AsSpan());
        writer.AddFile("/Data", "Windows", "Small.bin", Pattern(20).AsSpan());
        MARLIN_REQUIRE(writer.Write(path.AsView(), error));

        const SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), codecs);
        MARLIN_REQUIRE(pak.IsValid());
        MARLIN_EXPECT(pak->GetChunk() == "Base" && pak->GetFiles().Count() == 3);
        MARLIN_REQUIRE(pak->FindFile("/Data", "Windows", "Small.bin") != nullptr);
        MARLIN_EXPECT(pak->FindFile("/Data", "", "Missing.bin") == nullptr);

        const MountId mount = Vfs::Mount("/PakView", MakeUnique<PakMount>(pak, "/Data", ""), 0);
        List<uint8> read;
        MARLIN_REQUIRE(Vfs::ReadAll("/PakView/Folder/Big.bin", read));
        MARLIN_EXPECT(read.Count() == big.Count() && MemCompare(read.Data(), big.Data(), big.Count()) == 0);
        const AsyncReadRequest part = Vfs::ReadAsync("/PakView/Folder/Big.bin", IoPriority::Critical, 4090, 9000);
        MARLIN_REQUIRE(part.Wait(5000) && part.Succeeded());
        MARLIN_EXPECT(part.GetData().Count() == 9000 &&
                      MemCompare(part.GetData().Data(), big.Data() + 4090, 9000) == 0);
        FileInfo info;
        MARLIN_EXPECT(Vfs::GetInfo("/PakView/Folder", info) && info.IsDirectory);
        MARLIN_EXPECT(Vfs::GetInfo("/PakView/Small.bin", info) && info.Size == 10);
        usize entries = 0;
        Vfs::Enumerate("/PakView", [&](StringView /*name*/, const FileInfo & /*entry*/) { ++entries; });
        MARLIN_EXPECT(entries == 2);
        MARLIN_EXPECT(Vfs::Open("/PakView/Small.bin", FileAccess::Read).IsValid() == false);
        Vfs::Unmount(mount);
    }
    (void)Vfs::Delete("/Temp/WriterTest.mpak");

    List<uint8> corrupt;
    PakWriter broken("Base", nullptr, 0);
    broken.AddFile("/Data", "", "A", Pattern(10).AsSpan());
    MARLIN_REQUIRE(broken.Write(path.AsView(), error));
    MARLIN_REQUIRE(Vfs::ReadAll("/Temp/WriterTest.mpak", corrupt));
    corrupt[0] ^= 1;
    WriteNative(path, corrupt.AsSpan());
    MARLIN_EXPECT(!PakFile::Open(path.AsView(), codecs).IsValid());
    (void)Vfs::Delete("/Temp/WriterTest.mpak");
}

MARLIN_TEST(Packaging, UnchangedPackagesAreKept)
{
    Fixture fixture;
    PackageDesc desc = fixture.Describe("Kept");
    desc.StateDirectory = Format("{0}/State", fixture.Physical);
    PackageResult first;
    StringBuilder<1024> errors;
    MARLIN_REQUIRE(BuildPackages(desc, first, errors));
    MARLIN_EXPECT(first.Packages.Count() == 2 && first.Kept == 0);
    const String base = Format("{0}/Kept/Base.mpak", fixture.Physical);
    const String stored = Format("{0}/Kept/Base.Stored.mpak", fixture.Physical);
    const auto modified = [](const String &path) {
        FileInfo info;
        return PlatformFile::GetInfo(path.CStr(), info) ? info.ModifiedTime : 0;
    };
    const int64 baseTime = modified(base);
    const int64 storedTime = modified(stored);

    // Nothing changed: both packages stay as they are, unread and unwritten.
    PackageResult second;
    MARLIN_REQUIRE(BuildPackages(desc, second, errors));
    MARLIN_EXPECT(second.Packages.Count() == 2 && second.Kept == 2 && second.Objects == first.Objects &&
                  second.Files == first.Files);
    MARLIN_EXPECT(modified(base) == baseTime && modified(stored) == storedTime);

    // A loose file changed: the package that holds the files is written again, the other kept.
    const List<uint8> edited = Pattern(64);
    WriteNative(Format("{0}/Engine/Config/Small.mtext", fixture.Physical), edited.AsSpan());
    PackageResult third;
    MARLIN_REQUIRE(BuildPackages(desc, third, errors));
    MARLIN_EXPECT(third.Packages.Count() == 2 && third.Kept == 1);
    MARLIN_EXPECT(modified(stored) == storedTime);
    const SharedPtr<PakFile> pak = PakFile::Open(base.AsView(), fixture.Codecs);
    MARLIN_REQUIRE(pak.IsValid());
    const PakFileEntry *small = pak->FindFile("/Engine", "", "Config/Small.mtext");
    MARLIN_EXPECT(small != nullptr && small->Size == edited.Count());
}

MARLIN_TEST(Packaging, BuildLoadAndPatch)
{
    Fixture fixture;
    PackageResult result;
    StringBuilder<1024> errors;
    MARLIN_REQUIRE(BuildPackages(fixture.Describe("Release"), result, errors));
    MARLIN_EXPECT(result.Objects == 3 && result.Files == 3 && result.Packages.Count() == 2);

    const String base = Format("{0}/Release/Base.mpak", fixture.Physical);
    const String stored = Format("{0}/Release/Base.Stored.mpak", fixture.Physical);
    {
        const SharedPtr<PakFile> basePak = PakFile::Open(base.AsView(), fixture.Codecs);
        const SharedPtr<PakFile> gpuPak = PakFile::Open(stored.AsView(), fixture.Codecs);
        MARLIN_REQUIRE(basePak.IsValid() && gpuPak.IsValid());
        MARLIN_EXPECT(basePak->FindAsset(A) != nullptr && basePak->FindAsset(B) != nullptr);
        MARLIN_EXPECT(basePak->FindAsset(Unused) == nullptr && basePak->FindAsset(Gpu) == nullptr);
        MARLIN_EXPECT(gpuPak->FindAsset(Gpu) != nullptr && gpuPak->GetFiles().IsEmpty());

        const MountId engine = Vfs::Mount("/PackagedEngine", MakeUnique<PakMount>(basePak, "/Engine", ""), 0);
        List<uint8> big;
        MARLIN_REQUIRE(Vfs::ReadAll("/PackagedEngine/Config/Big.mtext", big));
        const List<uint8> expected = Pattern(200000);
        MARLIN_EXPECT(big.Count() == expected.Count() && MemCompare(big.Data(), expected.Data(), big.Count()) == 0);
        Vfs::Unmount(engine);

        Assets::Initialize();
        const AssetStoreId baseStore = Assets::AddStore(MakeUnique<PakAssetStore>(basePak), 0);
        const AssetStoreId gpuStore = Assets::AddStore(MakeUnique<PakAssetStore>(gpuPak), 0);
        {
            const AssetRef<TestPackagedAsset> loaded = Assets::LoadSync<TestPackagedAsset>(A);
            MARLIN_REQUIRE(loaded.Get() != nullptr);
            MARLIN_EXPECT(loaded->Value == 7 && loaded->Next.Get() != nullptr && loaded->Next->Value == 8);
            const AssetRef<TestGpuAsset> gpu = Assets::LoadSync<TestGpuAsset>(Gpu);
            MARLIN_REQUIRE(gpu.Get() != nullptr);
            MARLIN_EXPECT(gpu->Texels.Count() == 5 && gpu->Texels[4] == 5);
        }
        Assets::CollectGarbage();
        Assets::RemoveStore(baseStore);
        Assets::RemoveStore(gpuStore);
        Assets::Shutdown();
    }

    // The patch: B changes, Gpu's source and the Windows layer's Small.mtext are deleted.
    String text;
    MARLIN_REQUIRE(Vfs::ReadAll("/PackagingTest/Content/Objects.masset", text));
    MARLIN_REQUIRE(text.AsView().Contains("Value = 8"));
    String patched(text.AsView());
    patched.Replace("Value = 8", "Value = 80");
    MARLIN_REQUIRE(Vfs::WriteAll("/PackagingTest/Content/Objects.masset", patched.AsView()));
    MARLIN_REQUIRE(Vfs::Delete("/PackagingTest/Content/Gpu/Gpu.masset"));
    MARLIN_REQUIRE(Vfs::Delete("/PackagingTest/Engine/Platforms/Windows/Config/Small.mtext"));
    List<String> changed;
    fixture.Scanner->ScanAll(changed);
    PackageDesc patch = fixture.Describe("Patch");
    patch.Previous.Add(base);
    patch.Previous.Add(stored);
    patch.PatchNumber = 1;
    MARLIN_REQUIRE(BuildPackages(patch, result, errors));
    MARLIN_EXPECT(result.Objects == 1 && result.Files == 0 && result.Unchanged == 3 && result.Deleted == 2);
    MARLIN_REQUIRE(result.Packages.Count() == 1);
    const SharedPtr<PakFile> patchPak = PakFile::Open(result.Packages[0].AsView(), fixture.Codecs);
    MARLIN_REQUIRE(patchPak.IsValid());
    MARLIN_EXPECT(patchPak->GetChunk() == "Patch_0001" && patchPak->FindAsset(B) != nullptr);
    MARLIN_EXPECT(patchPak->FindAsset(A) == nullptr);
    const PakAssetEntry *gone = patchPak->FindAsset(Gpu);
    MARLIN_EXPECT(gone != nullptr && gone->BlockSize == 0);
    const PakFileEntry *removed = patchPak->FindFile("/Engine", "Windows", "Config/Small.mtext");
    MARLIN_REQUIRE(removed != nullptr);
    MARLIN_EXPECT((removed->Flags & PakFileDeleted) != 0);

    // Mounted over the release, the patch hides the file it deletes.
    const SharedPtr<PakFile> basePak = PakFile::Open(base.AsView(), fixture.Codecs);
    MARLIN_REQUIRE(basePak.IsValid());
    const MountId below = Vfs::Mount("/PatchedEngine", MakeUnique<PakMount>(basePak, "/Engine", "Windows"), 0);
    MARLIN_EXPECT(Vfs::Exists("/PatchedEngine/Config/Small.mtext"));
    const MountId above = Vfs::Mount("/PatchedEngine", MakeUnique<PakMount>(patchPak, "/Engine", "Windows"), 1);
    MARLIN_EXPECT(!Vfs::Exists("/PatchedEngine/Config/Small.mtext"));
    usize entries = 0;
    Vfs::Enumerate("/PatchedEngine/Config", [&](StringView /*name*/, const FileInfo & /*entry*/) { ++entries; });
    MARLIN_EXPECT(entries == 0);
    Vfs::Unmount(above);
    Vfs::Unmount(below);
}

MARLIN_TEST(Packaging, LoadOrderLayout)
{
    StringBuilder<512> directory;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", directory) && !directory.IsEmpty());
    const String path = Format("{0}/LayoutTest.mpak", directory.AsView());
    {
        // Added in load order; the table is sorted by Uuid, the stream keeps the order the assets were added in.
        const List<uint8> bytes = Pattern(100);
        PakWriter writer("Base", nullptr, 0);
        writer.AddAsset(Gpu, bytes.AsSpan(), {});
        writer.AddAsset(A, bytes.AsSpan(), bytes.AsSpan());
        writer.AddAsset(B, bytes.AsSpan(), {});
        StringBuilder<128> error;
        MARLIN_REQUIRE(writer.Write(path.AsView(), error));
        CompressionCodecSet codecs;
        codecs.Build({});
        const SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), codecs);
        MARLIN_REQUIRE(pak.IsValid() && pak->GetAssets().Count() == 3);
        MARLIN_EXPECT(pak->GetAssets()[0].Uuid == A && pak->GetAssets()[2].Uuid == Gpu);
        const PakAssetEntry *gpu = pak->FindAsset(Gpu);
        const PakAssetEntry *a = pak->FindAsset(A);
        const PakAssetEntry *b = pak->FindAsset(B);
        MARLIN_REQUIRE(gpu != nullptr && a != nullptr && b != nullptr);
        MARLIN_EXPECT(gpu->BlockOffset < a->BlockOffset && a->BlockOffset < a->BulkOffset &&
                      a->BulkOffset < b->BlockOffset);
    }
    (void)Vfs::Delete("/Temp/LayoutTest.mpak");
}

MARLIN_TEST(Packaging, DependencyTableAndReads)
{
    CompressionCodecSet codecs;
    codecs.Build({});
    StringBuilder<512> directory;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", directory) && !directory.IsEmpty());
    const Guid chainA = {0x9a0c0000, 0, 1, 1};
    const Guid chainB = {0x9a0c0000, 0, 1, 2};
    const Guid chainC = {0x9a0c0000, 0, 1, 3};
    const auto write = [](const Guid &guid, int32 value, const Guid &next, List<uint8> &block) {
        TestPackagedAsset asset;
        asset.Value = value;
        if (next.IsValid())
        {
            asset.Next = AssetRef<TestPackagedAsset>::FromGuid(next);
        }
        MARLIN_REQUIRE(AssetBlock::Write(asset, guid, block));
    };
    List<uint8> a;
    List<uint8> b;
    List<uint8> c;
    write(chainA, 1, chainB, a);
    write(chainB, 2, chainC, b);
    write(chainC, 3, Guid(), c);
    for (const bool compressed : {false, true})
    {
        const String path = Format("{0}/DependencyTest.mpak", directory.AsView());
        {
            PakWriter writer("Base", compressed ? codecs.Find("TestRle") : nullptr, 0, 4096);
            const Guid nextOfA[] = {chainB};
            const Guid nextOfB[] = {chainC};
            writer.AddAsset(chainA, a.AsSpan(), {}, Span<const Guid>(nextOfA));
            writer.AddAsset(chainB, b.AsSpan(), Pattern(20000).AsSpan(), Span<const Guid>(nextOfB));
            writer.AddAsset(chainC, c.AsSpan(), {});
            StringBuilder<128> error;
            MARLIN_REQUIRE(writer.Write(path.AsView(), error));
        }
        const SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), codecs);
        MARLIN_REQUIRE(pak.IsValid());

        // The table round trip: each entry's range names the Guids its block references.
        const PakAssetEntry *entryA = pak->FindAsset(chainA);
        const PakAssetEntry *entryC = pak->FindAsset(chainC);
        MARLIN_REQUIRE(entryA != nullptr && entryC != nullptr);
        MARLIN_EXPECT(pak->GetDependencies(*entryA).Count() == 1 && pak->GetDependencies(*entryA)[0] == chainB);
        MARLIN_EXPECT(pak->GetDependencies(*entryC).IsEmpty());

        // Stored blocks read at their exact offset; compressed ones keep the blocks reads want only part of.
        const PakAssetEntry *entryB = pak->FindAsset(chainB);
        MARLIN_REQUIRE(entryB != nullptr && entryB->BulkSize == 20000);
        MARLIN_EXPECT(pak->ReadsDirectly() == !compressed);
        const FileRegion bulk = pak->GetRegion(entryB->BulkOffset, entryB->BulkSize);
        MARLIN_EXPECT(bulk.Compression.IsValid() == compressed);
        const List<uint8> expected = Pattern(20000);
        const AsyncReadRequest part = Vfs::ReadAsync(bulk, IoPriority::Critical, 5000, 100);
        MARLIN_REQUIRE(part.Wait(5000) && part.Succeeded());
        MARLIN_EXPECT(MemCompare(part.GetData().Data(), expected.Data() + 5000, 100) == 0);
        if (compressed)
        {
            const uint64 index = (entryB->BulkOffset + 5000) / pak->GetBlockSize();
            MARLIN_EXPECT(pak->FindCachedBlock(index).IsValid());
            const AsyncReadRequest again = Vfs::ReadAsync(bulk, IoPriority::Critical, 5100, 50);
            MARLIN_REQUIRE(again.Wait(5000) && again.Succeeded());
            MARLIN_EXPECT(MemCompare(again.GetData().Data(), expected.Data() + 5100, 50) == 0);
        }

        // Loading the head of the chain starts the rest from the table, before its block arrives.
        Assets::Initialize();
        const AssetStoreId store = Assets::AddStore(MakeUnique<PakAssetStore>(pak), 0);
        {
            const AssetRef<TestPackagedAsset> head = Assets::Load<TestPackagedAsset>(chainA);
            MARLIN_EXPECT(Assets::FindSlot(chainB) != nullptr);
            MARLIN_REQUIRE(Assets::Wait(*head.GetSlot(), 5000));
            MARLIN_REQUIRE(head->Next.IsLoaded() && head->Next->Next.IsLoaded());
            MARLIN_EXPECT(head->Value == 1 && head->Next->Value == 2 && head->Next->Next->Value == 3);
        }
        Assets::CollectGarbage();
        Assets::RemoveStore(store);
        Assets::Shutdown();
    }
    (void)Vfs::Delete("/Temp/DependencyTest.mpak");
}

MARLIN_TEST(Packaging, Encryption)
{
    CompressionCodecSet codecs;
    codecs.Build({});
    PakCrypto crypto;
    crypto.Build({});
    PakEncryption encryption;
    encryption.Cipher = crypto.FindCipher("Aes256Ctr");
    MARLIN_REQUIRE(encryption.Cipher != nullptr);
    encryption.KeyId = {0x9a0c0000, 0, 0, 0x4b};
    for (uint32 i = 0; i < 32; ++i)
    {
        encryption.Key.Add(static_cast<uint8>(i * 7 + 3));
    }
    StringBuilder<512> temp;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", temp) && !temp.IsEmpty());
    const String path = Format("{0}/EncryptedTest.mpak", temp.AsView());
    const List<uint8> big = Pattern(300000);
    List<uint8> zeros;
    zeros.Resize(20000, 0);
    {
        // Blocks run-length coding cannot shrink are stored, Zeros.bin's are compressed; both, and the tables, are
        // encrypted.
        PakWriter writer("Base", codecs.Find("TestRle"), 0, 4096, &encryption);
        writer.AddFile("/Data", "", "Folder/Big.bin", big.AsSpan());
        writer.AddFile("/Data", "", "Small.bin", Pattern(10).AsSpan());
        writer.AddFile("/Data", "", "Zeros.bin", zeros.AsSpan());
        writer.AddAsset(A, Pattern(5000).AsSpan(), {});
        StringBuilder<128> error;
        MARLIN_REQUIRE(writer.Write(path.AsView(), error));
    }
    List<uint8> raw;
    MARLIN_REQUIRE(Vfs::ReadAll("/Temp/EncryptedTest.mpak", raw));
    MARLIN_EXPECT(!StringView(reinterpret_cast<const char *>(raw.Data()), raw.Count()).Contains("Folder/Big.bin"));
    MARLIN_EXPECT(!PakFile::Open(path.AsView(), codecs).IsValid());
    MARLIN_EXPECT(!PakFile::Open(path.AsView(), codecs, &crypto).IsValid());
    crypto.AddKey(encryption.KeyId, encryption.Key.AsSpan());
    {
        const SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), codecs, &crypto);
        MARLIN_REQUIRE(pak.IsValid());
        MARLIN_EXPECT(pak->IsEncrypted() && pak->GetFiles().Count() == 3 && pak->FindAsset(A) != nullptr);
        const MountId mount = Vfs::Mount("/EncryptedView", MakeUnique<PakMount>(pak, "/Data", ""), 0);
        List<uint8> read;
        MARLIN_EXPECT(Vfs::ReadAll("/EncryptedView/Folder/Big.bin", read) && read.Count() == big.Count() &&
                      MemCompare(read.Data(), big.Data(), big.Count()) == 0);
        const AsyncReadRequest part = Vfs::ReadAsync("/EncryptedView/Folder/Big.bin", IoPriority::Critical, 4090, 9000);
        MARLIN_REQUIRE(part.Wait(5000) && part.Succeeded());
        MARLIN_EXPECT(MemCompare(part.GetData().Data(), big.Data() + 4090, 9000) == 0);
        MARLIN_EXPECT(Vfs::ReadAll("/EncryptedView/Small.bin", read) && read.Count() == 10);
        MARLIN_EXPECT(Vfs::ReadAll("/EncryptedView/Zeros.bin", read) && read.Count() == zeros.Count() &&
                      MemCompare(read.Data(), zeros.Data(), zeros.Count()) == 0);
        Vfs::Unmount(mount);
    }
    (void)Vfs::Delete("/Temp/EncryptedTest.mpak");

    // The packager encrypts with the configured cipher and the key it is given, and refuses a key of the wrong size.
    Fixture fixture;
    PackageDesc desc = fixture.Describe("Encrypted");
    desc.Settings.Encryption.Cipher = "Aes256Ctr";
    desc.Settings.Encryption.KeyId = encryption.KeyId;
    desc.EncryptionKey.AddRange(Span<const uint8>(encryption.Key.Data(), 16));
    PackageResult result;
    StringBuilder<1024> errors;
    MARLIN_EXPECT(!BuildPackages(desc, result, errors));
    desc.EncryptionKey.Clear();
    desc.EncryptionKey.AddRange(encryption.Key.AsSpan());
    MARLIN_REQUIRE(BuildPackages(desc, result, errors));
    const String base = Format("{0}/Encrypted/Base.mpak", fixture.Physical);
    MARLIN_EXPECT(!PakFile::Open(base.AsView(), fixture.Codecs).IsValid());
    const SharedPtr<PakFile> basePak = PakFile::Open(base.AsView(), fixture.Codecs, &crypto);
    MARLIN_REQUIRE(basePak.IsValid());
    MARLIN_EXPECT(basePak->IsEncrypted() && basePak->FindAsset(A) != nullptr);
}

MARLIN_TEST(Packaging, Signing)
{
    Fixture fixture;
    PakCrypto plain;
    plain.Build({});
    const PakSignatureScheme *scheme = plain.FindScheme("EcdsaP256");
    MARLIN_REQUIRE(scheme != nullptr);
    List<uint8> privateKey;
    List<uint8> publicKey;
    MARLIN_REQUIRE(scheme->Generate(privateKey, publicKey));
    const Guid keyId{0x5191ed00, 0, 0, 0x2a};

    // Signing needs the private key, which never comes from config.
    PackageDesc desc = fixture.Describe("Signed");
    desc.Settings.Signing.Scheme = "EcdsaP256";
    desc.Settings.Signing.KeyId = keyId;
    PackageResult result;
    StringBuilder<1024> errors;
    MARLIN_EXPECT(!BuildPackages(desc, result, errors));
    desc.SigningKey = privateKey;
    MARLIN_REQUIRE(BuildPackages(desc, result, errors));
    const String base = Format("{0}/Signed/Base.mpak", fixture.Physical);
    const String signature = PakSigning::SignaturePath(base.AsView());
    MARLIN_EXPECT(PlatformFile::FileExists(signature.CStr()));

    // Without the public key the package opens unsigned; with it, verified.
    const SharedPtr<PakFile> unverified = PakFile::Open(base.AsView(), fixture.Codecs, &plain);
    MARLIN_REQUIRE(unverified.IsValid());
    MARLIN_EXPECT(!unverified->IsSigned() && !plain.RequiresSignatures());
    PakCrypto crypto;
    crypto.Build({});
    crypto.AddPublicKey(keyId, publicKey.AsSpan());
    MARLIN_EXPECT(crypto.RequiresSignatures());
    const SharedPtr<PakFile> pak = PakFile::Open(base.AsView(), fixture.Codecs, &crypto);
    MARLIN_REQUIRE(pak.IsValid());
    MARLIN_EXPECT(pak->IsSigned() && !pak->IsPlain(0) && pak->FindAsset(A) != nullptr);

    // A block whose stored bytes changed no longer reads.
    uint64 offset = 0;
    uint32 stored = 0;
    uint32 logical = 0;
    MARLIN_REQUIRE(pak->GetBlock(0, offset, stored, logical));
    List<uint8> bytes;
    bytes.Resize(stored);
    {
        const FileHandle file = PlatformFile::Open(base.CStr(), FileAccess::Read, FileMode::OpenExisting);
        usize read = 0;
        MARLIN_REQUIRE(file.IsValid() && PlatformFile::ReadAt(file, offset, bytes.Data(), bytes.Count(), read));
        PlatformFile::Close(file);
    }
    List<uint8> output;
    output.Resize(logical);
    MARLIN_EXPECT(pak->Decompress(0, bytes.AsSpan(), output.AsSpan()));
    bytes[0] ^= 1;
    MARLIN_EXPECT(!pak->Decompress(0, bytes.AsSpan(), output.AsSpan()));

    // A changed signature, or another key, does not verify.
    List<uint8> original;
    MARLIN_REQUIRE(Vfs::ReadAll("/PackagingTest/Signed/Base.msig", original));
    List<uint8> forged(original);
    forged[offsetof(PakSignatureHeader, Signature)] ^= 1;
    WriteNative(signature, forged.AsSpan());
    MARLIN_EXPECT(!PakFile::Open(base.AsView(), fixture.Codecs, &crypto).IsValid());
    WriteNative(signature, original.AsSpan());
    MARLIN_EXPECT(PakFile::Open(base.AsView(), fixture.Codecs, &crypto).IsValid());
    List<uint8> otherPrivate;
    List<uint8> otherPublic;
    MARLIN_REQUIRE(scheme->Generate(otherPrivate, otherPublic));
    PakCrypto other;
    other.AddPublicKey(keyId, otherPublic.AsSpan());
    other.Build({});
    MARLIN_EXPECT(!PakFile::Open(base.AsView(), fixture.Codecs, &other).IsValid());

    // An unsigned build refuses to write over a package whose stale signature would make the game reject it.
    desc.Settings.Signing = {};
    MARLIN_EXPECT(!BuildPackages(desc, result, errors));
}
} // namespace Marlin::Tests
