#include "Testing/Testing.h"

#include "Containers/List.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformTime.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
namespace
{
constexpr int32 MountCount = 32;
constexpr usize LargeSize = usize{256} << 20;
constexpr usize ChunkSize = usize{64} << 10;

void WriteFile(StringView path, usize size)
{
    List<uint8> bytes;
    bytes.ResizeUninitialized(size);
    for (usize i = 0; i < size; ++i)
    {
        bytes[i] = static_cast<uint8>(i * 31);
    }
    const FileHandle file = PlatformFile::Open(String(path).CStr(), FileAccess::Write, FileMode::CreateAlways);
    MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, bytes.Data(), bytes.Count()));
    PlatformFile::Close(file);
}

void Report(StringView name, float64 seconds, float64 count, StringView unit)
{
    Testing::Print(Format("{0,-46} {1,10:F2} ms {2,12:F1} {3}\n", name, seconds * 1000, count / seconds, unit));
}

void ReportBytes(StringView name, float64 seconds, float64 bytes)
{
    Testing::Print(Format("{0,-46} {1,10:F2} ms {2,12:F2} GB/s\n", name, seconds * 1000, bytes / seconds / 1e9));
}
} // namespace

MARLIN_TEST(Benchmark, Vfs, Benchmark)
{
    StringBuilder<512> root;
    MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp", root));
    root.Append("/VfsBenchmark");
    const String directory(root.AsView());
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    MARLIN_REQUIRE(PlatformFile::CreateDirectory(directory.CStr()));
    WriteFile(directory + "/small.bin", 4096);
    WriteFile(directory + "/large.bin", LargeSize);

    // Mounts at many points and priorities; the file is in the lowest mount of the deepest point.
    List<MountId> mounts;
    for (int32 i = 0; i < MountCount; ++i)
    {
        StringBuilder<64> point("/Bench");
        point.Append(i % 2 == 0 ? "/Deep" : "/Other");
        mounts.Add(Vfs::Mount(point.AsView(), MakeUnique<DirectoryMount>(directory, false), i));
    }
    mounts.Add(Vfs::Mount("/Bench/Deep", MakeUnique<DirectoryMount>(directory, false), -1));
    const String smallPhysical = directory + "/small.bin";
    const String largePhysical = directory + "/large.bin";

    constexpr int32 Lookups = 20000;
    float64 start = PlatformTime::Seconds();
    for (int32 i = 0; i < Lookups; ++i)
    {
        StringBuilder<128> normal;
        (void)Vfs::Normalize("/Bench//Deep/./Data\\small.bin", normal);
    }
    Report("Normalize", PlatformTime::Seconds() - start, Lookups, "/s");
    start = PlatformTime::Seconds();
    for (int32 i = 0; i < Lookups; ++i)
    {
        (void)PlatformFile::FileExists(smallPhysical.CStr());
    }
    const float64 raw = PlatformTime::Seconds() - start;
    Report("PlatformFile::FileExists", raw, Lookups, "/s");
    start = PlatformTime::Seconds();
    for (int32 i = 0; i < Lookups; ++i)
    {
        (void)Vfs::Exists("/Bench/Deep/small.bin");
    }
    const float64 resolved = PlatformTime::Seconds() - start;
    Report("Vfs::Exists, first of 17 mounts has it", resolved, Lookups, "/s");
    start = PlatformTime::Seconds();
    for (int32 i = 0; i < Lookups; ++i)
    {
        (void)Vfs::Exists("/Bench/Deep/missing.bin");
    }
    Report("Vfs::Exists, missing in all 17 mounts", PlatformTime::Seconds() - start, Lookups, "/s");

    constexpr int32 SmallReads = 5000;
    List<uint8> buffer;
    start = PlatformTime::Seconds();
    for (int32 i = 0; i < SmallReads; ++i)
    {
        const FileHandle file = PlatformFile::Open(smallPhysical.CStr(), FileAccess::Read, FileMode::OpenExisting);
        uint8 bytes[4096];
        usize read = 0;
        (void)PlatformFile::Read(file, bytes, sizeof(bytes), read);
        PlatformFile::Close(file);
    }
    Report("4 KiB read, PlatformFile", PlatformTime::Seconds() - start, SmallReads, "/s");
    start = PlatformTime::Seconds();
    for (int32 i = 0; i < SmallReads; ++i)
    {
        (void)Vfs::ReadAll("/Bench/Deep/small.bin", buffer);
    }
    Report("4 KiB read, Vfs::ReadAll", PlatformTime::Seconds() - start, SmallReads, "/s");

    start = PlatformTime::Seconds();
    {
        const FileHandle file = PlatformFile::Open(largePhysical.CStr(), FileAccess::Read, FileMode::OpenExisting);
        buffer.ResizeUninitialized(LargeSize);
        usize read = 0;
        (void)PlatformFile::Read(file, buffer.Data(), LargeSize, read);
        PlatformFile::Close(file);
    }
    ReportBytes("256 MiB read, PlatformFile", PlatformTime::Seconds() - start, LargeSize);
    start = PlatformTime::Seconds();
    MARLIN_EXPECT(Vfs::ReadAll("/Bench/Deep/large.bin", buffer) && buffer.Count() == LargeSize);
    ReportBytes("256 MiB read, Vfs::ReadAll", PlatformTime::Seconds() - start, LargeSize);
    start = PlatformTime::Seconds();
    const AsyncReadRequest whole = Vfs::ReadAsync("/Bench/Deep/large.bin");
    MARLIN_EXPECT(whole.Wait() && whole.Succeeded());
    ReportBytes("256 MiB read, Vfs::ReadAsync in 16 MiB parts", PlatformTime::Seconds() - start, LargeSize);

    constexpr usize Chunks = LargeSize / ChunkSize;
    List<AsyncReadRequest> requests;
    requests.Reserve(Chunks);
    start = PlatformTime::Seconds();
    for (usize i = 0; i < Chunks; ++i)
    {
        requests.Add(Vfs::ReadAsync("/Bench/Deep/large.bin", IoPriority::Streaming, i * ChunkSize, ChunkSize));
    }
    const float64 submitted = PlatformTime::Seconds() - start;
    bool ok = true;
    for (const AsyncReadRequest &request : requests)
    {
        ok = ok && request.Wait() && request.Succeeded();
    }
    const float64 seconds = PlatformTime::Seconds() - start;
    MARLIN_EXPECT(ok);
    Report("4096 x 64 KiB ReadAsync, issuing", submitted, Chunks, "requests/s");
    ReportBytes("4096 x 64 KiB ReadAsync, all done", seconds, LargeSize);
    Testing::Print(Format("Vfs::Exists costs {0:F2}x a direct FileExists\n", resolved / raw));

    for (const MountId id : mounts)
    {
        Vfs::Unmount(id);
    }
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}
} // namespace Marlin::Tests
