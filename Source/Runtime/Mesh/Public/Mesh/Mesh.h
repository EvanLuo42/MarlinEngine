#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Math/Bounds.h"
#include "Material/Material.h"
#include "Math/Vector.h"
#include "Mesh/Mesh.gen.h"

namespace Marlin
{
/// A bounding sphere and the simplification error of the geometry inside it, both in mesh space. The renderer
/// projects Error over the distance to Center, less Radius, to get the error on screen.
MSTRUCT()
struct MeshLodBounds
{
    MGENERATED_BODY()

    MPROPERTY()
    Vector3 Center;
    MPROPERTY()
    float32 Radius = 0;
    MPROPERTY()
    float32 Error = 0;
};

/// Up to Mesh::MaxClusterVertices vertices and Mesh::MaxClusterTriangles triangles that are culled and drawn together,
/// as the GPU reads it: a page starts with the records of its clusters (Documentation/Architecture/07 §5.3). Offsets
/// are in bytes from the page's geometry, which follows the records. Mirrors SceneTypes.mshader's SceneCluster.
struct MeshPageCluster
{
    uint32 VertexOffset = 0;
    uint32 IndexOffset = 0;
    uint32 VertexCount = 0;
    uint32 TriangleCount = 0;
    /// The group whose simplification produced this cluster, or -1 for the source triangles. A cluster is drawn when
    /// its own group's simplified error is too large on screen and RefinedGroup's is small enough or not resident.
    int32 RefinedGroup = -1;
    /// The group the cluster belongs to.
    uint32 Group = 0;
    uint32 Section = 0;
    /// The longest edge of its triangles, in mesh space: how large they are on screen picks the raster that draws them.
    float32 MaxEdgeLength = 0;
    /// For culling, in mesh space: centre and radius.
    float32 Sphere[4] = {};
    /// The backface cone: the cluster faces away from every view direction d with dot(d, axis) >= cutoff; a cutoff of
    /// 1 or more culls nothing.
    float32 Cone[4] = {0, 0, 0, 1};
    /// The positions are on the mesh's grid of Mesh::PositionStep: PositionMin plus, per vertex, an offset of
    /// PositionBits bits per axis (x in bits 0-7, y 8-15, z 16-23), packed least significant bit first into 32-bit
    /// words from VertexOffset. Bits 24 and up are flags.
    int32 PositionMin[3] = {};
    uint32 PositionBits = 0;

    /// PositionBits flag: the section's material is two-sided, so neither the cone nor the raster culls back faces.
    static constexpr uint32 TwoSided = 1u << 24;

    /// The bits of a vertex's position offsets.
    [[nodiscard]] uint32 GetBitsPerVertex() const
    {
        return (PositionBits & 255) + ((PositionBits >> 8) & 255) + ((PositionBits >> 16) & 255);
    }
    /// The bytes of the packed positions, with a word of padding that lets a reader take two words at any vertex;
    /// the attributes follow them.
    [[nodiscard]] uint32 GetPositionBytes() const
    {
        return (VertexCount * GetBitsPerVertex() + 31) / 32 * 4 + 4;
    }
};
static_assert(sizeof(MeshPageCluster) == 80);

/// Clusters simplified together: a node of the DAG. Its clusters are consecutive records of one page.
MSTRUCT()
struct MeshClusterGroup
{
    MGENERATED_BODY()

    /// 0 for groups of source triangles, then one more per simplification.
    MPROPERTY()
    uint32 Depth = 0;
    /// The bounds of the coarser clusters this group simplifies into; Error is the largest float for a group that
    /// could not be simplified further, which is always drawn when reached.
    MPROPERTY()
    MeshLodBounds Simplified;
    MPROPERTY()
    uint32 Page = 0;
    /// The first of its clusters among its page's records.
    MPROPERTY()
    uint32 FirstCluster = 0;
    MPROPERTY()
    uint32 ClusterCount = 0;
};

/// A node of the bounding volume hierarchy over the groups, one tree per DAG depth: traversal descends while a node's
/// error is too large on screen.
MSTRUCT()
struct MeshHierarchyNode
{
    MGENERATED_BODY()

    /// The worst error of every group below.
    MPROPERTY()
    MeshLodBounds Bounds;
    /// The group of a leaf, or -1 for an inner node.
    MPROPERTY()
    int32 Group = -1;
    MPROPERTY()
    uint32 FirstChild = 0;
    MPROPERTY()
    uint32 ChildCount = 0;
};

/// The unit of streaming: whole groups at Offset in the bulk data. A page holds ClusterCount MeshPageCluster records,
/// then its geometry: each cluster's packed positions followed by its VertexCount attribute sets of
/// Mesh::AttributeStride bytes, VertexBytes in all, then three byte-sized cluster-local indices per triangle.
MSTRUCT()
struct MeshPage
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Offset = 0;
    MPROPERTY()
    uint32 Size = 0;
    MPROPERTY()
    uint32 ClusterCount = 0;
    MPROPERTY()
    uint32 VertexCount = 0;
    MPROPERTY()
    uint32 TriangleCount = 0;
    /// The bytes of the clusters' positions and attributes, after the records; the indices follow.
    MPROPERTY()
    uint32 VertexBytes = 0;
    /// The coarsest group depth in the page; pages are stored coarsest first.
    MPROPERTY()
    uint32 Depth = 0;
    /// Mesh::PageParents[FirstParent, FirstParent + ParentCount): the pages that hold the clusters this page's groups
    /// simplify into. They must be resident before this page draws, and this page must go before they do.
    MPROPERTY()
    uint32 FirstParent = 0;
    MPROPERTY()
    uint32 ParentCount = 0;

