#include "Testing/Testing.h"

#include "ClusterLod/ClusterLod.h"
#include "Math/MathUtility.h"
#include "clusterlod.h"
#include "meshoptimizer.h"

namespace Marlin::Tests
{
namespace
{
/// A bumpy UV sphere with a seam where the longitude wraps: positions, then a normal, per vertex.
struct TestMesh
{
    static constexpr uint32 Floats = 6;
    List<float32> Vertices;
    List<uint32> Indices;
    uint32 VertexCount = 0;

    TestMesh(uint32 rings, uint32 segments)
    {
        for (uint32 ring = 0; ring <= rings; ++ring)
        {
            const float32 theta = Math::Pi * static_cast<float32>(ring) / static_cast<float32>(rings);
            for (uint32 segment = 0; segment <= segments; ++segment)
            {
                const float32 phi = Math::TwoPi * static_cast<float32>(segment) / static_cast<float32>(segments);
                const Vector3 normal(Math::Sin(theta) * Math::Cos(phi), Math::Cos(theta),
                                     Math::Sin(theta) * Math::Sin(phi));
                // Bumps, so that simplification has error to measure.
                const float32 radius = 1 + 0.05f * Math::Sin(7 * theta) * Math::Cos(5 * phi);
                Vertices.AddRange(
                    {normal.X * radius, normal.Y * radius, normal.Z * radius, normal.X, normal.Y, normal.Z});
            }
        }
        VertexCount = (rings + 1) * (segments + 1);
        const auto at = [segments](uint32 ring, uint32 segment) { return ring * (segments + 1) + segment; };
        for (uint32 ring = 0; ring < rings; ++ring)
        {
            for (uint32 segment = 0; segment < segments; ++segment)
            {
                Indices.AddRange({at(ring, segment), at(ring, segment + 1), at(ring + 1, segment)});
                Indices.AddRange({at(ring, segment + 1), at(ring + 1, segment + 1), at(ring + 1, segment)});
            }
        }
    }
};

struct ClodGroup
{
    clodGroup Group;
    List<clodCluster> Clusters;
    List<List<uint32>> Indices;
};

constexpr float32 Weights[3] = {0.5f, 0.5f, 0.5f};

clodMesh ToClod(const TestMesh &mesh)
{
    clodMesh input = {};
    input.indices = mesh.Indices.Data();
    input.index_count = mesh.Indices.Count();
    input.vertex_count = mesh.VertexCount;
    input.vertex_positions = mesh.Vertices.Data();
    input.vertex_positions_stride = TestMesh::Floats * sizeof(float32);
    input.vertex_attributes = mesh.Vertices.Data() + 3;
    input.vertex_attributes_stride = TestMesh::Floats * sizeof(float32);
    input.attribute_weights = Weights;
    input.attribute_count = 3;
    return input;
}

ClusterLodInput ToInput(const TestMesh &mesh)
{
    ClusterLodInput input;
    input.Indices = mesh.Indices.AsSpan();
    input.VertexCount = mesh.VertexCount;
    input.Positions = mesh.Vertices.Data();
    input.PositionStride = TestMesh::Floats * sizeof(float32);
    input.Attributes = mesh.Vertices.Data() + 3;
    input.AttributeStride = TestMesh::Floats * sizeof(float32);
    input.AttributeWeights = Span<const float32>(Weights, 3);
    return input;
}

/// The settings MeshConverter uses, as clodDefaultConfig with regular simplification.
ClusterLodSettings ConverterSettings(clodConfig &config)
{
    config = clodDefaultConfig(128);
    config.simplify_permissive = false;
    ClusterLodSettings settings;
    settings.MaxVertices = static_cast<uint32>(config.max_vertices);
    settings.MinTriangles = static_cast<uint32>(config.min_triangles);
    settings.MaxTriangles = static_cast<uint32>(config.max_triangles);
    settings.Permissive = false;
    return settings;
}

bool SameIndices(Span<const uint32> a, Span<const uint32> b)
{
    if (a.Count() != b.Count())
    {
        return false;
    }
    for (usize i = 0; i < a.Count(); ++i)
    {
        if (a[i] != b[i])
        {
            return false;
        }
    }
    return true;
}

bool SameBounds(const clodBounds &a, const ClusterLodBounds &b)
{
    return a.center[0] == b.Center.X && a.center[1] == b.Center.Y && a.center[2] == b.Center.Z &&
           a.radius == b.Radius && a.error == b.Error;
}
} // namespace

MARLIN_TEST(ClusterLod, BuildsWhatClusterlodBuilds)
{
    const TestMesh mesh(96, 192);
    clodConfig config;
    const ClusterLodSettings settings = ConverterSettings(config);

    List<ClodGroup> expected;
    clodBuild(config, ToClod(mesh), [&](clodGroup group, const clodCluster *clusters, size_t count) {
        ClodGroup &out = expected.AddDefaulted();
        out.Group = group;
        for (size_t c = 0; c < count; ++c)
        {
            out.Clusters.Add(clusters[c]);
            out.Indices.AddDefaulted().AddRange(Span<const uint32>(clusters[c].indices, clusters[c].index_count));
        }
        return static_cast<int>(expected.Count() - 1);
    });

    ClusterLodResult result;
    BuildClusterLod(ToInput(mesh), settings, result);
    MARLIN_REQUIRE(expected.Count() > 20 && result.Groups.Count() == expected.Count());
    uint32 depthCount = 0;
    for (usize g = 0; g < expected.Count(); ++g)
    {
        const ClodGroup &want = expected[g];
        const ClusterLodGroup &got = result.Groups[g];
        MARLIN_REQUIREF(static_cast<int>(got.Depth) == want.Group.depth &&
                            SameBounds(want.Group.simplified, got.Simplified) &&
                            got.ClusterCount == want.Clusters.Count(),
                        "group {0} differs", g);
        depthCount = Max(depthCount, got.Depth + 1);
        for (usize c = 0; c < want.Clusters.Count(); ++c)
        {
            const clodCluster &wantCluster = want.Clusters[c];
            const ClusterLodCluster &gotCluster = result.Clusters[got.FirstCluster + c];
            const Span<const uint32> indices =
                result.Indices.AsSpan().Slice(gotCluster.FirstIndex, gotCluster.IndexCount);
            MARLIN_REQUIREF(gotCluster.Refined == wantCluster.refined && gotCluster.Group == g &&
                                SameBounds(wantCluster.bounds, gotCluster.Bounds) &&
                                gotCluster.VertexCount == wantCluster.vertex_count &&
                                SameIndices(indices, want.Indices[c].AsSpan()),
                            "cluster {0} of group {1} differs", c, g);
        }
    }
    MARLIN_EXPECT(result.DepthCount == depthCount && depthCount >= 4);

    // The hierarchy is clodBuildHierarchy's too.
    List<clodGroup> groups;
    for (const ClodGroup &group : expected)
    {
        groups.Add(group.Group);
    }
    List<clodNode> wantNodes;
    wantNodes.Resize(clodBuildHierarchyBound(groups.Count(), 8, depthCount));
    wantNodes.Resize(clodBuildHierarchy(wantNodes.Data(), groups.Data(), groups.Count(), 8, depthCount));
    List<ClusterLodNode> nodes;
    BuildClusterLodHierarchy(result.Groups.AsSpan(), depthCount, 8, nodes);
    MARLIN_REQUIRE(nodes.Count() == wantNodes.Count());
    for (usize n = 0; n < nodes.Count(); ++n)
    {
        MARLIN_REQUIREF(nodes[n].Group == wantNodes[n].group && nodes[n].FirstChild == wantNodes[n].child_offset &&
                            nodes[n].ChildCount == wantNodes[n].child_count &&
                            SameBounds(wantNodes[n].bounds, nodes[n].Bounds),
                        "node {0} differs", n);
    }
}

MARLIN_TEST(ClusterLod, ClustersCoverTheMeshAndLocalIndicesRoundTrip)
{
    const TestMesh mesh(24, 48);
    clodConfig config;
    ClusterLodResult result;
    BuildClusterLod(ToInput(mesh), ConverterSettings(config), result);
    uint32 sourceTriangles = 0;
    List<uint32> vertices;
    List<uint8> triangles;
    for (const ClusterLodCluster &cluster : result.Clusters)
    {
        MARLIN_EXPECT(cluster.IndexCount <= 128 * 3 && cluster.VertexCount <= 128);
        sourceTriangles += cluster.Refined < 0 ? cluster.IndexCount / 3 : 0;
        const Span<const uint32> indices = result.Indices.AsSpan().Slice(cluster.FirstIndex, cluster.IndexCount);
        GetClusterLocalIndices(indices, vertices, triangles);
        MARLIN_EXPECT(vertices.Count() == cluster.VertexCount);
        for (usize i = 0; i < indices.Count(); ++i)
        {
            MARLIN_EXPECT(vertices[triangles[i]] == indices[i]);
        }
    }
    MARLIN_EXPECT(sourceTriangles == mesh.Indices.Count() / 3);
    // The last group is the single coarsest cluster.
    MARLIN_EXPECT(result.Groups.Last().ClusterCount == 1 && result.Groups.Last().Simplified.Error >= __FLT_MAX__);
}

MARLIN_TEST(ClusterLod, WithoutDagOnlyTheSourceClustersComeOut)
{
    const TestMesh mesh(48, 96);
    clodConfig config;
    ClusterLodSettings settings = ConverterSettings(config);
    ClusterLodResult dag;
    BuildClusterLod(ToInput(mesh), settings, dag);
    settings.BuildDag = false;
    ClusterLodResult flat;
    BuildClusterLod(ToInput(mesh), settings, flat);
    // The same clusters as the DAG's finest level, grouped all the same, every group terminal.
    uint32 sourceClusters = 0;
    for (const ClusterLodCluster &cluster : dag.Clusters)
    {
        sourceClusters += cluster.Refined < 0 ? 1 : 0;
    }
    MARLIN_EXPECT(flat.DepthCount == 1);
    MARLIN_EXPECTF(flat.Groups.Count() > 1, "{0} groups", flat.Groups.Count());
    MARLIN_EXPECTF(flat.Clusters.Count() == sourceClusters, "{0} clusters, {1} in the DAG", flat.Clusters.Count(),
                   sourceClusters);
    uint32 triangles = 0;
    for (const ClusterLodGroup &group : flat.Groups)
    {
        MARLIN_EXPECT(group.Depth == 0 && group.Simplified.Error >= __FLT_MAX__ && group.ClusterCount > 0);
    }
    for (const ClusterLodCluster &cluster : flat.Clusters)
    {
        MARLIN_EXPECT(cluster.Refined == -1 && cluster.IndexCount <= 128 * 3 && cluster.VertexCount <= 128);
        triangles += cluster.IndexCount / 3;
    }
    MARLIN_EXPECT(triangles == mesh.Indices.Count() / 3);
}

MARLIN_TEST(ClusterLod, LargeMeshesClusterInParallelChunks)
{
    // Just over a million triangles, so the first clustering splits into two runs.
    const TestMesh mesh(512, 1040);
    MARLIN_REQUIRE(mesh.Indices.Count() / 3 > (usize{1} << 20));
    clodConfig config;
    ClusterLodResult result;
    BuildClusterLod(ToInput(mesh), ConverterSettings(config), result);
    // The source clusters cover the triangles, refinements point back, and the DAG ends in one cluster.
    uint32 sourceTriangles = 0;
    for (const ClusterLodCluster &cluster : result.Clusters)
    {
        MARLIN_EXPECT(cluster.IndexCount <= 128 * 3 && cluster.VertexCount <= 128 && cluster.IndexCount > 0);
        sourceTriangles += cluster.Refined < 0 ? cluster.IndexCount / 3 : 0;
        MARLIN_EXPECT(cluster.Refined < 0 || static_cast<uint32>(cluster.Refined) < cluster.Group);
    }
    MARLIN_EXPECT(sourceTriangles == mesh.Indices.Count() / 3);
    MARLIN_EXPECT(result.Groups.Last().ClusterCount == 1 && result.DepthCount > 6);
}

MARLIN_TEST(ClusterLod, EmptyAndSingleClusterMeshes)
{
    clodConfig config;
    const ClusterLodSettings settings = ConverterSettings(config);
    ClusterLodResult result;
    BuildClusterLod(ClusterLodInput(), settings, result);
    MARLIN_EXPECT(result.Groups.IsEmpty() && result.Clusters.IsEmpty());

    // Two triangles fit one cluster, which is its own terminal group.
    const float32 positions[12] = {0, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1};
    const uint32 indices[6] = {0, 2, 1, 0, 3, 2};
    ClusterLodInput input;
    input.Indices = Span<const uint32>(indices, 6);
    input.VertexCount = 4;
    input.Positions = positions;
    BuildClusterLod(input, settings, result);
    MARLIN_REQUIRE(result.Groups.Count() == 1 && result.Clusters.Count() == 1);
    MARLIN_EXPECT(result.Groups[0].Depth == 0 && result.Groups[0].Simplified.Error >= __FLT_MAX__);
    MARLIN_EXPECT(result.Clusters[0].Refined == -1 && result.Clusters[0].IndexCount == 6 && result.DepthCount == 1);
}
} // namespace Marlin::Tests
