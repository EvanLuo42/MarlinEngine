#include "ClusterLod/ClusterLod.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/Jobs.h"
#include "Math/MathUtility.h"
#include "Templates/Utility.h"
#include "meshoptimizer.h"

// The algorithm follows clusterlod.h from meshoptimizer's demo folder, Copyright (C) 2016-2026 Arseny Kapoulkine, under
// the MIT License (the notice is at the end of this file), restructured so that each level's groups simplify in
// parallel, and on Marlin's containers.

namespace Marlin
{
namespace
{
static_assert(sizeof(Vector3) == 3 * sizeof(float32), "bounds are read as float arrays");

constexpr float32 TerminalError = __FLT_MAX__;

/// A cluster while the DAG builds: its triangles at IndexOffset in its level's index list.
struct Cluster
{
    uint32 Vertices = 0;
    usize IndexOffset = 0;
    uint32 IndexCount = 0;
    int32 Refined = -1;
    ClusterLodBounds Bounds;
};

/// What one thread reuses from group to group.
struct Scratch
{
    List<meshopt_Meshlet> Meshlets;
    List<uint32> MeshletVertices;
    List<uint8> MeshletTriangles;
    List<uint32> Merged;
    List<uint32> Simplified;
};

/// Calls *body(index, scratch)* for each index below *count* on the job system's workers and the calling thread, each
/// with a Scratch of its own, and waits. Jobs get large stacks: meshoptimizer keeps some working sets on the stack.
template <typename TBody> void ForEachInParallel(uint32 count, const TBody &body)
{
    if (count == 0)
    {
        return;
    }
    struct Context
    {
        const TBody *Body = nullptr;
        uint32 Count = 0;
        Atomic<uint32> Next{0};
    };
    Context context{&body, count};
    const JobEntry entry = [](void *argument) {
        auto *shared = static_cast<Context *>(argument);
        Scratch scratch;
        for (uint32 index = shared->Next.FetchAdd(1, MemoryOrder::Relaxed); index < shared->Count;
             index = shared->Next.FetchAdd(1, MemoryOrder::Relaxed))
        {
            (*shared->Body)(index, scratch);
        }
    };
    const uint32 jobCount = Min(count, static_cast<uint32>(JobSystem::GetWorkerCount() + 1));
    List<JobDecl> jobs;
    jobs.Resize(jobCount, JobDecl{.Entry = entry, .Argument = &context, .LargeStack = true});
    JobCounter counter;
    Jobs::Run(jobs.AsSpan(), &counter);
    Jobs::Wait(counter);
}

ClusterLodBounds ComputeBounds(const ClusterLodInput &input, Span<const uint32> indices, float32 error)
{
    const meshopt_Bounds bounds = meshopt_computeClusterBounds(indices.Data(), indices.Count(), input.Positions,
                                                               input.VertexCount, input.PositionStride);
    ClusterLodBounds result;
    result.Center = Vector3(bounds.center[0], bounds.center[1], bounds.center[2]);
    result.Radius = bounds.radius;
    result.Error = error;
    return result;
}

/// A sphere around *count* spheres *stride* bytes apart, with the largest of their errors.
ClusterLodBounds MergeBounds(const ClusterLodBounds *bounds, usize count, usize stride)
{
    const meshopt_Bounds merged =
        meshopt_computeSphereBounds(&bounds->Center.X, count, stride, &bounds->Radius, stride);
    ClusterLodBounds result;
    result.Center = Vector3(merged.center[0], merged.center[1], merged.center[2]);
    result.Radius = merged.radius;
    for (usize i = 0; i < count; ++i)
    {
        const auto *at =
            reinterpret_cast<const ClusterLodBounds *>(reinterpret_cast<const uint8 *>(bounds) + i * stride);
        result.Error = Max(result.Error, at->Error);
    }
    return result;
}

/// Clusters *indices* and appends the clusters to *clusters* and their triangles to *clusterIndices*.
void Clusterize(const ClusterLodInput &input, const ClusterLodSettings &settings, Span<const uint32> indices,
                int32 refined, List<Cluster> &clusters, List<uint32> &clusterIndices, Scratch &scratch)
{
    const usize maxMeshlets = meshopt_buildMeshletsBound(indices.Count(), settings.MaxVertices, settings.MinTriangles);
    scratch.Meshlets.ResizeUninitialized(maxMeshlets);
    scratch.MeshletVertices.ResizeUninitialized(indices.Count());
    scratch.MeshletTriangles.ResizeUninitialized(indices.Count());
    const usize meshletCount =
        settings.ClusterSpatially
            ? meshopt_buildMeshletsSpatial(
                  scratch.Meshlets.Data(), scratch.MeshletVertices.Data(), scratch.MeshletTriangles.Data(),
                  indices.Data(), indices.Count(), input.Positions, input.VertexCount, input.PositionStride,
                  settings.MaxVertices, settings.MinTriangles, settings.MaxTriangles, settings.FillWeight)
            : meshopt_buildMeshletsFlex(scratch.Meshlets.Data(), scratch.MeshletVertices.Data(),
                                        scratch.MeshletTriangles.Data(), indices.Data(), indices.Count(),
                                        input.Positions, input.VertexCount, input.PositionStride, settings.MaxVertices,
                                        settings.MinTriangles, settings.MaxTriangles, 0, settings.SplitFactor);
    usize indexOffset = clusterIndices.Count();
    clusterIndices.ResizeUninitialized(indexOffset + indices.Count());
    clusters.Reserve(clusters.Count() + meshletCount);
    for (usize m = 0; m < meshletCount; ++m)
    {
        const meshopt_Meshlet &meshlet = scratch.Meshlets[m];
        uint32 *vertices = scratch.MeshletVertices.Data() + meshlet.vertex_offset;
        uint8 *triangles = scratch.MeshletTriangles.Data() + meshlet.triangle_offset;
        if (settings.OptimizeClusters)
        {
            meshopt_optimizeMeshletLevel(vertices, meshlet.vertex_count, triangles, meshlet.triangle_count,
                                         static_cast<int>(settings.OptimizeLevel));
        }
        Cluster &cluster = clusters.AddDefaulted();
        cluster.Vertices = meshlet.vertex_count;
        cluster.IndexOffset = indexOffset;
        cluster.IndexCount = meshlet.triangle_count * 3;
        cluster.Refined = refined;
        // The cluster-local indices go; GetClusterLocalIndices recovers them.
        for (uint32 i = 0; i < cluster.IndexCount; ++i)
        {
            clusterIndices[indexOffset + i] = vertices[triangles[i]];
        }
        indexOffset += cluster.IndexCount;
    }
    clusterIndices.Resize(indexOffset);
}

/// Clusters the input's triangles. Clustering is one pass over all of them, so a mesh above ChunkThreshold triangles
/// is first sorted along a space-filling curve and cut into runs of ChunkTriangles, which cluster in parallel: the
/// clusters at the cuts may be a little less full, and the cuts depend on the mesh alone, never on the workers.
/// Smaller meshes cluster whole.
void ClusterizeInput(const ClusterLodInput &input, const ClusterLodSettings &settings, List<Cluster> &clusters,
                     List<uint32> &clusterIndices)
{
    constexpr usize ChunkThreshold = usize{1} << 20;
    // Small enough that the runs share the workers evenly; large enough that few clusters sit at a cut.
    constexpr usize ChunkTriangles = usize{1} << 18;
    const usize triangleCount = input.Indices.Count() / 3;
    if (triangleCount <= ChunkThreshold)
    {
        Scratch scratch;
        Clusterize(input, settings, input.Indices, -1, clusters, clusterIndices, scratch);
        return;
    }
    List<uint32> sorted;
    sorted.ResizeUninitialized(input.Indices.Count());
    meshopt_spatialSortTriangles(sorted.Data(), input.Indices.Data(), input.Indices.Count(), input.Positions,
                                 input.VertexCount, input.PositionStride);
    struct Chunk
    {
        List<Cluster> Clusters;
        List<uint32> Indices;
    };
    List<Chunk> chunks;
    chunks.Resize((triangleCount + ChunkTriangles - 1) / ChunkTriangles);
    ForEachInParallel(static_cast<uint32>(chunks.Count()), [&](uint32 c, Scratch &scratch) {
        const usize first = usize{c} * ChunkTriangles * 3;
        const usize count = Min(ChunkTriangles * 3, sorted.Count() - first);
        Clusterize(input, settings, sorted.AsSpan().Slice(first, count), -1, chunks[c].Clusters, chunks[c].Indices,
                   scratch);
    });
    sorted = List<uint32>();
    for (Chunk &chunk : chunks)
    {
        const usize indexBase = clusterIndices.Count();
        for (Cluster &cluster : chunk.Clusters)
        {
            cluster.IndexOffset += indexBase;
            clusters.Add(cluster);
        }
        clusterIndices.AddRange(chunk.Indices.AsSpan());
        chunk = Chunk();
    }
}

/// Sorts *clusters* into groups and returns where each group starts, and one past the last.
List<uint32> Partition(const ClusterLodInput &input, const ClusterLodSettings &settings, List<Cluster> &clusters,
                       const List<uint32> &clusterIndices, const List<uint32> &remap)
{
    List<uint32> offsets;
    if (clusters.Count() <= settings.GroupSize)
    {
        offsets.AddRange({0u, static_cast<uint32>(clusters.Count())});
        return offsets;
    }
    // The clusters' triangles with every vertex replaced by the first at its position, so that seams connect.
    List<uint32> counts;
    counts.ResizeUninitialized(clusters.Count());
    List<usize> starts;
    starts.ResizeUninitialized(clusters.Count());
    usize total = 0;
    for (usize c = 0; c < clusters.Count(); ++c)
    {
        counts[c] = clusters[c].IndexCount;
        starts[c] = total;
        total += clusters[c].IndexCount;
    }
    List<uint32> remapped;
    remapped.ResizeUninitialized(total);
    Jobs::ParallelFor(static_cast<uint32>(clusters.Count()), 1024, [&](uint32 begin, uint32 end) {
        for (uint32 c = begin; c < end; ++c)
        {
            const Cluster &cluster = clusters[c];
            for (uint32 i = 0; i < cluster.IndexCount; ++i)
            {
                remapped[starts[c] + i] = remap[clusterIndices[cluster.IndexOffset + i]];
            }
        }
    });
    List<uint32> part;
    part.ResizeUninitialized(clusters.Count());
    const usize partCount = meshopt_partitionClusters(
        part.Data(), remapped.Data(), remapped.Count(), counts.Data(), counts.Count(),
        settings.GroupSpatially ? input.Positions : nullptr, remap.Count(), input.PositionStride, settings.GroupSize);
    if (settings.SortGroups)
    {
        // Any point of a group will do for sorting; the last cluster's centre is at hand.
        List<float32> points;
        points.ResizeUninitialized(partCount * 3);
        for (usize c = 0; c < clusters.Count(); ++c)
        {
            const Vector3 &center = clusters[c].Bounds.Center;
            points[part[c] * 3] = center.X;
            points[part[c] * 3 + 1] = center.Y;
            points[part[c] * 3 + 2] = center.Z;
        }
        List<uint32> order;
        order.ResizeUninitialized(partCount);
        meshopt_spatialSortRemap(order.Data(), points.Data(), partCount, 3 * sizeof(float32));
        for (uint32 &p : part)
        {
            p = order[p];
        }
    }
    offsets.Resize(partCount + 1, 0);
    for (const uint32 p : part)
    {
        ++offsets[p + 1];
    }
    for (usize p = 1; p <= partCount; ++p)
    {
        offsets[p] += offsets[p - 1];
    }
    List<uint32> write;
    write.AddRange(offsets.AsSpan());
    List<Cluster> sorted;
    sorted.Resize(clusters.Count());
    for (usize c = 0; c < clusters.Count(); ++c)
    {
        sorted[write[part[c]]++] = clusters[c];
    }
    clusters = Move(sorted);
    return offsets;
}

/// Locks every vertex on a border between groups, and every vertex at the same position as one, in bit 0 of *locks*,
/// so that groups simplify without opening cracks between them; keeps the Protect bits and adds the input's locks.
/// A position is on a border when more than one group uses it, whatever the order, so the groups mark their positions
/// in parallel: the first to come claims a position, and any other group finding it claimed marks it shared.
void LockBorders(const ClusterLodInput &input, List<uint8> &locks, const List<Cluster> &clusters,
                 const List<uint32> &groupOffsets, const List<uint32> &clusterIndices, const List<uint32> &remap,
                 List<uint32> &owners)
{
    constexpr uint32 Unclaimed = ~0u;
    constexpr uint32 Shared = ~1u;
    owners.ResizeUninitialized(locks.Count());
    Jobs::ParallelFor(static_cast<uint32>(owners.Count()), 65536, [&](uint32 begin, uint32 end) {
        for (uint32 v = begin; v < end; ++v)
        {
            owners[v] = Unclaimed;
        }
    });
    Jobs::ParallelFor(static_cast<uint32>(groupOffsets.Count() - 1), 16, [&](uint32 begin, uint32 end) {
        for (uint32 g = begin; g < end; ++g)
        {
            for (uint32 c = groupOffsets[g]; c < groupOffsets[g + 1]; ++c)
            {
                const Cluster &cluster = clusters[c];
                for (uint32 i = 0; i < cluster.IndexCount; ++i)
                {
                    const AtomicRef<uint32> owner(owners[remap[clusterIndices[cluster.IndexOffset + i]]]);
                    uint32 seen = owner.Load(MemoryOrder::Relaxed);
                    while (seen != g && seen != Shared &&
                           !owner.CompareExchangeWeak(seen, seen == Unclaimed ? g : Shared, MemoryOrder::Relaxed))
                    {
                    }
                }
            }
        }
    });
    Jobs::ParallelFor(static_cast<uint32>(locks.Count()), 65536, [&](uint32 begin, uint32 end) {
        for (uint32 v = begin; v < end; ++v)
        {
            uint8 lock =
                static_cast<uint8>((owners[remap[v]] == Shared ? 1 : 0) | (locks[v] & meshopt_SimplifyVertex_Protect));
            if (!input.VertexLocks.IsEmpty())
            {
                lock |= input.VertexLocks[v];
            }
            locks[v] = lock;
        }
    });
}

/// Sloppy simplification of a subset of the mesh: deindexed first, since without sparsity it would cost as much as
/// the whole vertex buffer. *error* comes back absolute.
void SimplifySloppy(const ClusterLodInput &input, Span<const uint32> indices, const List<uint8> &locks,
                    usize targetCount, List<uint32> &result, float32 &error)
{
    struct SloppyVertex
    {
        float32 X, Y, Z;
        uint32 Id;
    };
    List<SloppyVertex> subset;
    List<uint8> subsetLocks;
    subset.ResizeUninitialized(indices.Count());
    subsetLocks.ResizeUninitialized(indices.Count());
    result.ResizeUninitialized(indices.Count());
    const usize stride = input.PositionStride / sizeof(float32);
    for (usize i = 0; i < indices.Count(); ++i)
    {
        const uint32 v = indices[i];
        const float32 *position = input.Positions + usize{v} * stride;
        subset[i] = {position[0], position[1], position[2], v};
        subsetLocks[i] = locks[v];
        result[i] = static_cast<uint32>(i);
    }
    result.Resize(meshopt_simplifySloppy(result.Data(), result.Data(), result.Count(), &subset[0].X, subset.Count(),
                                         sizeof(SloppyVertex), subsetLocks.Data(), targetCount, TerminalError, &error));
    error *= meshopt_simplifyScale(&subset[0].X, subset.Count(), sizeof(SloppyVertex));
    for (uint32 &index : result)
    {
        index = subset[index].Id;
    }
}

/// Simplifies *indices* towards *targetCount* indices into *result*, with its absolute error.
void Simplify(const ClusterLodInput &input, const ClusterLodSettings &settings, Span<const uint32> indices,
              const List<uint8> &locks, usize targetCount, List<uint32> &result, float32 &error)
{
    uint32 options = meshopt_SimplifySparse | meshopt_SimplifyErrorAbsolute;
    options |= settings.ClampError ? meshopt_SimplifyErrorClamped : 0u;
    options |= settings.Permissive ? meshopt_SimplifyPermissive : 0u;
    options |= settings.Regularize ? meshopt_SimplifyRegularize : 0u;
    options |= settings.PreserveFolds ? meshopt_SimplifyPreserveFolds : 0u;
    const auto simplify = [&](uint32 flags) {
        result.ResizeUninitialized(indices.Count());
        result.Resize(meshopt_simplifyWithAttributes(
            result.Data(), indices.Data(), indices.Count(), input.Positions, input.VertexCount, input.PositionStride,
            input.Attributes, input.AttributeStride, input.AttributeWeights.Data(), input.AttributeWeights.Count(),
            locks.Data(), targetCount, TerminalError, flags, &error));
    };
    simplify(options);
    if (result.Count() > targetCount && settings.PermissiveFallback && !settings.Permissive)
    {
        simplify(options | meshopt_SimplifyPermissive);
    }
    // Sloppy simplification has no sparsity or absolute error, hence the subset.
    if (result.Count() > targetCount && settings.SloppyFallback)
    {
        SimplifySloppy(input, indices, locks, targetCount, result, error);
        error *= settings.SloppyErrorFactor;
    }
    // An empty result would leave an empty cluster, which breaks the DAG: the first triangle stands in.
    if (result.IsEmpty() && !indices.IsEmpty())
    {
        result.AddRange(indices.Slice(0, 3));
    }
    if (settings.EdgeErrorLimit > 0)
    {
        // The shortest edge, or a quarter of the longest for thin triangles such as wires.
        const usize stride = input.PositionStride / sizeof(float32);
        float32 maxEdgeSquared = 0;
        for (usize i = 0; i + 2 < indices.Count(); i += 3)
        {
            const float32 *a = input.Positions + usize{indices[i]} * stride;
            const float32 *b = input.Positions + usize{indices[i + 1]} * stride;
            const float32 *c = input.Positions + usize{indices[i + 2]} * stride;
            const auto squared = [](const float32 *p, const float32 *q) {
                return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]);
            };
            const float32 ab = squared(a, b);
            const float32 ac = squared(a, c);
            const float32 bc = squared(b, c);
            const float32 longest = Max(Max(ab, ac), bc);
            const float32 shortest = Min(Min(ab, ac), bc);
            maxEdgeSquared = Max(maxEdgeSquared, Max(shortest, longest / 4));
        }
        error = Min(error, Math::Sqrt(maxEdgeSquared) * settings.EdgeErrorLimit);
    }
}

