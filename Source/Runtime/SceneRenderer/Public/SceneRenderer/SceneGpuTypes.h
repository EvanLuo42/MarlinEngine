#pragma once

#include "CoreTypes.h"
#include "Math/Quaternion.h"
#include "Math/Vector.h"
#include "Math/WorldPosition.h"

namespace Marlin
{
// The GPU Scene as the scene passes read it (Documentation/Architecture/07 §5.2, §5.3). Mirrors
// Language/Shaders/Passes/Scene/SceneTypes.mshader; keep both in step.

/// Metres per tile of the large-coordinate split: a position is a whole number of tiles plus a float offset, and
/// each view subtracts its camera's tile exactly before it converts to float.
inline constexpr float64 SceneTileSize = 1024;
/// A visibility value holds the triangle in its low TriangleBits bits, the visible cluster above them and the depth in
/// the high word, so a view lists at most this many clusters.
inline constexpr uint32 SceneTriangleBits = 7;
inline constexpr uint32 SceneMaxVisibleClusters = 1u << (32 - SceneTriangleBits);
inline constexpr uint32 SceneInvalidMesh = ~0u;
/// The words of a view's cluster counts (SceneClusters.mshader).
inline constexpr uint32 SceneCountWords = 6;
/// A page table entry of a page that is not resident.
inline constexpr uint32 SceneNotResident = ~0u;
/// Cluster records each page slot holds in the cluster pool: a cluster is named by slot * this + its record.
inline constexpr uint32 SceneSlotClusters = 256;
/// The words of a view's traversal state (SceneLod.mshader): queue write and read positions, items pending, items
/// dropped for a full queue, visible groups and groups dropped for a full list, then the nodes the first phase set
/// aside as occluded, the groups the first phase listed and where the second phase's entries start in the queue.
inline constexpr uint32 SceneTraversalWords = 12;
/// The words before the entries of the frame's page request list: count, requests dropped for a full list, the
/// longest traversal queue of the frame's views, and SceneTraversalFlags of any of them.
inline constexpr uint32 SceneRequestHeaderWords = 4;
/// The words of a request: the page in the page table, and the error on screen in pixels (float bits) of the group
/// that asked for it.
inline constexpr uint32 SceneRequestWords = 2;
enum SceneTraversalFlags : uint32
{
    SceneTraversalQueueFull = 1,
    SceneTraversalGroupsFull = 2,
    SceneTraversalUnfinished = 4,
};

/// A position as a whole number of tiles and an offset within the tile.
struct SCENERENDERER_API SceneTilePosition
{
    int32 Tile[3] = {};
    float32 Offset[3] = {};

