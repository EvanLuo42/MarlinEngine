#include "Testing/Testing.h"

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "Config/Config.h"
#include "HAL/PlatformFile.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "MeshPipeline/MeshConverter.h"
#include "MeshPipeline/MeshImporter.h"
#include "MeshTestTypes.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"
#include "World/Hierarchy.h"

namespace Marlin::Tests
{
namespace
{
class MeshLocationStore final : public IAssetStore
{
public:
    /// *path* holds the converted result, as ConversionStore::ExportResult writes it.
    void Add(const Guid &guid, StringView path, const ConvertedResultLocation &location)
    {
        _guids.Add(guid);
        _paths.Add(String(path));
        _locations.Add(location);
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const usize index = _guids.IndexOf(guid);
        FileRegion file;
        if (index == List<Guid>::NotFound || !Vfs::OpenRegion(_paths[index].AsView(), file))
        {
            done(nullptr);
            return;
        }
        const ConvertedResultLocation &found = _locations[index];
        AssetLocation location;
        location.Block = file;
        location.Block.Offset = file.Offset + found.BlockOffset;
        location.Block.Size = found.BlockSize;
        location.Bulk = file;
        location.Bulk.Offset = file.Offset + found.BulkOffset;
        location.Bulk.Size = found.BulkSize;
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "MeshTest";
    }

private:
    List<Guid> _guids;
    List<String> _paths;
    List<ConvertedResultLocation> _locations;
};

/// The record of a group's *index*-th cluster in *bulk*.
MeshPageCluster ReadCluster(const Mesh &mesh, Span<const uint8> bulk, const MeshClusterGroup &group, uint32 index)
{
    MeshPageCluster cluster;
    MemCopy(&cluster,
            bulk.Data() + mesh.Pages[group.Page].Offset + (usize{group.FirstCluster} + index) * sizeof(MeshPageCluster),
            sizeof(cluster));
    return cluster;
}

/// Checks that the DAG is consistent: pages lie in the bulk data and hold their records and geometry, every group's
/// clusters lie in its page, refinements point to finer groups whose pages name this one as a parent, clusters fit
/// the limits and their page's geometry, and the source clusters cover the triangles.
bool CheckDag(const Mesh &mesh, Span<const uint8> bulk)
{
    if (mesh.Groups.IsEmpty() || mesh.Pages.IsEmpty() || mesh.RootPageCount == 0 ||
        mesh.RootPageCount > mesh.Pages.Count() || mesh.Hierarchy.Count() < mesh.DepthCount)
    {
        return false;
    }
    uint32 clusterCount = 0;
    for (const MeshPage &page : mesh.Pages)
    {
        if (page.Offset % 16 != 0 || page.Offset + page.Size > bulk.Count() ||
            page.ClusterCount > Mesh::MaxPageClusters ||
            page.Size != page.GetGeometryOffset() + page.VertexBytes + page.TriangleCount * 3 ||
            page.FirstParent + page.ParentCount > mesh.PageParents.Count())
        {
            return false;
        }
        clusterCount += page.ClusterCount;
    }
    uint32 sourceTriangles = 0;
    for (usize g = 0; g < mesh.Groups.Count(); ++g)
    {
        const MeshClusterGroup &group = mesh.Groups[g];
        if (group.Depth >= mesh.DepthCount || group.Page >= mesh.Pages.Count() ||
            group.FirstCluster + group.ClusterCount > mesh.Pages[group.Page].ClusterCount)
        {
            return false;
        }
        const MeshPage &page = mesh.Pages[group.Page];
        for (uint32 c = 0; c < group.ClusterCount; ++c)
        {
            const MeshPageCluster cluster = ReadCluster(mesh, bulk, group, c);
            bool refined = cluster.RefinedGroup < 0;
            if (!refined && static_cast<usize>(cluster.RefinedGroup) < mesh.Groups.Count())
            {
                const MeshClusterGroup &finer = mesh.Groups[static_cast<usize>(cluster.RefinedGroup)];
                const MeshPage &finerPage = mesh.Pages[finer.Page];
                bool parent = finer.Page == group.Page;
                for (uint32 p = 0; p < finerPage.ParentCount; ++p)
                {
                    parent = parent || mesh.PageParents[finerPage.FirstParent + p] == group.Page;
                }
                refined = finer.Depth < group.Depth && parent;
            }
            const uint32 vertexBytes = page.VertexBytes;
            if (cluster.Group != g || !refined || cluster.Section >= mesh.Sections.Count() ||
                cluster.VertexCount > Mesh::MaxClusterVertices || cluster.TriangleCount > Mesh::MaxClusterTriangles ||
                cluster.VertexOffset % 4 != 0 ||
                cluster.VertexOffset + cluster.GetPositionBytes() + cluster.VertexCount * mesh.AttributeStride >
                    vertexBytes ||
                cluster.IndexOffset < vertexBytes || cluster.IndexOffset + cluster.TriangleCount * 3 > page.Size)
            {
                return false;
            }
            // The packed positions decode to points of the mesh inside the cluster's sphere.
            const uint8 *geometry = bulk.Data() + page.Offset + page.GetGeometryOffset();
            const Vector3 center(cluster.Sphere[0], cluster.Sphere[1], cluster.Sphere[2]);
            for (uint32 v = 0; v < cluster.VertexCount; ++v)
            {
                const Vector3 position = mesh.ReadPosition(geometry, cluster, v);
                const Vector3 low = mesh.Bounds.Min - Vector3(mesh.PositionStep);
                const Vector3 high = mesh.Bounds.Max + Vector3(mesh.PositionStep);
                if ((position - center).Length() > cluster.Sphere[3] * 1.001f + mesh.PositionStep ||
                    position.X < low.X || position.Y < low.Y || position.Z < low.Z || position.X > high.X ||
                    position.Y > high.Y || position.Z > high.Z)
                {
                    return false;
                }
            }
            // Every edge lies inside the sphere, so none is longer than its diameter.
            if (!(cluster.MaxEdgeLength > 0) || cluster.MaxEdgeLength > cluster.Sphere[3] * 2.001f + 1e-6f)
            {
                return false;
            }
            sourceTriangles += cluster.RefinedGroup < 0 ? cluster.TriangleCount : 0;
        }
    }
    return sourceTriangles == mesh.TriangleCount && clusterCount == mesh.ClusterCount;
}

/// A scratch folder at /Temp/MeshPipelineTest mounted as /MeshTest. Everything is created and deleted through the
/// VFS, inside the /Temp mount, so no failed lookup can turn into a native path outside it.
struct MeshFixture
{
    MountId Mount = 0;