/// What simplifying one group gives: its bounds, and unless it is stuck, the clusters of the simplified triangles.
struct GroupResult
{
    ClusterLodBounds Bounds;
    bool Terminal = false;
    List<Cluster> Clusters;
    List<uint32> Indices;
};

/// Appends a group and its clusters, with their triangles, to *result*; returns the group's index.
int32 OutputGroup(const ClusterLodInput &input, const ClusterLodSettings &settings, const Cluster *clusters,
                  usize clusterCount, const List<uint32> &clusterIndices, const ClusterLodBounds &simplified,
                  uint32 depth, ClusterLodResult &result)
{
    const auto index = static_cast<uint32>(result.Groups.Count());
    ClusterLodGroup &group = result.Groups.AddDefaulted();
    group.Depth = depth;
    group.Simplified = simplified;
    group.FirstCluster = static_cast<uint32>(result.Clusters.Count());
    group.ClusterCount = static_cast<uint32>(clusterCount);
    result.DepthCount = Max(result.DepthCount, depth + 1);
    for (usize c = 0; c < clusterCount; ++c)
    {
        const Cluster &cluster = clusters[c];
        const Span<const uint32> indices = clusterIndices.AsSpan().Slice(cluster.IndexOffset, cluster.IndexCount);
        ClusterLodCluster &out = result.Clusters.AddDefaulted();
        out.Refined = cluster.Refined;
        out.Bounds = settings.TightBounds && cluster.Refined != -1 ? ComputeBounds(input, indices, cluster.Bounds.Error)
                                                                   : cluster.Bounds;
        out.Group = index;
        out.FirstIndex = static_cast<uint32>(result.Indices.Count());
        out.IndexCount = cluster.IndexCount;
        out.VertexCount = cluster.Vertices;
        result.Indices.AddRange(indices);
    }
    return static_cast<int32>(index);
}
} // namespace

