#include "Testing/Testing.h"

#include "AssetPipeline/AssetDatabase.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/PackedColumn.h"
#include "Concurrency/Jobs.h"
#include "HAL/PlatformTime.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
MARLIN_TEST(AssetDatabase, PackedColumn)
{
    List<uint64> values;
    for (uint64 i = 0; i < 10000; ++i)
    {
        values.Add(i < 5000 ? 7 : (i < 9000 ? 1000 + (i * 37) % 300 : (i * 0x9E3779B97F4A7C15ull)));
    }
    values[4096] = ~uint64{0};
    List<uint8> bytes;
    PackedColumnWriter::Write(values.AsSpan(), bytes);
    PackedColumnView column;
    MARLIN_REQUIRE(column.Open(bytes.Data(), bytes.Count()) && column.Count() == values.Count());
    bool same = true;
    for (uint32 i = 0; i < values.Count(); ++i)
    {
        same = same && column[i] == values[i];
    }
    MARLIN_EXPECT(same);
    List<uint64> constant;
    constant.Resize(PackedColumnPageRows * 3, 42);
    bytes.Clear();
    PackedColumnWriter::Write(constant.AsSpan(), bytes);
    MARLIN_REQUIRE(column.Open(bytes.Data(), bytes.Count()));
    MARLIN_EXPECT(column[PackedColumnPageRows * 2 + 5] == 42 && bytes.Count() < 128);
}

MARLIN_TEST(AssetDatabase, MergeCopiesBaseRows)
{
    const auto make = [](StringView path, uint32 id, StringView name) {
        AssetFileRecord record;
        record.Path = path;
        record.IsAsset = true;
        record.Size = id;
        AssetObjectRecord &object = record.Objects.AddDefaulted();
        object.Uuid = Guid{id, 0, 0, 1};
        object.Name = name;
        object.Type = "Material";
        object.Tags.Add("Wet");
        object.Links.Add(Guid{1, 0, 0, 1});
        return record;
    };
    const AssetFileRecord a = make("/Project/Content/A.masset", 1, "A");
    const AssetFileRecord b = make("/Project/Content/Rocks/B.masset", 2, "B");
    const AssetFileRecord c = make("/Project/Content/Rocks/C.masset", 3, "C");
    const AssetFileRecord *base[] = {&c, &a, &b};
    List<uint8> baseImage;
    AssetDatabaseBuilder::Build(base, {}, 1, baseImage);
    AssetDatabaseView baseView;
    MARLIN_REQUIRE(baseView.Open(baseImage.AsSpan()));

    // B is replaced, C removed, D added between A and B.
    AssetFileRecord replaced = make("/Project/Content/Rocks/B.masset", 2, "B2");
    const AssetFileRecord d = make("/Project/Content/D.masset", 4, "D");
    const String removed[] = {"/Project/Content/Rocks/B.masset", "/Project/Content/Rocks/C.masset"};
    const AssetFileRecord *changes[] = {&replaced, &d};
    List<uint8> merged;
    AssetDatabaseBuilder::Merge(baseView, removed, changes, 2, merged);
    AssetDatabaseView view;
    MARLIN_REQUIRE(view.Open(merged.AsSpan()));
    MARLIN_REQUIRE(view.FileCount() == 3 && view.ObjectCount() == 3 && view.GetGeneration() == 2);
    StringBuilder<256> path;
    view.GetPath(0, path);
    MARLIN_EXPECT(path.AsView() == "/Project/Content/A.masset");
    path.Clear();
    view.GetPath(1, path);
    MARLIN_EXPECT(path.AsView() == "/Project/Content/D.masset");
    const uint32 copied = view.FindObject(Guid{1, 0, 0, 1});
    MARLIN_REQUIRE(copied != AssetDatabaseNone);
    MARLIN_EXPECT(view.GetName(copied) == "A" && view.GetType(copied) == "Material");
    uint32 start = 0;
    uint32 count = 0;
    view.GetTags(copied, start, count);
    MARLIN_EXPECT(count == 1 && view.GetTagPool()[view.GetTagId(start)] == "Wet");
    view.GetReferencers(Guid{1, 0, 0, 1}, start, count);
    MARLIN_EXPECT(count == 3);
    MARLIN_EXPECT(view.GetName(view.FindObject(Guid{2, 0, 0, 1})) == "B2");
    MARLIN_EXPECT(view.FindObject(Guid{3, 0, 0, 1}) == AssetDatabaseNone);
    MARLIN_EXPECT(view.FindFile("/Project/Content/Rocks/B.masset") == 2);
    MARLIN_EXPECT(view.LowerBound("/Project/Content/Rocks") == 2 && view.LowerBound("/Project/Content/Z") == 3);
}