    MeshFixture()
    {
        (void)Vfs::Delete("/Temp/MeshPipelineTest");
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/MeshPipelineTest"));
        MARLIN_REQUIRE(Vfs::CreateDirectory("/Temp/MeshPipelineTest/Content"));
        StringBuilder<512> path;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath("/Temp/MeshPipelineTest", path) && !path.IsEmpty());
        Mount = Vfs::Mount("/MeshTest", MakeUnique<DirectoryMount>(path.AsView(), true), 0);
    }

    ~MeshFixture()
    {
        if (Mount != 0)
        {
            Vfs::Unmount(Mount);
        }
        (void)Vfs::Delete("/Temp/MeshPipelineTest");
    }

    MARLIN_NONCOPYABLE(MeshFixture);
};
} // namespace

MARLIN_TEST(MeshPipeline, ImportAndConvert)
{
    MeshFixture fixture;
    MARLIN_REQUIRE(Vfs::WriteAll("/MeshTest/Content/Rocks.testscene", "scene"));
    AssetIndex index;
    List<String> folders;
    folders.Add("/MeshTest/Content");
    AssetScanner scanner(index, folders);
    List<String> changed;
    scanner.ScanAll(changed);
    AssetFileRecord record;
    MARLIN_REQUIRE(index.GetRecord("/MeshTest/Content/Rocks.testscene", record) && record.BinaryGuid.IsValid());

    // By default, the whole file as one MeshSource; the options are kept, so importing again keeps it.
    constexpr StringView RocksPath = "/MeshTest/Content/Rocks.testscene";
    AssetImport importer;
    const ImportResult imported = importer.Import(RocksPath, record.BinaryGuid);
    MARLIN_REQUIRE(imported.Succeeded);
    MARLIN_EXPECT(imported.AssetPath == "/MeshTest/Content/Rocks.masset" && imported.Added == 1);
    const ImportResult again = importer.Import(RocksPath, record.BinaryGuid);
    MARLIN_EXPECT(again.Succeeded && again.Added == 0 && again.Kept == 1);

    // As a prefab: a MeshSource per mesh, an entity per node and the prefab. The combined mesh stays, and the kept
    // options change, so importing again without options keeps importing as a prefab.
    MeshImportOptions asPrefab;
    asPrefab.Combine = false;
    const ImportResult prefabImport = importer.Import(RocksPath, record.BinaryGuid, &asPrefab);
    MARLIN_EXPECT(prefabImport.Succeeded && prefabImport.Added == 3 + 3 + 1 && prefabImport.Kept == 0);
    const ImportResult prefabAgain = importer.Import(RocksPath, record.BinaryGuid);
    MARLIN_EXPECT(prefabAgain.Succeeded && prefabAgain.Added == 0 && prefabAgain.Kept == 7);
    {
        String masset;
        MARLIN_REQUIRE(Vfs::ReadAll("/MeshTest/Content/Rocks.masset", masset));
        MarlinTextDocument document;
        MarlinTextDiagnostics diagnostics;
        MARLIN_REQUIRE(document.Parse(masset.AsView(), diagnostics));
        MARLIN_EXPECT(document.GetObjectCount() == 1 + 1 + 7);
        const Guid options = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, ":Options");
        const usize optionsIndex = document.FindObject(options, diagnostics);
        MARLIN_REQUIRE(optionsIndex != MarlinTextDocument::NotFound);
        const MarlinTextField *combine = document.GetObject(optionsIndex, diagnostics)->Find("Combine");
        MARLIN_EXPECT(combine != nullptr && combine->Value.Source == "false");
        // Floor hangs under Rocks.
        const Guid floor = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, ":Node/Floor");
        const usize floorIndex = document.FindObject(floor, diagnostics);
        MARLIN_REQUIRE(floorIndex != MarlinTextDocument::NotFound);
        const MarlinTextObject *entity = document.GetObject(floorIndex, diagnostics);
        MARLIN_REQUIRE(entity != nullptr);
        MARLIN_EXPECT(entity->GetTypeName() == "PrefabEntity" && entity->Find("Parent") != nullptr);
        // The pivot's matrix comes apart into its translation, rotation and scale, and it draws nothing.
        const Guid pivot = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, ":Node/Pivot");
        const MarlinTextObject *pivotEntity = document.GetObject(document.FindObject(pivot, diagnostics), diagnostics);
        MARLIN_REQUIRE(pivotEntity != nullptr);
        const MarlinTextField *components = pivotEntity->Find("Components");
        MARLIN_REQUIRE(components != nullptr);
        const MarlinTextField *localField = components->Value.FindField("LocalTransform");
        MARLIN_REQUIRE(localField != nullptr && components->Value.FindField("RenderMesh") == nullptr);
        LocalTransform local;
        MARLIN_REQUIRE(
            MarlinTextSerializer::ReadValue(localField->Value, TypeOf<LocalTransform>(), &local, diagnostics));
        MARLIN_EXPECT(local.Translation.X == 0 && local.Translation.Y == 0 && local.Translation.Z == 3);
        MARLIN_EXPECTF(
            Math::Abs(local.Rotation.Y - 0.70710678f) < 1e-5f && Math::Abs(local.Rotation.W - 0.70710678f) < 1e-5f &&
                Math::Abs(local.Rotation.X) < 1e-6f && Math::Abs(local.Rotation.Z) < 1e-6f,
            "rotation ({0}, {1}, {2}, {3})", local.Rotation.X, local.Rotation.Y, local.Rotation.Z, local.Rotation.W);
        MARLIN_EXPECT(local.Scale.X == 2 && local.Scale.Y == 2 && local.Scale.Z == 2);
    }

    // And one written by hand that merges every placed mesh into a slot per material.
    StringBuilder<40> binary;
    record.BinaryGuid.FormatTo(binary, {});
    const String text =
        Format("!Version \"1.0\"\n\nMeshSource\n{{\n    !UUID = \"3e500000-0000-0000-0000-000000000001\"\n"
               "    !Name = \"All\"\n    SourceFile = `source:Rocks.testscene#{0}`\n"
               "    MergePartsByMaterial = true\n"
               "    Slots = {{ Rock = <Rock#3e500000-0000-0000-0000-000000000099> }}\n}}\n",
               binary.AsView());
    MARLIN_REQUIRE(Vfs::WriteAll("/MeshTest/Content/All.masset", text.AsView()));
    // And the sphere again without a DAG.
    const String flatText =
        Format("!Version \"1.0\"\n\nMeshSource\n{{\n    !UUID = \"3e500000-0000-0000-0000-000000000002\"\n"
               "    !Name = \"Flat\"\n    SourceFile = `source:Rocks.testscene#{0}`\n    Mesh = \"Sphere\"\n"
               "    BuildDag = false\n}}\n",
               binary.AsView());
    MARLIN_REQUIRE(Vfs::WriteAll("/MeshTest/Content/Flat.masset", flatText.AsView()));
    scanner.ScanAll(changed);

    // RECOVERY-DECISION TM-D18: conversions read Engine.Meshes through the config, so the test initializes it on its
    // own (empty) folder, as the texture test does; the defaults of MeshSettings apply.
    MARLIN_REQUIRE(Vfs::CreateDirectory("/MeshTest/Config"));
    ConfigDesc desc;
    desc.Directories.Add(String("/MeshTest/Config"));
    desc.Variables.Add({"Platform", "Windows"});
    Config::Initialize(desc);

    IndexSourceResolver resolver(index);
    ConversionStore globalStore(ConversionStoreDesc::InProcess("/MeshTest/GlobalStore"));
    AssetConversion conversion(resolver, globalStore);
    ConversionTarget target;
    target.Platform = "Windows";
    const Guid sphereGuid = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, "Sphere");
    const Guid quadGuid = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, "Quad");
    const Guid boxGuid = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, "Box");
    const Guid allGuid = {0x3e500000, 0, 0, 1};
    const Guid flatGuid = {0x3e500000, 0, 0, 2};
    const Guid combinedGuid = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, ":Combined");
    const Guid prefabGuid = ImportContext::MakeUuid(TypeOf<MeshImporter>(), record.BinaryGuid, ":Prefab");
    {
        // The prefab flattens and captures: its links and components are well formed.
        const ConversionResult prefab = conversion.Convert(prefabGuid, target);
        MARLIN_EXPECTF(prefab.Succeeded, "{0}", prefab.Errors);
    }
    auto store = MakeUnique<MeshLocationStore>();
    // The bulk data of each, in the order of the list.
    List<List<uint8>> bulks;
    for (const Guid &guid : {sphereGuid, quadGuid, boxGuid, allGuid, flatGuid, combinedGuid})
    {
        const ConversionResult result = conversion.Convert(guid, target);
        MARLIN_REQUIREF(result.Succeeded, "{0}: {1}", guid, result.Errors);
        const String path = Format("/MeshTest/{0}.bin", guid);
        MARLIN_REQUIRE(globalStore.ExportResult(result.Key, path.AsView()));
        store->Add(guid, path.AsView(), result.Location);
        List<uint8> block;
        MARLIN_REQUIRE(globalStore.ReadResult(result.Key, block, bulks.AddDefaulted()));
    }

    Assets::Initialize();
    const AssetStoreId storeId = Assets::AddStore(Move(store), 0);
    {
        const TestSceneDecoder decoder;
        const uint32 triangles = 2 * decoder.Segments * (decoder.Rings - 1);

        const AssetRef<Mesh> rocks = Assets::LoadSync<Mesh>(sphereGuid);
        MARLIN_REQUIRE(rocks.Get() != nullptr);
        MARLIN_EXPECT(rocks->TriangleCount == triangles && rocks->Sections.Count() == 2);
        MARLIN_EXPECT(rocks->Sections[0].Material.AsView() == "Rock" && rocks->Sections[1].Material.AsView() == "Moss");
        MARLIN_EXPECT(rocks->HasTangents && rocks->TexCoordCount == 1 && !rocks->HasColors);
        MARLIN_EXPECT(rocks->AttributeStride == Mesh::GetAttributeStride(true, 1, false));
        // A 2 m sphere on an 18-bit grid: 2^-17 m steps.
        MARLIN_EXPECT(rocks->PositionStep == 1.0f / 131072.0f);
        MARLIN_EXPECT(rocks->Bounds.Max.X > 0.99f && rocks->Bounds.Max.X < 1.01f);
        MARLIN_EXPECT(rocks->DepthCount >= 3 && rocks->Groups.Count() > rocks->DepthCount);
        // MikkTSpace gave every corner a vertex of its own; welding merged them again, so the source clusters hold
        // far fewer vertices than the corners.
        uint32 sourceVertices = 0;
        for (const MeshClusterGroup &group : rocks->Groups)
        {
            for (uint32 c = 0; c < group.ClusterCount; ++c)
            {
                const MeshPageCluster cluster = ReadCluster(*rocks, bulks[0].AsSpan(), group, c);
                sourceVertices += cluster.RefinedGroup < 0 ? cluster.VertexCount : 0;
            }
        }
        MARLIN_EXPECT(sourceVertices < triangles * 3 / 2);
        MARLIN_EXPECT(CheckDag(*rocks, bulks[0].AsSpan()));
        // More than one page, so dependencies between them.
        MARLIN_EXPECT(rocks->Pages.Count() > 1 && !rocks->PageParents.IsEmpty());

        const AssetRef<Mesh> quad = Assets::LoadSync<Mesh>(quadGuid);
        MARLIN_REQUIRE(quad.Get() != nullptr);
        MARLIN_EXPECT(quad->TriangleCount == 2 && quad->ClusterCount == 1 && !quad->HasTangents);
        // Generated normals split a vertex only across a hard edge: the flat quad keeps its four.
        MARLIN_EXPECT(ReadCluster(*quad, bulks[1].AsSpan(), quad->Groups[0], 0).VertexCount == 4);
        MARLIN_EXPECT(quad->Sections.Count() == 1 && quad->Sections[0].Material.AsView() == "Rock");
        MARLIN_EXPECT(CheckDag(*quad, bulks[1].AsSpan()));

        const AssetRef<Mesh> box = Assets::LoadSync<Mesh>(boxGuid);
        MARLIN_REQUIRE(box.Get() != nullptr);
        MARLIN_EXPECT(box->TriangleCount == 12 && box->ClusterCount == 1 &&
                      ReadCluster(*box, bulks[2].AsSpan(), box->Groups[0], 0).VertexCount == 24);

        // The merged mesh is in scene space: the sphere 2 m along x, the quad under it; by material, two slots.
        const AssetRef<Mesh> all = Assets::LoadSync<Mesh>(allGuid);
        MARLIN_REQUIRE(all.Get() != nullptr);
        MARLIN_EXPECT(all->TriangleCount == triangles + 2 && all->Sections.Count() == 2);
        MARLIN_EXPECT(all->Sections[0].Part.AsView().IsEmpty() && all->Sections[1].Part.AsView().IsEmpty());
        // The Rock slot has the default material MeshSource.Slots gives it; Moss has none.
        const Guid rockMaterial = {0x3e500000, 0, 0, 0x99};
        for (const MeshSection &section : all->Sections)
        {
            MARLIN_EXPECT(section.DefaultMaterial.GetGuid() == (section.Material.AsView() == "Rock" ? rockMaterial : Guid()));
        }
        MARLIN_EXPECT(all->Bounds.Max.X > 2.99f && all->Bounds.Max.X < 3.01f && all->Bounds.Min.Y < -1.49f);
        MARLIN_EXPECT(CheckDag(*all, bulks[3].AsSpan()));

        // The imported combined mesh: the same triangles, a slot per part and material, so the floor's Rock is a slot
        // of its own.
        const AssetRef<Mesh> combined = Assets::LoadSync<Mesh>(combinedGuid);
        MARLIN_REQUIRE(combined.Get() != nullptr);
        MARLIN_EXPECT(combined->TriangleCount == triangles + 2);
        MARLIN_REQUIRE(combined->Sections.Count() == 3);
        const auto slot = [&](usize s) {
            return Format("{0}/{1}", combined->Sections[s].Part.AsView(), combined->Sections[s].Material.AsView());
        };
        MARLIN_EXPECT(slot(0) == "Rocks/Rock" && slot(1) == "Rocks/Moss" && slot(2) == "Floor/Rock");
        MARLIN_EXPECT(combined->Sections[2].TriangleCount == 2);
        MARLIN_EXPECT(CheckDag(*combined, bulks[5].AsSpan()));

        // Without a DAG: the sphere's source clusters alone, in the same format, every group terminal and every page
        // resident.
        const AssetRef<Mesh> flat = Assets::LoadSync<Mesh>(flatGuid);
        MARLIN_REQUIRE(flat.Get() != nullptr);
        MARLIN_EXPECT(flat->TriangleCount == triangles && flat->Sections.Count() == 2);
        MARLIN_EXPECT(flat->DepthCount == 1 && flat->RootPageCount == flat->Pages.Count());
        MARLIN_EXPECT(flat->ClusterCount < rocks->ClusterCount && flat->Groups.Count() > 1);
        for (const MeshClusterGroup &group : flat->Groups)
        {
            MARLIN_EXPECT(group.Depth == 0 && group.Simplified.Error >= __FLT_MAX__);
        }
        MARLIN_EXPECT(CheckDag(*flat, bulks[4].AsSpan()));
    }
    Assets::CollectGarbage();
    Assets::RemoveStore(storeId);
    Assets::Shutdown();
    Config::Shutdown();
}
} // namespace Marlin::Tests