void BuildClusterLod(const ClusterLodInput &input, const ClusterLodSettings &settings, ClusterLodResult &result)
{
    result = ClusterLodResult();
    if (input.Indices.IsEmpty())
    {
        return;
    }
    List<uint8> locks;
    locks.Resize(input.VertexCount, 0);
    // For connectivity across seams: each vertex to the first vertex at its position.
    List<uint32> remap;
    remap.ResizeUninitialized(input.VertexCount);
    meshopt_generatePositionRemap(remap.Data(), input.Positions, input.VertexCount, input.PositionStride);
    if (input.ProtectMask != 0)
    {
        // Protect the attribute seams of permissive simplification.
        const usize floats = input.AttributeStride / sizeof(float32);
        for (usize v = 0; v < input.VertexCount; ++v)
        {
            const uint32 r = remap[v];
            for (usize a = 0; a < floats && r != v; ++a)
            {
                if ((input.ProtectMask & (1u << a)) != 0 &&
                    input.Attributes[v * floats + a] != input.Attributes[usize{r} * floats + a])
                {
                    locks[v] |= meshopt_SimplifyVertex_Protect;
                }
            }
        }
    }

    // The input split into clusters; their bounds are exact, those of simplified clusters their group's.
    List<Cluster> clusters;
    List<uint32> clusterIndices;
    ClusterizeInput(input, settings, clusters, clusterIndices);
    Jobs::ParallelFor(static_cast<uint32>(clusters.Count()), 256, [&](uint32 begin, uint32 end) {
        for (uint32 c = begin; c < end; ++c)
        {
            Cluster &cluster = clusters[c];
            cluster.Bounds =
                ComputeBounds(input, clusterIndices.AsSpan().Slice(cluster.IndexOffset, cluster.IndexCount), 0);
        }
    });

    if (!settings.BuildDag && clusters.Count() > 1)
    {
        // Grouped all the same, for paging and culling; each group is its own coarsest version.
        const List<uint32> groupOffsets = Partition(input, settings, clusters, clusterIndices, remap);
        for (usize g = 0; g + 1 < groupOffsets.Count(); ++g)
        {
            const usize groupSize = groupOffsets[g + 1] - groupOffsets[g];
            ClusterLodBounds bounds = MergeBounds(&clusters[groupOffsets[g]].Bounds, groupSize, sizeof(Cluster));
            bounds.Error = TerminalError;
            OutputGroup(input, settings, clusters.Data() + groupOffsets[g], groupSize, clusterIndices, bounds, 0,
                        result);
        }
        return;
    }

    List<Cluster> pending;
    List<uint32> pendingIndices;
    List<uint32> owners;
    List<GroupResult> groups;
    for (uint32 depth = 0; clusters.Count() > 1; ++depth)
    {
        const List<uint32> groupOffsets = Partition(input, settings, clusters, clusterIndices, remap);
        LockBorders(input, locks, clusters, groupOffsets, clusterIndices, remap, owners);

        // Every group simplifies on its own, in parallel; the results go out in group order below, so they do not
        // depend on which finished first.
        const auto groupCount = static_cast<uint32>(groupOffsets.Count() - 1);
        groups.Clear();
        groups.Resize(groupCount);
        ForEachInParallel(groupCount, [&](uint32 g, Scratch &scratch) {
            const Cluster *group = clusters.Data() + groupOffsets[g];
            const usize groupSize = groupOffsets[g + 1] - groupOffsets[g];
            scratch.Merged.Clear();
            for (usize c = 0; c < groupSize; ++c)
            {
                scratch.Merged.AddRange(clusterIndices.AsSpan().Slice(group[c].IndexOffset, group[c].IndexCount));
            }
            const auto targetCount =
                static_cast<usize>(static_cast<float32>(scratch.Merged.Count() / 3) * settings.SimplifyRatio) * 3;
            // Bounds and error never shrink from a level to the next: merged from the clusters', not measured anew.
            GroupResult &out = groups[g];
            out.Bounds = MergeBounds(&group->Bounds, groupSize, sizeof(Cluster));
            float32 error = 0;
            Simplify(input, settings, scratch.Merged.AsSpan(), locks, targetCount, scratch.Simplified, error);
            if (static_cast<float32>(scratch.Simplified.Count()) >
                static_cast<float32>(scratch.Merged.Count()) * settings.SimplifyThreshold)
            {
                out.Bounds.Error = TerminalError;
                out.Terminal = true;
                return;
            }
            out.Bounds.Error =
                Max(out.Bounds.Error * settings.ErrorMergePrevious, error) + error * settings.ErrorMergeAdditive;
            Clusterize(input, settings, scratch.Simplified.AsSpan(), -1, out.Clusters, out.Indices, scratch);
            for (Cluster &cluster : out.Clusters)
            {
                cluster.Bounds = out.Bounds;
            }
        });

        pending.Clear();
        pendingIndices.Clear();
        for (uint32 g = 0; g < groupCount; ++g)
        {
            GroupResult &group = groups[g];
            const int32 index =
                OutputGroup(input, settings, clusters.Data() + groupOffsets[g], groupOffsets[g + 1] - groupOffsets[g],
                            clusterIndices, group.Bounds, depth, result);
            if (group.Terminal)
            {
                continue;
            }
            const usize indexBase = pendingIndices.Count();
            for (Cluster &cluster : group.Clusters)
            {
                cluster.Refined = index;
                cluster.IndexOffset += indexBase;
                pending.Add(cluster);
            }
            pendingIndices.AddRange(group.Indices.AsSpan());
        }
        clusters = Move(pending);
        clusterIndices = Move(pendingIndices);
        pending = List<Cluster>();
        pendingIndices = List<uint32>();
        if (clusters.Count() == 1)
        {
            // The one cluster left is the coarsest version of the whole mesh.
            ClusterLodBounds bounds = clusters[0].Bounds;
            bounds.Error = TerminalError;
            OutputGroup(input, settings, clusters.Data(), 1, clusterIndices, bounds, depth + 1, result);
        }
    }
    if (result.Groups.IsEmpty() && clusters.Count() == 1)
    {
        // A mesh of one cluster is its own coarsest version.
        ClusterLodBounds bounds = clusters[0].Bounds;
        bounds.Error = TerminalError;
        OutputGroup(input, settings, clusters.Data(), 1, clusterIndices, bounds, 0, result);
    }
}

