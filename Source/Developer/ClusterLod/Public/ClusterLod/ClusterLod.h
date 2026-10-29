#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Math/Vector.h"

namespace Marlin
{
/// How BuildClusterLod clusters, groups and simplifies; the defaults are clusters for rasterization.
struct ClusterLodSettings
{
    /// Simplify level by level; off, only the input clusters come out, grouped, every group terminal at depth 0.
    bool BuildDag = true;

    /// The largest cluster; clusters aim at no fewer than MinTriangles.
    uint32 MaxVertices = 128;
    uint32 MinTriangles = 42;
    uint32 MaxTriangles = 128;

    /// Clusters per group, as a target: groups can be up to a third larger.
    uint32 GroupSize = 16;
    /// Group clusters that lie close as well as those that share edges.
    bool GroupSpatially = true;
    /// Order the groups of a level along a space-filling curve.
    bool SortGroups = false;

    /// Cluster by position alone (better for ray tracing) instead of by connectivity; FillWeight steers how full
    /// spatial clusters are, SplitFactor how readily connected ones split.
    bool ClusterSpatially = false;
    float32 FillWeight = 0;
    float32 SplitFactor = 2;

    /// Each level aims at this fraction of the triangles; a group that keeps more than Threshold of them is stuck and
    /// ends its branch of the DAG.
    float32 SimplifyRatio = 0.5f;
    float32 SimplifyThreshold = 0.85f;
    /// A group's error is max(previous * MergePrevious, current) + current * MergeAdditive.
    float32 ErrorMergePrevious = 1;
    float32 ErrorMergeAdditive = 0;
    /// Scales the error of a group simplified by the sloppy fallback, for its worse appearance.
    float32 SloppyErrorFactor = 2;
    /// Above zero: the error is at most this many times the longest short edge of the group, so that dense groups
    /// lose sub-pixel triangles even when their attribute error is high.
    float32 EdgeErrorLimit = 0;
    /// Keep attribute error on the scale of position error.
    bool ClampError = true;
    /// Simplify across attribute seams that are not protected; otherwise only when stuck, if PermissiveFallback.
    bool Permissive = true;
    bool PermissiveFallback = false;
    /// Fall back to sloppy simplification when stuck.
    bool SloppyFallback = true;
    /// Even out triangle density, at some cost in triangle count; for deformable meshes.
    bool Regularize = false;
    /// Keep folds between opposite-facing triangles.
    bool PreserveFolds = false;

    /// Compute each simplified cluster's bounds from its own triangles instead of its group's.
    bool TightBounds = false;
    /// Reorder each cluster's triangles and vertices for locality, at this level (0 to 3) of effort.
    bool OptimizeClusters = true;
    uint32 OptimizeLevel = 1;
};

/// The triangles to build from. Positions are three floats; Attributes, if any, AttributeCount floats weighted by
/// AttributeWeights for attribute-aware simplification. Strides are in bytes.
struct ClusterLodInput
{
    Span<const uint32> Indices;
    uint32 VertexCount = 0;
    const float32 *Positions = nullptr;
    usize PositionStride = 3 * sizeof(float32);
    const float32 *Attributes = nullptr;
    usize AttributeStride = 0;
    Span<const float32> AttributeWeights;
    /// meshopt_SimplifyVertex_* flags per vertex, or empty; a set Protect bit keeps that vertex's attribute seams.
    Span<const uint8> VertexLocks;
    /// Bit K protects the seams of attribute K in permissive simplification.
    uint32 ProtectMask = 0;
};

/// A bounding sphere and the simplification error of what is inside it, in the input's space.
struct ClusterLodBounds
{
    Vector3 Center;
    float32 Radius = 0;
    float32 Error = 0;
};

/// Up to MaxTriangles triangles, as ClusterLodResult::Indices[FirstIndex, FirstIndex + IndexCount) into the input
/// vertices, with VertexCount distinct vertices among them.
struct ClusterLodCluster
{
    /// The group whose simplification produced this cluster, or -1 for the input triangles.
    int32 Refined = -1;
    /// For culling only: Error is not monotonic across the DAG.
    ClusterLodBounds Bounds;
    uint32 Group = 0;
    uint32 FirstIndex = 0;
    uint32 IndexCount = 0;
    uint32 VertexCount = 0;
};

/// Clusters simplified together, a node of the DAG. A cluster is drawn when its group's Simplified error is too large
/// on screen and its Refined group's (if any) is not. Simplified.Error is the largest float for a terminal group.
struct ClusterLodGroup
{
    uint32 Depth = 0;
    ClusterLodBounds Simplified;
    uint32 FirstCluster = 0;
    uint32 ClusterCount = 0;
};

struct ClusterLodResult
{
    List<ClusterLodGroup> Groups;
    List<ClusterLodCluster> Clusters;
    List<uint32> Indices;
    /// One more than the deepest group's depth.
    uint32 DepthCount = 0;
};

/// Builds the cluster LOD DAG of *input*: clusters the triangles, then, level by level, partitions the clusters into
/// groups, locks each group's border, simplifies each group to about half and clusters the result again, until one
/// cluster is left or every group is stuck (without BuildDag, stops after the first partition). The groups of a level
/// are simplified in parallel jobs; the result does not depend on how many workers ran them. Groups come out level by
/// level, a group's clusters consecutive. Equivalent to meshoptimizer's clodBuild (clusterlod.h), whose algorithm this
/// follows, without border dilation, up to a million triangles; above that, the first clustering runs in parallel over
/// spatially sorted runs of a quarter million triangles, so clusters where the runs meet may be less full.
CLUSTERLOD_API void BuildClusterLod(const ClusterLodInput &input, const ClusterLodSettings &settings,
                                    ClusterLodResult &result);

/// A node of the bounding volume hierarchy over the groups: bounds holding the worst error below; a leaf's Group, or
/// an inner node's Children nodes from FirstChild.
struct ClusterLodNode
{
    ClusterLodBounds Bounds;
    int32 Group = -1;
    uint32 FirstChild = 0;
    uint32 ChildCount = 0;
};

/// One tree per depth over *groups*, which need not be in BuildClusterLod's order: nodes 0 to depthCount - 1 are the
/// roots, and each inner node has up to *width* children, which lie close together; a group's index is its position
/// in *groups*. Traversal descends while a node's error is too large on screen.
CLUSTERLOD_API void BuildClusterLodHierarchy(Span<const ClusterLodGroup> groups, uint32 depthCount, uint32 width,
                                             List<ClusterLodNode> &nodes);

/// *indices* of one cluster made cluster-local: *vertices* gets each distinct vertex once, in first use, and
/// *triangles* the position of each corner's vertex in it, so that vertices[triangles[i]] == indices[i].
CLUSTERLOD_API void GetClusterLocalIndices(Span<const uint32> indices, List<uint32> &vertices, List<uint8> &triangles);
} // namespace Marlin