MARLIN_TEST(Benchmark, AssetDatabase, Benchmark)
{
    const bool startJobs = !JobSystem::IsRunning();
    if (startJobs)
    {
        MARLIN_REQUIRE(JobSystem::Start());
    }
    constexpr uint32 Files = 250000;
    constexpr uint32 ObjectsPerFile = 4;
    const char *types[] = {"Material", "TextureSource", "MeshSource", "EnvironmentProbe", "Prefab", "Quest"};
    List<AssetFileRecord> records;
    records.Resize(Files);
    for (uint32 f = 0; f < Files; ++f)
    {
        AssetFileRecord &record = records[f];
        record.Path = Format("/Project/Content/Area{0}/Group{1}/Asset{2}.masset", f / 5000, (f / 100) % 50, f);
        record.IsAsset = true;
        record.Size = 1000 + f;
        record.ContentHash = {f * 0x9E3779B97F4A7C15ull, f};
        for (uint32 o = 0; o < ObjectsPerFile; ++o)
        {
            AssetObjectRecord &object = record.Objects.AddDefaulted();
            const uint32 id = f * ObjectsPerFile + o;
            object.Uuid = {0xA55E7000u, id, id * 2654435761u, id};
            object.Name = Format("{0}_{1}", types[id % 6], id % 20000);
            object.Type = types[id % 6];
            object.Line = 1 + o * 12;
            if (id > 0)
            {
                const uint32 target = (id * 7919) % id;
                object.Links.Add({0xA55E7000u, target, target * 2654435761u, target});
            }
            if (id % 10 == 0)
            {
                object.Tags.Add("Wet");
            }
        }
    }
    List<const AssetFileRecord *> pointers;
    for (const AssetFileRecord &record : records)
    {
        pointers.Add(&record);
    }
    float64 start = PlatformTime::Seconds();
    List<uint8> image;
    AssetDatabaseBuilder::Build(pointers.AsSpan(), {}, 1, image);
    const float64 build = PlatformTime::Seconds() - start;
    AssetIndexView view;
    view.Open(image.AsSpan(), {});
    const uint32 objects = Files * ObjectsPerFile;
    Testing::Print(Format("{0} files, {1} objects: built in {2:F2} s, image {3:F1} MB ({4:F1} bytes per object)\n",
                          Files, objects, build, image.Count() / 1e6, static_cast<float64>(image.Count()) / objects));

    constexpr uint32 Lookups = 1000000;
    AssetObjectInfo info;
    start = PlatformTime::Seconds();
    uint32 found = 0;
    for (uint32 i = 0; i < Lookups; ++i)
    {
        const uint32 id = (i * 2654435761u) % objects;
        Hash128 hash;
        found += view.GetCanonicalHash({0xA55E7000u, id, id * 2654435761u, id}, hash) ? 1 : 0;
    }
    const float64 lookup = PlatformTime::Seconds() - start;
    MARLIN_EXPECT(found == Lookups);
    Testing::Print(Format("UUID to hash, one thread: {0:F3} us each, {1:F0} per second\n", lookup * 1e6 / Lookups,
                          Lookups / lookup));
    List<Guid> batch;
    for (uint32 i = 0; i < Lookups; ++i)
    {
        const uint32 id = (i * 2654435761u) % objects;
        batch.Add({0xA55E7000u, id, id * 2654435761u, id});
    }
    List<AssetObjectInfo> infos;
    List<bool> hits;
    start = PlatformTime::Seconds();
    view.FindMany(batch.AsSpan(), infos, hits);
    const float64 many = PlatformTime::Seconds() - start;
    Testing::Print(Format("FindMany, {0} workers: {1:F0} objects per second with names and paths\n",
                          JobSystem::GetWorkerCount() + 1, Lookups / many));

    AssetQuery query;
    query.Type = "EnvironmentProbe";
    query.Limit = ~0u;
    List<AssetObjectInfo> results;
    start = PlatformTime::Seconds();
    view.Query(query, results);
    Testing::Print(
        Format("Query by type: {0} objects in {1:F2} ms\n", results.Count(), (PlatformTime::Seconds() - start) * 1000));
    query.Type = {};
    query.Name = "*Quest_1999*";
    results.Clear();
    start = PlatformTime::Seconds();
    view.Query(query, results);
    Testing::Print(Format("Query by name pattern: {0} objects in {1:F2} ms\n", results.Count(),
                          (PlatformTime::Seconds() - start) * 1000));
    start = PlatformTime::Seconds();
    uint32 referenced = 0;
    for (uint32 i = 0; i < 100000; ++i)
    {
        const uint32 id = (i * 40503u) % objects;
        uint32 first = 0;
        uint32 count = 0;
        view.GetBase().GetReferencers({0xA55E7000u, id, id * 2654435761u, id}, first, count);
        referenced += count != 0 ? 1 : 0;
    }
    Testing::Print(Format("Is it referenced: {0:F3} us each ({1} of 100000 are)\n",
                          (PlatformTime::Seconds() - start) * 1e6 / 100000, referenced));
    if (startJobs)
    {
        JobSystem::Stop();
    }
}
} // namespace Marlin::Tests