    [[nodiscard]] static SceneTilePosition FromWorld(const WorldPosition &position);
};

/// One slot of a RenderScene's instance buffer; a free slot has Mesh = SceneInvalidMesh.
struct SceneGpuInstance
{
    int32 Tile[3] = {};
    uint32 Mesh = SceneInvalidMesh;
    float32 Rotation[4] = {0, 0, 0, 1};
    float32 Offset[3] = {};
    uint32 Flags = 0;
    float32 Scale[3] = {1, 1, 1};
    /// Its material binding (SceneRendererSubsystem::AcquireMaterialBinding): 0 for the mesh's default materials.
    uint32 Materials = 0;
};
static_assert(sizeof(SceneGpuInstance) == 64);

/// A mesh of the geometry pool: where its hierarchy, groups and pages are in their tables. DepthCount is zero, and
/// the mesh draws nothing, until its root pages are resident.
struct SceneGpuMesh
{
    uint32 FirstNode = 0;
    /// Nodes FirstNode to FirstNode + DepthCount - 1 are the roots of the per-depth trees.
    uint32 DepthCount = 0;
    uint32 FirstGroup = 0;
    uint32 FirstPage = 0;
    /// The bounding sphere in mesh space: center, radius.
    float32 Sphere[4] = {};
    /// Its sections' material slots in the section table, from here on.
    uint32 FirstSection = 0;
    /// The bytes of a vertex's attributes (Mesh::AttributeStride) and their layout: the UV count in bits 0-3, bit 4
    /// for tangents, bit 5 for colors.
    uint32 AttributeStride = 0;
    uint32 AttributeLayout = 0;
    uint32 Pad = 0;
};
static_assert(sizeof(SceneGpuMesh) == 48);

/// SceneGpuMesh::AttributeLayout bits.
inline constexpr uint32 SceneAttributeTangents = 1u << 4;
inline constexpr uint32 SceneAttributeColors = 1u << 5;

/// A node of a mesh's hierarchy (Mesh::Hierarchy): its bounds with the worst error below, and either a group (a
/// leaf) or children, which are nodes of the same mesh.
struct SceneGpuNode
{
    float32 Sphere[4] = {};
    float32 Error = 0;
    /// The mesh's group of a leaf, or -1.
    int32 Group = -1;
    uint32 FirstChild = 0;
    uint32 ChildCount = 0;
};
static_assert(sizeof(SceneGpuNode) == 32);

/// A group of a mesh's DAG: the bounds and error of what it simplifies into, and its clusters among its page's
/// records.
struct SceneGpuGroup
{
    float32 Sphere[4] = {};
    float32 Error = 0;
    /// The mesh's page.
    uint32 Page = 0;
    /// The first of its clusters in the page's records in the low 16 bits, how many in the high 16.
    uint32 ClusterRange = 0;
    /// Its level in the DAG, 0 for the source triangles' groups.
    uint32 Depth = 0;
};
static_assert(sizeof(SceneGpuGroup) == 32);

/// A resident cluster in the cluster pool, at slot * SceneSlotClusters + its record: where its vertices and triangles
/// are in the geometry pool, in bytes.
struct SceneGpuCluster
{
    uint32 VertexOffset = 0;
    uint32 IndexOffset = 0;
    /// VertexCount in bits 0-8, TriangleCount from bit 9.
    uint32 Counts = 0;
    /// The cluster's section of its mesh (MeshPageCluster::Section): its material slot is the mesh's section table's.
    uint32 Section = 0;
    /// The mesh's position grid (Mesh::PositionStep).
    float32 PositionStep = 0;
    /// The mesh's group whose simplification produced the cluster, or -1 (MeshPageCluster::RefinedGroup).
    int32 RefinedGroup = -1;
    uint32 Group = 0;
    /// The longest triangle edge in mesh space (MeshPageCluster::MaxEdgeLength).
    float32 MaxEdgeLength = 0;
    /// The bounding sphere in mesh space: center, radius.
    float32 Sphere[4] = {};
    /// The backface cone in mesh space: axis, cutoff (1 or more for none).
    float32 Cone[4] = {0, 0, 0, 1};
    /// The packed positions' corner on the grid and bits per axis with the flags (MeshPageCluster).
    int32 PositionMin[3] = {};
    uint32 PositionBits = 0;
};
static_assert(sizeof(SceneGpuCluster) == 80);

struct SceneGpuVisibleCluster
{
    uint32 Instance = 0;
    uint32 Cluster = 0;
};
static_assert(sizeof(SceneGpuVisibleCluster) == 8);

/// A view's camera: its basis with the projection scales in w, and its position split like an instance's. Projection
/// is reversed-Z with an infinite far plane.
struct SceneGpuView
{
    float32 Right[4] = {};
    float32 Up[4] = {};
    float32 Forward[4] = {};
    int32 Tile[3] = {};
    uint32 Width = 0;
    float32 Offset[3] = {};
    uint32 Height = 0;
};
static_assert(sizeof(SceneGpuView) == 80);

/// An instance that changed this frame, for the scatter pass.
struct SceneGpuInstanceUpdate
{
    uint32 Slot = 0;
    uint32 Pad[3] = {};
    SceneGpuInstance Instance;
};
static_assert(sizeof(SceneGpuInstanceUpdate) == 80);
/// A view's lights, as Lighting.mshader's SceneLights: the unit direction towards the sun, its illuminance, and the
/// ambient illuminance from the sky, in lux; w unused.
struct SceneGpuLights
{
    float32 SunDirection[4] = {0, 1, 0, 0};
    float32 SunIlluminance[4] = {};
    float32 Ambient[4] = {};
};
static_assert(sizeof(SceneGpuLights) == 48);

} // namespace Marlin