    /// Where the geometry starts, after the cluster records.
    [[nodiscard]] uint32 GetGeometryOffset() const
    {
        return ClusterCount * static_cast<uint32>(sizeof(MeshPageCluster));
    }
};

/// One material slot: the triangles a material instance is assigned to. A slot is named by its part (the node of a
/// combined scene, empty otherwise) and its material, written `Part/Material` (Documentation/Architecture/06 §3.5).
MSTRUCT()
struct MeshSection
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Part;
    MPROPERTY()
    FrozenString Material;
    MPROPERTY()
    uint32 TriangleCount = 0;
    /// The material shows both sides of its triangles; otherwise back faces are culled.
    MPROPERTY()
    bool TwoSided = false;
    /// The slot's material unless an entity's RenderMesh overrides it (MeshSource.Slots, 06 §3.5); null for the
    /// engine's Default material. Soft: the renderer loads the materials it draws with, not every mesh's.
    MPROPERTY()
    SoftAssetRef<Marlin::Material> DefaultMaterial;

    /// The slot's name: `Part/Material`, or the material's name for a mesh without parts.
    [[nodiscard]] String GetSlotName() const
    {
        return Part.AsView().IsEmpty() ? String(Material.AsView()) : String(Part.AsView()) + "/" + Material.AsView();
    }
};

/// A static mesh as a cluster DAG (Documentation/Architecture/07 §3), for GPU-driven culling and continuous LOD: the
/// block holds the groups, hierarchy, page table and page dependencies; the bulk data holds the pages, clusters and
/// all. The first RootPageCount pages hold the coarsest levels and stay resident; the renderer streams finer pages as
/// their groups come close enough. Each vertex is, in order: position as three floats; normal and, if HasTangents,
/// tangent as octahedral snorm16 pairs, the bitangent sign in the tangent's low bit of y; TexCoordCount float pairs;
/// and, if HasColors, RGBA8.
MCLASS(PackageClass = "GpuBuffer")
class MESH_API Mesh final : public Asset
{
    MGENERATED_BODY()
public:
    static constexpr uint32 MaxClusterVertices = 256;  // RECOVERY-DECISION D2: Nanite-style cluster size.
    static constexpr uint32 MaxClusterTriangles = 128; // RECOVERY-DECISION D2: Nanite-style cluster size.
    /// Clusters a page holds at most, so that a cluster is addressed by its page's slot and an 8-bit index.
    static constexpr uint32 MaxPageClusters = 256;

    MPROPERTY()
    Box Bounds;
    MPROPERTY()
    FrozenArray<MeshSection> Sections;
    MPROPERTY()
    FrozenArray<MeshClusterGroup> Groups;
    /// Nodes 0 to DepthCount - 1 are the roots of the per-depth trees.
    MPROPERTY()
    FrozenArray<MeshHierarchyNode> Hierarchy;
    MPROPERTY()
    FrozenArray<MeshPage> Pages;
    /// The parents of every page, by MeshPage::FirstParent.
    MPROPERTY()
    FrozenArray<uint32> PageParents;
    MPROPERTY()
    uint32 DepthCount = 0;
    MPROPERTY()
    uint32 RootPageCount = 0;
    /// The bytes of a vertex's attributes, after the cluster's packed positions.
    MPROPERTY()
    uint32 AttributeStride = 0;
    /// The quantization grid of the positions, a power of two in metres: every cluster's vertices are on it, so the
    /// vertices clusters share agree exactly.
    MPROPERTY()
    float32 PositionStep = 0;
    MPROPERTY()
    bool HasTangents = false;
    MPROPERTY()
    uint32 TexCoordCount = 0;
    MPROPERTY()
    bool HasColors = false;
    /// Triangles of the source geometry, the finest level of the DAG.
    MPROPERTY()
    uint32 TriangleCount = 0;
    /// Clusters across every page.
    MPROPERTY()
    uint32 ClusterCount = 0;

    Mesh() = default;
    ~Mesh() override = default;
    MARLIN_NONCOPYABLE(Mesh);

    /// The bytes of a vertex's attributes: the normal, the tangent with tangents, the UVs and the color.
    [[nodiscard]] static uint32 GetAttributeStride(bool tangents, uint32 texCoords, bool colors)
    {
        return 4u + (tangents ? 4u : 0u) + texCoords * 8u + (colors ? 4u : 0u);
    }

    /// The position of *cluster*'s *vertex*, whose page geometry starts at *geometry*.
    [[nodiscard]] Vector3 ReadPosition(const uint8 *geometry, const MeshPageCluster &cluster, uint32 vertex) const
    {
        const uint32 bits[3] = {cluster.PositionBits & 255, (cluster.PositionBits >> 8) & 255,
                                (cluster.PositionBits >> 16) & 255};
        uint32 bit = vertex * cluster.GetBitsPerVertex();
        float32 position[3];
        for (uint32 axis = 0; axis < 3; ++axis)
        {
            uint32 words[2];
            __builtin_memcpy(words, geometry + cluster.VertexOffset + (bit >> 5) * 4, sizeof(words));
            const uint64 window = uint64{words[0]} | (uint64{words[1]} << 32);
            const auto offset = static_cast<uint32>(window >> (bit & 31)) & ((1u << bits[axis]) - 1u);
            position[axis] = static_cast<float32>(cluster.PositionMin[axis] + static_cast<int32>(offset)) * PositionStep;
            bit += bits[axis];
        }
        return Vector3(position[0], position[1], position[2]);
    }
};
} // namespace Marlin
