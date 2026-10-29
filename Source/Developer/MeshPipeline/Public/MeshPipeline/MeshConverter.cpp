#include "MeshPipeline/MeshConverter.h"

#include "MeshPipeline/MeshImporter.h"

#include "ClusterLod/ClusterLod.h"
#include "Concurrency/Jobs.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "meshoptimizer.h"

namespace Marlin
{
namespace
{
/// Where each attribute of a vertex lies while converting, in floats. Every vertex has a tangent slot, filled even
/// when the mesh keeps no tangents, so that the normal and the UVs are one run of attributes for simplification.
struct VertexLayout
{
    uint32 TexCoords = 0;
    bool Colors = false;

    uint32 Position = 0;
    uint32 Normal = 3;
    uint32 TexCoord0 = 10;

    [[nodiscard]] uint32 Tangent() const
    {
        return Normal + 3;
    }
    [[nodiscard]] uint32 TexCoord(uint32 set) const
    {
        return TexCoord0 + set * 2;
    }
    [[nodiscard]] uint32 Color() const
    {
        return TexCoord0 + TexCoords * 2;
    }
    [[nodiscard]] uint32 Floats() const
    {
        return Color() + (Colors ? 4 : 0);
    }
};

/// The triangles of one slot, in VertexLayout::Floats floats per vertex.
struct SectionGeometry
{
    String Part;
    String Material;
    bool TwoSided = false;
    List<float32> Vertices;
    uint32 VertexCount = 0;
    List<uint32> Indices;
};

struct BuiltCluster
{
    ClusterLodBounds Bounds;
    int32 Refined = -1;
    uint32 Group = 0;
    uint32 Section = 0;
    List<uint32> Vertices;
    List<uint8> Triangles;
    Vector3 ConeAxis;
    float32 ConeCutoff = 0;
    float32 MaxEdgeLength = 0;
};

struct BuiltGroup
{
    ClusterLodGroup Group;
    List<uint32> Clusters;
};

Matrix4x4 WorldTransform(const DecodedScene &scene, int32 node)
{
    const auto count = static_cast<int32>(scene.Nodes.Count());
    Matrix4x4 transform = scene.Nodes[static_cast<usize>(node)].Transform;
    // At most one step per node, so a cycle in a broken file cannot loop forever.
    for (int32 parent = scene.Nodes[static_cast<usize>(node)].Parent, steps = 0;
         parent >= 0 && parent < count && steps < count;
         parent = scene.Nodes[static_cast<usize>(parent)].Parent, ++steps)
    {
        transform = scene.Nodes[static_cast<usize>(parent)].Transform * transform;
    }
    return transform;
}

/// *vector* at unit length, or zero for a zero vector. Vector3::Normalized takes anything shorter than
/// Math::SmallNumber for zero, which is every face normal of a mesh of millimetre triangles; the length is taken in
/// doubles here so that such tiny cross products neither underflow nor vanish.
Vector3 UnitOrZero(const Vector3 &vector)
{
    const float64 lengthSquared =
        float64{vector.X} * vector.X + float64{vector.Y} * vector.Y + float64{vector.Z} * vector.Z;
    if (lengthSquared <= 0)
    {
        return Vector3();
    }
    const auto scale = static_cast<float32>(1 / Math::Sqrt(lengthSquared));
    return Vector3(vector.X * scale, vector.Y * scale, vector.Z * scale);
}

/// Normals for a mesh that has none, with edges sharper than *creaseAngle* kept hard: around each vertex the triangles
/// fall into groups whose first triangle is within the angle of each other one, and each group becomes a vertex of
/// its own with the area-weighted normal of its triangles; a degenerate triangle joins the vertex's first group.
/// Writes, for each new vertex, the source vertex it came from and its normal, and for each corner the new vertex it
/// uses; no more vertices than a vertex has hard edges need.
void GenerateCreaseNormals(Span<const Vector3> positions, Span<const uint32> indices, float32 creaseAngle,
                           List<uint32> &sources, List<Vector3> &normals, List<uint32> &corners)
{
    const usize vertexCount = positions.Count();
    const usize triangleCount = indices.Count() / 3;
    List<Vector3> faceNormals;
    faceNormals.Resize(triangleCount);
    Jobs::ParallelFor(static_cast<uint32>(triangleCount), 16384, [&](uint32 begin, uint32 end) {
        for (usize t = begin; t < end; ++t)
        {
            const Vector3 &a = positions[indices[t * 3]];
            faceNormals[t] = Vector3::Cross(positions[indices[t * 3 + 1]] - a, positions[indices[t * 3 + 2]] - a);
        }
    });
    // The corners around each vertex, in corner order: first[v] to first[v + 1] in around.
    List<uint32> first;
    first.Resize(vertexCount + 1, 0);
    for (const uint32 vertex : indices)
    {
        ++first[vertex + 1];
    }
    for (usize v = 0; v < vertexCount; ++v)
    {
        first[v + 1] += first[v];
    }
    List<uint32> around;
    around.ResizeUninitialized(indices.Count());
    {
        List<uint32> next(first);
        for (usize c = 0; c < indices.Count(); ++c)
        {
            around[next[indices[c]]++] = static_cast<uint32>(c);
        }
    }
    // Each vertex groups its corners on its own, in parallel: first each corner's group among its vertex's, and how
    // many groups each vertex has; the groups then become vertices in vertex order, so the result does not depend on
    // the workers.
    const float32 cosine = Math::Cos(creaseAngle);
    corners.ResizeUninitialized(indices.Count());
    List<uint32> groupFirst;
    groupFirst.Resize(vertexCount + 1, 0);
    Jobs::ParallelFor(static_cast<uint32>(vertexCount), 4096, [&](uint32 begin, uint32 end) {
        List<Vector3> seeds;
        for (uint32 v = begin; v < end; ++v)
        {
            seeds.Clear();
            for (uint32 i = first[v]; i < first[v + 1]; ++i)
            {
                const uint32 corner = around[i];
                const Vector3 unit = UnitOrZero(faceNormals[corner / 3]);
                const bool degenerate = unit.X == 0 && unit.Y == 0 && unit.Z == 0;
                usize group = degenerate && !seeds.IsEmpty() ? 0 : List<Vector3>::NotFound;
                for (usize s = 0; group == List<Vector3>::NotFound && s < seeds.Count(); ++s)
                {
                    group = Vector3::Dot(seeds[s], unit) >= cosine ? s : group;
                }
                if (group == List<Vector3>::NotFound)
                {
                    group = seeds.Count();
                    seeds.Add(unit);
                }
                corners[corner] = static_cast<uint32>(group);
            }
            groupFirst[v + 1] = static_cast<uint32>(seeds.Count());
        }
    });
    for (usize v = 0; v < vertexCount; ++v)
    {
        groupFirst[v + 1] += groupFirst[v];
    }
    sources.ResizeUninitialized(groupFirst[vertexCount]);
    normals.Resize(groupFirst[vertexCount]);
    Jobs::ParallelFor(static_cast<uint32>(vertexCount), 4096, [&](uint32 begin, uint32 end) {
        for (uint32 v = begin; v < end; ++v)
        {
            for (uint32 g = groupFirst[v]; g < groupFirst[v + 1]; ++g)
            {
                sources[g] = v;
            }
            for (uint32 i = first[v]; i < first[v + 1]; ++i)
            {
                const uint32 corner = around[i];
                const uint32 group = groupFirst[v] + corners[corner];
                normals[group] += faceNormals[corner / 3];
                corners[corner] = group;
            }
            for (uint32 g = groupFirst[v]; g < groupFirst[v + 1]; ++g)
            {
                normals[g] = UnitOrZero(normals[g]);
            }
        }
    });
}

/// Writes one vertex of a primitive being added, from its source vertex *vertex* and its normal and (if any) tangent:
/// transformed, in VertexLayout.
void WriteSectionVertex(const DecodedPrimitive &primitive, uint32 vertex, const Vector3 &sourceNormal,
                        const Vector4 *sourceTangent, const Matrix4x4 &transform, const Matrix4x4 &normalTransform,
                        bool mirrored, const VertexLayout &layout, float32 *out)
{
    const usize vertexCount = primitive.Positions.Count();
    const Vector3 position = transform.TransformPoint(primitive.Positions[vertex]);
    const Vector3 normal = normalTransform.Transform(Vector4(sourceNormal, 0)).XYZ().Normalized();
    // Without tangents the slot holds a fixed one, which nothing reads.
    const Vector4 fileTangent = sourceTangent != nullptr ? *sourceTangent : Vector4(1, 0, 0, 1);
    const Vector3 tangent = transform.Transform(Vector4(fileTangent.XYZ(), 0)).XYZ().Normalized();
    const float32 sign = (fileTangent.W < 0) != mirrored ? -1.0f : 1.0f;
    const float32 values[10] = {position.X, position.Y, position.Z, normal.X,  normal.Y,
                                normal.Z,   tangent.X,  tangent.Y,  tangent.Z, sign};
    MemCopy(out, values, sizeof(values));
    for (uint32 set = 0; set < layout.TexCoords; ++set)
    {
        const bool present = set < primitive.TexCoords.Count() && primitive.TexCoords[set].Count() == vertexCount;
        const Vector2 uv = present ? primitive.TexCoords[set][vertex] : Vector2();
        out[layout.TexCoord(set)] = uv.X;
        out[layout.TexCoord(set) + 1] = uv.Y;
    }
    if (layout.Colors)
    {
        const Vector4 color = primitive.Colors.Count() == vertexCount ? primitive.Colors[vertex] : Vector4(1);
        MemCopy(out + layout.Color(), &color, sizeof(color));
    }
}

/// Appends *primitive*, transformed, to *section*; Convert welds identical vertices afterwards. Normals the file lacks
/// come from the triangles, tangents it lacks from the first UV set, MikkTSpace-compatible, which is the one case that
/// needs a vertex per corner; otherwise the vertices are the file's own, split only where generated normals need it.
void AddPrimitive(const DecodedPrimitive &primitive, const Matrix4x4 &transform, const VertexLayout &layout,
                  SectionGeometry &section)
{
    const usize vertexCount = primitive.Positions.Count();
    List<uint32> indices;
    indices.Reserve(primitive.Indices.Count());
    for (usize i = 0; i + 2 < primitive.Indices.Count(); i += 3)
    {
        const uint32 *triangle = primitive.Indices.Data() + i;
        if (triangle[0] < vertexCount && triangle[1] < vertexCount && triangle[2] < vertexCount)
        {
            indices.AddRange(Span<const uint32>(triangle, 3));
        }
    }
    const usize corners = indices.Count();
    if (corners == 0)
    {
        return;
    }
    const bool hasNormals = primitive.Normals.Count() == vertexCount;
    const bool hasTangents = primitive.Tangents.Count() == vertexCount;
    const bool hasTexCoords = !primitive.TexCoords.IsEmpty() && primitive.TexCoords[0].Count() == vertexCount;
    const bool generateTangents = !hasTangents && layout.TexCoords > 0 && hasTexCoords;

    // The vertices to emit: each one's source vertex (none: the file's vertices in order) and normal, and the vertex
    // each corner uses.
    List<uint32> sources;
    List<Vector3> normals;
    List<uint32> cornerVertices;
    if (!hasNormals)
    {
        // RECOVERY-DECISION TM-D12: edges sharper than 60 degrees stay hard, a common default for authoring tools.
        GenerateCreaseNormals(primitive.Positions.AsSpan(), indices.AsSpan(), 60 * Math::DegreesToRadians, sources,
                              normals, cornerVertices);
    }
    else if (!generateTangents)
    {
        normals.AddRange(primitive.Normals.AsSpan());
        cornerVertices = Move(indices);
    }
    if (generateTangents)
    {
        // MikkTSpace works per corner.
        List<Vector3> cornerNormals;
        cornerNormals.Resize(corners);
        for (usize c = 0; c < corners; ++c)
        {
            cornerNormals[c] = hasNormals ? primitive.Normals[indices[c]] : normals[cornerVertices[c]];
        }
        sources = Move(indices);
        normals = Move(cornerNormals);
        cornerVertices.ResizeUninitialized(corners);
        for (usize c = 0; c < corners; ++c)
        {
            cornerVertices[c] = static_cast<uint32>(c);
        }
    }
    const usize emitted = normals.Count();
    const auto source = [&](usize e) { return sources.IsEmpty() ? static_cast<uint32>(e) : sources[e]; };
    List<Vector4> tangents;
    if (hasTangents)
    {
        tangents.Resize(emitted);
        for (usize e = 0; e < emitted; ++e)
        {
            tangents[e] = primitive.Tangents[source(e)];
        }
    }
    else if (generateTangents)
    {
        tangents.Resize(emitted, Vector4(1, 0, 0, 1));
        List<Vector3> cornerPositions;
        List<Vector2> cornerTexCoords;
        cornerPositions.Reserve(corners);
        cornerTexCoords.Reserve(corners);
        for (usize c = 0; c < corners; ++c)
        {
            cornerPositions.Add(primitive.Positions[sources[c]]);
            cornerTexCoords.Add(primitive.TexCoords[0][sources[c]]);
        }
        meshopt_generateTangents(reinterpret_cast<float32 *>(tangents.Data()), nullptr, corners,
                                 reinterpret_cast<const float32 *>(cornerPositions.Data()), corners, sizeof(Vector3),
                                 reinterpret_cast<const float32 *>(normals.Data()), sizeof(Vector3),
                                 reinterpret_cast<const float32 *>(cornerTexCoords.Data()), sizeof(Vector2),
                                 meshopt_TangentCompatible);
    }

    // A mirroring transform turns the triangles inside out and the tangent frame with them.
    const Vector4 *rows = transform.Rows;
    const float32 determinant = rows[0].X * (rows[1].Y * rows[2].Z - rows[1].Z * rows[2].Y) -
                                rows[0].Y * (rows[1].X * rows[2].Z - rows[1].Z * rows[2].X) +
                                rows[0].Z * (rows[1].X * rows[2].Y - rows[1].Y * rows[2].X);
    const bool mirrored = determinant < 0;
    const Matrix4x4 normalTransform = transform.Inverse().Transposed();
    const uint32 floats = layout.Floats();
    const uint32 first = section.VertexCount;
    section.Vertices.Resize(section.Vertices.Count() + emitted * floats);
    float32 *const written = section.Vertices.Data() + usize{first} * floats;
    Jobs::ParallelFor(static_cast<uint32>(emitted), 16384, [&](uint32 begin, uint32 end) {
        for (usize e = begin; e < end; ++e)
        {
            WriteSectionVertex(primitive, source(e), normals[e], tangents.IsEmpty() ? nullptr : &tangents[e], transform,
                               normalTransform, mirrored, layout, written + e * floats);
        }
    });
    section.Indices.Resize(section.Indices.Count() + corners);
    uint32 *const triangles = section.Indices.Data() + section.Indices.Count() - corners;
    Jobs::ParallelFor(static_cast<uint32>(corners / 3), 16384, [&](uint32 begin, uint32 end) {
        for (usize c = usize{begin} * 3; c < usize{end} * 3; c += 3)
        {
            triangles[c] = first + cornerVertices[c];
            triangles[c + 1] = first + cornerVertices[c + (mirrored ? 2 : 1)];
            triangles[c + 2] = first + cornerVertices[c + (mirrored ? 1 : 2)];
        }
    });
    section.VertexCount += static_cast<uint32>(emitted);
}

/// Merges the vertices of *section* that are identical byte for byte and drops those no triangle uses: of each kind
/// the first in vertex order stays, and the survivors keep their order, whichever worker ran what. In parallel: every
/// vertex is hashed, the vertices go into buckets by hash, in vertex order, and each bucket finds its own duplicates.
void WeldVertices(SectionGeometry &section, uint32 floats)
{
    const auto vertexCount = static_cast<uint32>(section.VertexCount);
    const usize vertexBytes = floats * sizeof(float32);
    const float32 *vertices = section.Vertices.Data();
    List<uint8> used;
    used.Resize(vertexCount, 0);
    for (const uint32 index : section.Indices)
    {
        used[index] = 1;
    }
    List<uint64> hashes;
    hashes.ResizeUninitialized(vertexCount);
    Jobs::ParallelFor(vertexCount, 16384, [&](uint32 begin, uint32 end) {
        for (uint32 v = begin; v < end; ++v)
        {
            hashes[v] = Xxh3Hash64(vertices + usize{v} * floats, vertexBytes);
        }
    });

    // Buckets by the hash's top bits, each in vertex order: counted per run of vertices, then placed.
    constexpr uint32 BucketBits = 16;
    constexpr uint32 BucketCount = 1u << BucketBits;
    constexpr uint32 RunCount = 64;
    const uint32 runSize = Max((vertexCount + RunCount - 1) / RunCount, 1u);
    const auto bucketOf = [&](uint32 v) { return static_cast<uint32>(hashes[v] >> (64 - BucketBits)); };
    List<uint32> runOffsets;
    runOffsets.Resize(usize{RunCount} * BucketCount, 0);
    Jobs::ParallelFor(RunCount, 1, [&](uint32 begin, uint32 end) {
        for (uint32 run = begin; run < end; ++run)
        {
            for (uint32 v = run * runSize; v < Min(vertexCount, (run + 1) * runSize); ++v)
            {
                ++runOffsets[usize{run} * BucketCount + bucketOf(v)];
            }
        }
    });
    List<uint32> bucketFirst;
    bucketFirst.Resize(BucketCount + 1, 0);
    uint32 placed = 0;
    for (uint32 bucket = 0; bucket < BucketCount; ++bucket)
    {
        bucketFirst[bucket] = placed;
        for (uint32 run = 0; run < RunCount; ++run)
        {
            const uint32 count = runOffsets[usize{run} * BucketCount + bucket];
            runOffsets[usize{run} * BucketCount + bucket] = placed;
            placed += count;
        }
    }
    bucketFirst[BucketCount] = placed;
    List<uint32> buckets;
    buckets.ResizeUninitialized(vertexCount);
    Jobs::ParallelFor(RunCount, 1, [&](uint32 begin, uint32 end) {
        for (uint32 run = begin; run < end; ++run)
        {
            for (uint32 v = run * runSize; v < Min(vertexCount, (run + 1) * runSize); ++v)
            {
                buckets[runOffsets[usize{run} * BucketCount + bucketOf(v)]++] = v;
            }
        }
    });

    // Each used vertex to the first used vertex identical to it.
    List<uint32> canonical;
    canonical.ResizeUninitialized(vertexCount);
    Jobs::ParallelFor(BucketCount, 256, [&](uint32 begin, uint32 end) {
        List<uint32> sorted;
        for (uint32 bucket = begin; bucket < end; ++bucket)
        {
            sorted.Clear();
            sorted.AddRange(buckets.AsSpan().Slice(bucketFirst[bucket], bucketFirst[bucket + 1] - bucketFirst[bucket]));
            sorted.StableSort([&](uint32 a, uint32 b) { return hashes[a] < hashes[b]; });
            for (usize i = 0; i < sorted.Count();)
            {
                usize run = i;
                while (run < sorted.Count() && hashes[sorted[run]] == hashes[sorted[i]])
                {
                    ++run;
                }
                // Within a run of equal hashes, in vertex order: each vertex joins the first earlier one it equals.
                for (usize a = i; a < run; ++a)
                {
                    const uint32 v = sorted[a];
                    canonical[v] = v;
                    for (usize b = i; b < a && used[v] != 0; ++b)
                    {
                        const uint32 earlier = sorted[b];
                        if (used[earlier] != 0 && canonical[earlier] == earlier &&
                            MemCompare(vertices + usize{earlier} * floats, vertices + usize{v} * floats, vertexBytes) ==
                                0)
                        {
                            canonical[v] = earlier;
                            break;
                        }
                    }
                }
                i = run;
            }
        }
    });

    // The survivors numbered in vertex order: counted per run, then numbered.
    List<uint32> runFirst;
    runFirst.Resize(RunCount + 1, 0);
    Jobs::ParallelFor(RunCount, 1, [&](uint32 begin, uint32 end) {
        for (uint32 run = begin; run < end; ++run)
        {
            for (uint32 v = run * runSize; v < Min(vertexCount, (run + 1) * runSize); ++v)
            {
                runFirst[run + 1] += used[v] != 0 && canonical[v] == v ? 1 : 0;
            }
        }
    });
    for (uint32 run = 0; run < RunCount; ++run)
    {
        runFirst[run + 1] += runFirst[run];
    }
    const uint32 unique = runFirst[RunCount];
    List<uint32> remap;
    remap.ResizeUninitialized(vertexCount);
    List<float32> welded;
    welded.ResizeUninitialized(usize{unique} * floats);
    Jobs::ParallelFor(RunCount, 1, [&](uint32 begin, uint32 end) {
        for (uint32 run = begin; run < end; ++run)
        {
            uint32 next = runFirst[run];
            for (uint32 v = run * runSize; v < Min(vertexCount, (run + 1) * runSize); ++v)
            {
                if (used[v] != 0 && canonical[v] == v)
                {
                    MemCopy(welded.Data() + usize{next} * floats, vertices + usize{v} * floats, vertexBytes);
                    remap[v] = next++;
                }
            }
        }
    });
    // Duplicates follow their survivor, which comes before them.
    Jobs::ParallelFor(static_cast<uint32>(section.Indices.Count()), 65536, [&](uint32 begin, uint32 end) {
        for (uint32 i = begin; i < end; ++i)
        {
            section.Indices[i] = remap[canonical[section.Indices[i]]];
        }
    });
    section.Vertices = Move(welded);
    section.VertexCount = unique;
}

/// Appends the DAG *dag* of one section to *groups* and *clusters*, with each cluster's indices made cluster-local and
/// its backface cone, in parallel; group and cluster numbers continue after those already there.
void AddSectionDag(const ClusterLodResult &dag, const SectionGeometry &section, uint32 sectionIndex, uint32 floats,
                   List<BuiltGroup> &groups, List<BuiltCluster> &clusters)
{
    const auto groupBase = static_cast<uint32>(groups.Count());
    const auto clusterBase = static_cast<uint32>(clusters.Count());
    for (const ClusterLodGroup &group : dag.Groups)
    {
        BuiltGroup &built = groups.AddDefaulted();
        built.Group = group;
        for (uint32 c = 0; c < group.ClusterCount; ++c)
        {
            built.Clusters.Add(clusterBase + group.FirstCluster + c);
        }
    }
    clusters.Resize(clusterBase + dag.Clusters.Count());
    Jobs::ParallelFor(static_cast<uint32>(dag.Clusters.Count()), 64, [&](uint32 begin, uint32 end) {
        for (uint32 c = begin; c < end; ++c)
        {
            const ClusterLodCluster &cluster = dag.Clusters[c];
            const Span<const uint32> indices = dag.Indices.AsSpan().Slice(cluster.FirstIndex, cluster.IndexCount);
            BuiltCluster &out = clusters[clusterBase + c];
            out.Bounds = cluster.Bounds;
            out.Refined = cluster.Refined >= 0 ? cluster.Refined + static_cast<int32>(groupBase) : -1;
            out.Group = groupBase + cluster.Group;
            out.Section = sectionIndex;
            GetClusterLocalIndices(indices, out.Vertices, out.Triangles);
            const meshopt_Bounds bounds =
                meshopt_computeClusterBounds(indices.Data(), indices.Count(), section.Vertices.Data(),
                                             section.VertexCount, floats * sizeof(float32));
            out.ConeAxis = Vector3(bounds.cone_axis[0], bounds.cone_axis[1], bounds.cone_axis[2]);
            out.ConeCutoff = bounds.cone_cutoff;
            float32 longest = 0;
            for (usize i = 0; i + 2 < indices.Count(); i += 3)
            {
                for (usize e = 0; e < 3; ++e)
                {
                    const float32 *a = section.Vertices.Data() + usize{indices[i + e]} * floats;
                    const float32 *b = section.Vertices.Data() + usize{indices[i + (e + 1) % 3]} * floats;
                    longest = Max(longest, Vector3(b[0] - a[0], b[1] - a[1], b[2] - a[2]).LengthSquared());
                }
            }
            out.MaxEdgeLength = Math::Sqrt(longest);
        }
    });
}

float32 SignNotZero(float32 value)
{
    return value >= 0 ? 1.0f : -1.0f;
}

int16 Snorm16(float32 value)
{
    return static_cast<int16>(Math::Round(Math::Clamp(value, -1.0f, 1.0f) * 32767.0f));
}

/// A unit vector as two snorm16 on the octahedron folded onto a square.
void Octahedral(const float32 *vector, int16 (&encoded)[2])
{
    const float32 length = Math::Abs(vector[0]) + Math::Abs(vector[1]) + Math::Abs(vector[2]);
    float32 x = length > 0 ? vector[0] / length : 0;
    float32 y = length > 0 ? vector[1] / length : 0;
    if (vector[2] < 0)
    {
        const float32 folded = (1 - Math::Abs(y)) * SignNotZero(x);
        y = (1 - Math::Abs(x)) * SignNotZero(y);
        x = folded;
    }
    encoded[0] = Snorm16(x);
    encoded[1] = Snorm16(y);
}

/// Writes one vertex in Mesh's layout.
void WriteAttributes(const float32 *vertex, const VertexLayout &layout, uint8 *out)
{
    int16 encoded[2];
    Octahedral(vertex + layout.Normal, encoded);
    MemCopy(out, encoded, sizeof(encoded));
    out += sizeof(encoded);
    if (layout.TexCoords > 0)
    {
        // RECOVERY-DECISION TM-D13: a set low bit of y is a negative bitangent sign.
        Octahedral(vertex + layout.Tangent(), encoded);
        const bool negative = vertex[layout.Tangent() + 3] < 0;
        encoded[1] = static_cast<int16>((encoded[1] & ~1) | (negative ? 1 : 0));
        MemCopy(out, encoded, sizeof(encoded));
        out += sizeof(encoded);
    }
    MemCopy(out, vertex + layout.TexCoord0, layout.TexCoords * 2 * sizeof(float32));
    out += layout.TexCoords * 2 * sizeof(float32);
    if (layout.Colors)
    {
        for (uint32 c = 0; c < 4; ++c)
        {
            out[c] = static_cast<uint8>(Math::Round(Math::Clamp(vertex[layout.Color() + c], 0.0f, 1.0f) * 255.0f));
        }
    }
}

MeshLodBounds ToBounds(const ClusterLodBounds &bounds)
{
    MeshLodBounds result;
    result.Center = bounds.Center;
    result.Radius = bounds.Radius;
    result.Error = bounds.Error;
    return result;
}
} // namespace

bool MeshConverter::Convert(ConversionContext &context)
{
    MeshSource source;
    MeshSettings settings;
    if (!context.ReadSource(source) || !context.ReadConfig("Engine.Meshes", TypeOf<MeshSettings>(), &settings))
    {
        return false;
    }
    if (!source.SourceFile.IsValid())
    {
        context.Error("SourceFile names no scene");
        return false;
    }
    DecodedScene scene;
    // A named mesh is converted in its own space, so the decoder need read nothing of the others.
    if (!context.Decode(source.SourceFile.Uuid, scene, source.Mesh.AsView()))
    {
        return false;
    }
    if (scene.Meshes.IsEmpty())
    {
        context.Error("the scene has no meshes");
        return false;
    }

    // The meshes to merge, and the transform of each.
    struct Instance
    {
        const DecodedMesh *Mesh;
        Matrix4x4 Transform;
        /// The node, whose part name names its slots; -1 for slots by material alone.
        int32 Part = -1;
    };
    List<Instance> instances;
    const Matrix4x4 scale({source.Scale, 0, 0, 0}, {0, source.Scale, 0, 0}, {0, 0, source.Scale, 0}, {0, 0, 0, 1});
    if (!source.Mesh.IsEmpty())
    {
        const DecodedMesh *found = scene.Meshes.Find([&](const DecodedMesh &mesh) { return mesh.Name == source.Mesh; });
        if (found == nullptr)
        {
            context.Error(Format("the scene has no mesh named {0}", source.Mesh));
            return false;
        }
        instances.Add({found, scale, -1});
    }
    else
    {
        for (usize node = 0; node < scene.Nodes.Count(); ++node)
        {
            const int32 mesh = scene.Nodes[node].Mesh;
            if (mesh >= 0 && static_cast<usize>(mesh) < scene.Meshes.Count())
            {
                instances.Add({&scene.Meshes[static_cast<usize>(mesh)],
                               scale * WorldTransform(scene, static_cast<int32>(node)),
                               source.MergePartsByMaterial ? -1 : static_cast<int32>(node)});
            }
        }
        if (instances.IsEmpty())
        {
            context.Error("the scene places no mesh");
            return false;
        }
    }

    // One layout for every section: the most UV sets any primitive has, and colors if any has them.
    VertexLayout layout;
    for (const Instance &instance : instances)
    {
        for (const DecodedPrimitive &primitive : instance.Mesh->Primitives)
        {
            layout.TexCoords = Max(layout.TexCoords, static_cast<uint32>(primitive.TexCoords.Count()));
            layout.Colors = layout.Colors || !primitive.Colors.IsEmpty();
        }
    }
    layout.TexCoords = Min(layout.TexCoords, 4u); // RECOVERY-DECISION TM-D14: at most four UV sets.
    const uint32 floats = layout.Floats();

    // A section per slot, part and material, in the order the slots first appear. Parts are named as the importer
    // names the prefab's entities.
    List<String> partNames;
    GetScenePartNames(scene, partNames);
    struct SlotKey
    {
        int32 Part;
        uint32 Material;
        bool operator==(const SlotKey &) const = default;
    };
    List<SectionGeometry> sections;
    List<SlotKey> sectionSlots;
    for (const Instance &instance : instances)
    {
        for (const DecodedPrimitive &primitive : instance.Mesh->Primitives)
        {
            const SlotKey slot{instance.Part, primitive.Material};
            usize index = sectionSlots.IndexOf(slot);
            if (index == List<SlotKey>::NotFound)
            {
                index = sections.Count();
                sectionSlots.Add(slot);
                SectionGeometry &section = sections.AddDefaulted();
                section.Part = instance.Part >= 0 ? partNames[static_cast<usize>(instance.Part)] : String();
                const bool known = primitive.Material < scene.Materials.Count();
                section.Material =
                    known ? scene.Materials[primitive.Material].Name : Format("Material{0}", primitive.Material);
                section.TwoSided = known && scene.Materials[primitive.Material].TwoSided;
            }
            AddPrimitive(primitive, instance.Transform, layout, sections[index]);
        }
    }
    // The sections hold everything from here on; the decoded scene, as large again, goes.
    instances.Clear();
    scene = DecodedScene();

    // Weld identical vertices, such as those AddPrimitive gave each corner for generated tangents.
    Box bounds;
    uint32 triangleCount = 0;
    for (SectionGeometry &section : sections)
    {
        if (section.VertexCount == 0)
        {
            continue;
        }
        WeldVertices(section, floats);
        triangleCount += static_cast<uint32>(section.Indices.Count() / 3);
        // Bounds per run of vertices, merged after: min and max do not care about order.
        constexpr uint32 Runs = 64;
        Box runBounds[Runs];
        const uint32 runSize = Max((section.VertexCount + Runs - 1) / Runs, 1u);
        Jobs::ParallelFor(Runs, 1, [&](uint32 begin, uint32 end) {
            for (uint32 run = begin; run < end; ++run)
            {
                for (uint32 v = run * runSize; v < Min(section.VertexCount, (run + 1) * runSize); ++v)
                {
                    const float32 *position = section.Vertices.Data() + usize{v} * floats + layout.Position;
                    runBounds[run].Add(Vector3(position[0], position[1], position[2]));
                }
            }
        });
        for (const Box &run : runBounds)
        {
            bounds.Add(run);
        }
    }
    (void)sections.RemoveAll([](const SectionGeometry &section) { return section.Indices.IsEmpty(); });
    if (triangleCount == 0)
    {
        context.Error("the mesh has no triangles");
        return false;
    }

    // The cluster DAG of each section, one after the other: each builds its levels in parallel.
    ClusterLodSettings lod;
    lod.BuildDag = source.BuildDag;
    lod.MaxTriangles = Math::Clamp(settings.ClusterTriangles, 4u, Mesh::MaxClusterTriangles);
    lod.MinTriangles = lod.MaxTriangles / 3;
    lod.MaxVertices = Math::Clamp(settings.ClusterVertices, 3u, Mesh::MaxClusterVertices);
    // RECOVERY-DECISION TM-D15: regular simplification, which keeps UV and normal seams because welding left them as
    // separate vertices; permissive mode would need a protect mask per attribute. Normals weigh half as much as UVs,
    // tangents nothing.
    lod.Permissive = false;
    constexpr float32 Weights[9] = {0.5f, 0.5f, 0.5f, 0, 0, 0, 0, 1, 1};
    List<BuiltGroup> groups;
    List<BuiltCluster> clusters;
    for (usize s = 0; s < sections.Count(); ++s)
    {
        const SectionGeometry &section = sections[s];
        ClusterLodInput input;
        input.Indices = section.Indices.AsSpan();
        input.VertexCount = section.VertexCount;
        input.Positions = section.Vertices.Data() + layout.Position;
        input.PositionStride = floats * sizeof(float32);
        input.Attributes = section.Vertices.Data() + layout.Normal;
        input.AttributeStride = floats * sizeof(float32);
        input.AttributeWeights = Span<const float32>(Weights, layout.TexCoords > 0 ? 9 : 3);
        ClusterLodResult dag;
        BuildClusterLod(input, lod, dag);
        AddSectionDag(dag, section, static_cast<uint32>(s), floats, groups, clusters);
    }

    // RECOVERY-DECISION TM-D16: terminal groups first, since together they are the coarsest version of the whole
    // mesh, which stays resident; then coarser depths before finer ones, so streaming in page order refines evenly.
    const auto isTerminal = [](const BuiltGroup &group) { return group.Group.Simplified.Error >= __FLT_MAX__; };
    List<uint32> order;
    for (uint32 g = 0; g < groups.Count(); ++g)
    {
        order.Add(g);
    }
    order.StableSort([&](uint32 a, uint32 b) {
        const bool terminalA = isTerminal(groups[a]);
        const bool terminalB = isTerminal(groups[b]);
        return terminalA != terminalB ? terminalA : groups[a].Group.Depth > groups[b].Group.Depth;
    });
    List<uint32> renumbered;
    renumbered.Resize(groups.Count());
    for (uint32 g = 0; g < order.Count(); ++g)
    {
        renumbered[order[g]] = g;
    }

    // Every position on one grid, a power of two that fits the mesh's largest extent in PositionBits bits, so that
    // the vertices clusters share land on the same point; each cluster keeps its corner on the grid and the bits per
    // axis its own vertices need.
    const Vector3 extent = bounds.Max - bounds.Min;
    const float32 largest = Max(Max(extent.X, extent.Y), Max(extent.Z, 1e-6f));
    const int32 exponent = static_cast<int32>(__builtin_ceilf(__builtin_log2f(largest))) -
                           static_cast<int32>(Math::Clamp(settings.PositionBits, 8u, 24u));
    const float32 positionStep = __builtin_ldexpf(1.0f, exponent);
    struct Quantized
    {
        int32 Min[3] = {};
        uint32 Bits = 0;
        bool OutOfRange = false;
        List<int32> Positions;
    };
    List<Quantized> quantized;
    quantized.Resize(clusters.Count());
    Jobs::ParallelFor(static_cast<uint32>(clusters.Count()), 64, [&](uint32 begin, uint32 end) {
        for (uint32 c = begin; c < end; ++c)
        {
            const BuiltCluster &cluster = clusters[c];
            const SectionGeometry &section = sections[cluster.Section];
            Quantized &out = quantized[c];
            out.Positions.Resize(cluster.Vertices.Count() * 3);
            int64 low[3] = {__INT64_MAX__, __INT64_MAX__, __INT64_MAX__};
            int64 high[3] = {-__INT64_MAX__, -__INT64_MAX__, -__INT64_MAX__};
            for (usize v = 0; v < cluster.Vertices.Count(); ++v)
            {
                const float32 *position =
                    section.Vertices.Data() + usize{cluster.Vertices[v]} * floats + layout.Position;
                for (uint32 axis = 0; axis < 3; ++axis)
                {
                    const float32 scaled = __builtin_floorf(position[axis] / positionStep + 0.5f);
                    if (!(__builtin_fabsf(scaled) < 1073741824.0f))
                    {
                        out.OutOfRange = true;
                    }
                    const auto q = static_cast<int32>(Math::Clamp(scaled, -1073741824.0f, 1073741824.0f));
                    out.Positions[v * 3 + axis] = q;
                    low[axis] = Min<int64>(low[axis], q);
                    high[axis] = Max<int64>(high[axis], q);
                }
            }
            for (uint32 axis = 0; axis < 3; ++axis)
            {
                const auto range = static_cast<uint32>(high[axis] - low[axis]);
                out.Min[axis] = static_cast<int32>(low[axis]);
                out.Bits |= (range == 0 ? 0u : 32u - static_cast<uint32>(__builtin_clz(range))) << (axis * 8);
            }
            out.Bits |= section.TwoSided ? MeshPageCluster::TwoSided : 0u;
        }
    });
    bool outOfRange = false;
    for (const Quantized &cluster : quantized)
    {
        outOfRange = outOfRange || cluster.OutOfRange;
    }
    if (outOfRange)
    {
        context.Error("the mesh lies too far from its origin for its position grid");
        return false;
    }
    const auto positionBytes = [&](uint32 index) {
        MeshPageCluster record;
        record.VertexCount = static_cast<uint32>(clusters[index].Vertices.Count());
        record.PositionBits = quantized[index].Bits;
        return record.GetPositionBytes();
    };

    // Pack whole groups into pages of at most PageSize bytes and MaxPageClusters clusters: the cluster records, then
    // each cluster's packed positions and attributes, then three local indices per triangle. First where everything
    // goes, in order; then the bytes, in parallel, each cluster into its own place.
    const uint32 stride = Mesh::GetAttributeStride(layout.TexCoords > 0, layout.TexCoords, layout.Colors);
    const auto vertexBytes = [&](uint32 index) {
        return positionBytes(index) + static_cast<uint32>(clusters[index].Vertices.Count()) * stride;
    };
    constexpr usize RecordBytes = sizeof(MeshPageCluster);
    struct Placed
    {
        uint32 Source = 0;
        uint32 Page = 0;
        uint32 Record = 0;
        uint32 FirstVertex = 0;
        uint32 VertexByte = 0;
        uint32 FirstTriangle = 0;
        int32 RefinedGroup = -1;
        uint32 Group = 0;
    };
    List<MeshClusterGroup> meshGroups;
    List<Placed> placed;
    List<MeshPage> pages;
    List<uint32> sectionTriangles;
    sectionTriangles.Resize(sections.Count(), 0);
    uint32 rootPages = 0;
    uint32 depthCount = 0;
    usize pageBytes = 0;
    usize bulkSize = 0;
    bool pageOpen = false;
    const auto closePage = [&]() {
        MeshPage &page = pages.Last();
        page.Offset = (bulkSize + 15) & ~usize{15};
        page.Size = static_cast<uint32>(pageBytes);
        bulkSize = page.Offset + pageBytes;
        pageBytes = 0;
        pageOpen = false;
    };
    for (uint32 g = 0; g < order.Count(); ++g)
    {
        const BuiltGroup &built = groups[order[g]];
        usize groupBytes = 0;
        for (const uint32 index : built.Clusters)
        {
            groupBytes += RecordBytes + vertexBytes(index) + clusters[index].Triangles.Count();
        }
        if (pageOpen && (pageBytes + groupBytes > settings.PageSize ||
                         pages.Last().ClusterCount + built.Clusters.Count() > Mesh::MaxPageClusters))
        {
            closePage();
        }
        if (!pageOpen)
        {
            pages.AddDefaulted();
            pageOpen = true;
        }
        const auto pageIndex = static_cast<uint32>(pages.Count() - 1);
        if (isTerminal(built))
        {
            rootPages = pageIndex + 1;
        }
        const uint32 depth = built.Group.Depth;
        depthCount = Max(depthCount, depth + 1);
        MeshPage &page = pages.Last();
        page.Depth = Max(page.Depth, depth);

        MeshClusterGroup &group = meshGroups.AddDefaulted();
        group.Depth = depth;
        group.Simplified = ToBounds(built.Group.Simplified);
        group.Page = pageIndex;
        group.FirstCluster = page.ClusterCount;
        group.ClusterCount = static_cast<uint32>(built.Clusters.Count());
        for (const uint32 index : built.Clusters)
        {
            const BuiltCluster &cluster = clusters[index];
            Placed &out = placed.AddDefaulted();
            out.Source = index;
            out.Page = pageIndex;
            out.Record = page.ClusterCount++;
            out.FirstVertex = page.VertexCount;
            out.VertexByte = page.VertexBytes;
            out.FirstTriangle = page.TriangleCount;
            out.RefinedGroup =
                cluster.Refined >= 0 ? static_cast<int32>(renumbered[static_cast<usize>(cluster.Refined)]) : -1;
            out.Group = g;
            const auto triangles = static_cast<uint32>(cluster.Triangles.Count() / 3);
            page.VertexCount += static_cast<uint32>(cluster.Vertices.Count());
            page.VertexBytes += vertexBytes(index);
            page.TriangleCount += triangles;
            pageBytes += RecordBytes + vertexBytes(index) + cluster.Triangles.Count();
            if (cluster.Refined < 0)
            {
                sectionTriangles[cluster.Section] += triangles;
            }
        }
    }
    if (pageOpen)
    {
        closePage();
    }

    // A page's parents: the pages of the clusters its groups simplified into, which must draw while it is missing.
    List<List<uint32>> parents;
    parents.Resize(pages.Count());
    for (const Placed &cluster : placed)
    {
        if (cluster.RefinedGroup >= 0)
        {
            const uint32 child = meshGroups[static_cast<usize>(cluster.RefinedGroup)].Page;
            if (child != cluster.Page && !parents[child].Contains(cluster.Page))
            {
                parents[child].Add(cluster.Page);
            }
        }
    }
    List<uint32> pageParents;
    for (usize pageIndex = 0; pageIndex < pages.Count(); ++pageIndex)
    {
        parents[pageIndex].Sort();
        pages[pageIndex].FirstParent = static_cast<uint32>(pageParents.Count());
        pages[pageIndex].ParentCount = static_cast<uint32>(parents[pageIndex].Count());
        pageParents.AddRange(parents[pageIndex].AsSpan());
    }

    // Zeroed, so the padding between pages is deterministic.
    List<uint8> bulk;
    bulk.Resize(bulkSize, 0);
    Jobs::ParallelFor(static_cast<uint32>(placed.Count()), 64, [&](uint32 begin, uint32 end) {
        for (uint32 c = begin; c < end; ++c)
        {
            const Placed &where = placed[c];
            const BuiltCluster &cluster = clusters[where.Source];
            const MeshPage &page = pages[where.Page];
            uint8 *geometry = bulk.Data() + page.Offset + page.GetGeometryOffset();
            MeshPageCluster record;
            const Quantized &positions = quantized[where.Source];
            record.VertexOffset = where.VertexByte;
            record.IndexOffset = page.VertexBytes + where.FirstTriangle * 3;
            record.VertexCount = static_cast<uint32>(cluster.Vertices.Count());
            record.TriangleCount = static_cast<uint32>(cluster.Triangles.Count() / 3);
            record.RefinedGroup = where.RefinedGroup;
            record.Group = where.Group;
            record.Section = cluster.Section;
            record.MaxEdgeLength = cluster.MaxEdgeLength;
            record.Sphere[0] = cluster.Bounds.Center.X;
            record.Sphere[1] = cluster.Bounds.Center.Y;
            record.Sphere[2] = cluster.Bounds.Center.Z;
            // Snapping to the grid moves a vertex by up to half a step along each axis.
            record.Sphere[3] = cluster.Bounds.Radius + positionStep;
            record.Cone[0] = cluster.ConeAxis.X;
            record.Cone[1] = cluster.ConeAxis.Y;
            record.Cone[2] = cluster.ConeAxis.Z;
            record.Cone[3] = cluster.ConeCutoff;
            MemCopy(record.PositionMin, positions.Min, sizeof(record.PositionMin));
            record.PositionBits = positions.Bits;
            MemCopy(bulk.Data() + page.Offset + usize{where.Record} * RecordBytes, &record, RecordBytes);
            // The offsets, least significant bit first; the bulk is zeroed, so the words only gain bits.
            const uint32 bits[3] = {positions.Bits & 255, (positions.Bits >> 8) & 255, (positions.Bits >> 16) & 255};
            uint8 *packed = geometry + record.VertexOffset;
            uint32 bit = 0;
            for (usize v = 0; v < cluster.Vertices.Count(); ++v)
            {
                for (uint32 axis = 0; axis < 3; ++axis)
                {
                    const auto value = static_cast<uint32>(positions.Positions[v * 3 + axis] - positions.Min[axis]);
                    const uint64 shifted = uint64{value} << (bit & 31);
                    for (uint32 w = 0; w < 2; ++w)
                    {
                        uint32 word;
                        MemCopy(&word, packed + ((bit >> 5) + w) * 4, sizeof(word));
                        word |= static_cast<uint32>(shifted >> (w * 32));
                        MemCopy(packed + ((bit >> 5) + w) * 4, &word, sizeof(word));
                    }
                    bit += bits[axis];
                }
            }
            const SectionGeometry &section = sections[cluster.Section];
            uint8 *attributes = packed + record.GetPositionBytes();
            for (usize v = 0; v < cluster.Vertices.Count(); ++v)
            {
                WriteAttributes(section.Vertices.Data() + usize{cluster.Vertices[v]} * floats, layout,
                                attributes + v * stride);
            }
            MemCopy(geometry + record.IndexOffset, cluster.Triangles.Data(), cluster.Triangles.Count());
        }
    });

    // The bounding volume hierarchy over the groups, one tree per depth.
    // RECOVERY-DECISION TM-D17: eight children per node, a common BVH width for GPU traversal.
    constexpr uint32 NodeWidth = 8;
    List<ClusterLodGroup> orderedGroups;
    for (const uint32 index : order)
    {
        orderedGroups.Add(groups[index].Group);
    }
    List<ClusterLodNode> nodes;
    BuildClusterLodHierarchy(orderedGroups.AsSpan(), depthCount, NodeWidth, nodes);
    List<MeshHierarchyNode> hierarchy;
    for (const ClusterLodNode &source : nodes)
    {
        MeshHierarchyNode &node = hierarchy.AddDefaulted();
        node.Bounds = ToBounds(source.Bounds);
        node.Group = source.Group;
        node.FirstChild = source.FirstChild;
        node.ChildCount = source.ChildCount;
    }

    List<MeshSection> meshSections;
    for (usize s = 0; s < sections.Count(); ++s)
    {
        MeshSection &section = meshSections.AddDefaulted();
        section.Part = FrozenString(sections[s].Part.AsView());
        section.Material = FrozenString(sections[s].Material.AsView());
        section.TriangleCount = sectionTriangles[s];
        section.TwoSided = sections[s].TwoSided;
        if (const AssetRef<Material> *slot = source.Slots.Find(section.GetSlotName()))
        {
            section.DefaultMaterial = SoftAssetRef<Material>(slot->GetGuid());
        }
    }
    // A slot named that the mesh does not have is a typo or a stale name: say which slots there are.
    for (const auto &[name, material] : source.Slots)
    {
        (void)material;
        bool found = false;
        StringBuilder<512> slots;
        for (const MeshSection &section : meshSections)
        {
            const String slot = section.GetSlotName();
            found = found || slot == name;
            slots.Append(slots.IsEmpty() ? StringView() : StringView(", ")).Append(slot.AsView());
        }
        if (!found)
        {
            context.Error(Format("the mesh has no slot `{0}`; its slots are {1}", name, slots.AsView()).AsView());
        }
    }

    Mesh mesh;
    mesh.Name = FrozenString(context.GetSource().GetName());
    mesh.Bounds = bounds;
    mesh.Sections = FrozenArray<MeshSection>(meshSections.AsSpan());
    mesh.Groups = FrozenArray<MeshClusterGroup>(meshGroups.AsSpan());
    mesh.Hierarchy = FrozenArray<MeshHierarchyNode>(hierarchy.AsSpan());
    mesh.Pages = FrozenArray<MeshPage>(pages.AsSpan());
    mesh.PageParents = FrozenArray<uint32>(pageParents.AsSpan());
    mesh.DepthCount = depthCount;
    mesh.RootPageCount = rootPages;
    mesh.AttributeStride = stride;
    mesh.PositionStep = positionStep;
    mesh.HasTangents = layout.TexCoords > 0;
    mesh.TexCoordCount = layout.TexCoords;
    mesh.HasColors = layout.Colors;
    mesh.TriangleCount = triangleCount;
    mesh.ClusterCount = static_cast<uint32>(placed.Count());
    context.SetResult(mesh);
    context.SetBulk(Move(bulk));
    return !context.HasFailed();
}
} // namespace Marlin