void BuildClusterLodHierarchy(Span<const ClusterLodGroup> groups, uint32 depthCount, uint32 width,
                              List<ClusterLodNode> &nodes)
{
    width = Max(width, 2u);
    // Each tree's levels, padded by depthCount at every step for the unknown share of each depth.
    usize bound = depthCount;
    for (usize frontier = groups.Count(); frontier > 1; frontier = (frontier + width - 1) / width)
    {
        bound += frontier + depthCount;
    }
    nodes.Clear();
    nodes.Resize(bound);
    usize offset = depthCount;
    List<ClusterLodNode> row;
    List<uint32> order;
    order.ResizeUninitialized(groups.Count());
    for (uint32 depth = 0; depth < depthCount; ++depth)
    {
        // The groups of this depth are the leaves; each step up groups nearby centres.
        row.Clear();
        for (usize g = 0; g < groups.Count(); ++g)
        {
            if (groups[g].Depth == depth)
            {
                ClusterLodNode &leaf = row.AddDefaulted();
                leaf.Bounds = groups[g].Simplified;
                leaf.Group = static_cast<int32>(g);
            }
        }
        while (row.Count() > 1)
        {
            const usize count = row.Count();
            meshopt_spatialClusterPoints(order.Data(), &row[0].Bounds.Center.X, count, sizeof(ClusterLodNode), width);
            for (usize i = 0; i < count; ++i)
            {
                nodes[offset + i] = row[order[i]];
            }
            row.Clear();
            // Every cluster of points but the last is full.
            for (usize i = 0; i < count; i += width)
            {
                ClusterLodNode &parent = row.AddDefaulted();
                parent.ChildCount = static_cast<uint32>(Min<usize>(width, count - i));
                parent.FirstChild = static_cast<uint32>(offset + i);
                parent.Bounds = MergeBounds(&nodes[offset + i].Bounds, parent.ChildCount, sizeof(ClusterLodNode));
            }
            offset += count;
        }
        if (!row.IsEmpty())
        {
            nodes[depth] = row[0];
        }
    }
    nodes.Resize(offset);
}

void GetClusterLocalIndices(Span<const uint32> indices, List<uint32> &vertices, List<uint8> &triangles)
{
    vertices.ResizeUninitialized(indices.Count());
    triangles.ResizeUninitialized(indices.Count());
    vertices.Resize(meshopt_extractMeshletIndices(vertices.Data(), triangles.Data(), indices.Data(), indices.Count()));
}
} // namespace Marlin

/*
 * Copyright (c) 2016-2026 Arseny Kapoulkine
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
