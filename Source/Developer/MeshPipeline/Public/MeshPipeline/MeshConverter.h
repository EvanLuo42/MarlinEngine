#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "CoreTypes.h"
#include "MarlinText/BinaryFileLink.h"
#include "Mesh/Mesh.h"
#include "String/String.h"
#include "MeshPipeline/MeshConverter.gen.h"

namespace Marlin
{
/// How a mesh of a scene file becomes a Mesh (Documentation/Architecture/06 §3.5).
MSTRUCT()
struct MeshSource
{
    MGENERATED_BODY()

    MPROPERTY()
    BinaryFileLink SourceFile;
    /// The mesh of the scene to convert, in its own space; empty merges every mesh the scene's nodes place, in scene
    /// space.
    MPROPERTY()
    String Mesh;
    /// Applied after the decoder's conversion to metres.
    MPROPERTY()
    float32 Scale = 1;
    /// Build the cluster DAG; off, the mesh keeps only its source triangles' clusters, in the same pages and groups,
    /// all of them drawn and resident.
    MPROPERTY()
    bool BuildDag = true;
    /// When merging every placed mesh, one slot per material instead of one per part (node) and material, as UE's
    /// slots by material: parts of one material then simplify together.
    MPROPERTY()
    bool MergePartsByMaterial = false;
    /// A default material for slots by name (`Part/Material`, or the material's name without parts); the others draw
    /// with the engine's Default material until an entity's RenderMesh overrides them (06 §3.5).
    MPROPERTY()
    HashMap<String, AssetRef<Material>> Slots;
};

/// The Engine.Meshes config section (06 §3.5); platform layers override it to match their GPUs' mesh shaders.
MSTRUCT(Config = "Engine.Meshes")
struct MeshSettings
{
    MGENERATED_BODY()
    // RECOVERY-DECISION TM-D6: the cluster limits default to Mesh's maxima, as Nanite's 128 triangles.

    /// At most Mesh::MaxClusterVertices.
    MPROPERTY()
    uint32 ClusterVertices = Mesh::MaxClusterVertices;
    /// At most Mesh::MaxClusterTriangles.
    MPROPERTY()
    uint32 ClusterTriangles = Mesh::MaxClusterTriangles;
    /// The streaming page the clusters are packed into; a group larger than this gets a page of its own.
    MPROPERTY()
    uint32 PageSize = 128 * 1024; // RECOVERY-DECISION TM-D10: Nanite's page size.
    /// The position grid: a power of two such that the mesh's largest extent spans at most 2^PositionBits steps, as
    /// Nanite's position precision. Clusters store their vertices as offsets on it in as few bits as they need.
    MPROPERTY()
    uint32 PositionBits = 18;
};

/// Turns a MeshSource into a Mesh: decodes the scene through SceneDecoder, welds the triangles of each slot (material,
/// and part when merging the scene's nodes), fills
/// in normals and MikkTSpace-compatible tangents the file lacks, and builds the cluster DAG with clusterlod: clusters
/// of the source triangles, grouped and simplified level by level with locked group borders, so any cut through the
/// DAG is crack-free (unless MeshSource::BuildDag is off: then only the source clusters, each group terminal). Groups
/// are then packed into pages, coarsest first.
MCLASS()
class MESHPIPELINE_API MeshConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "MeshSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Mesh>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 12;
    }
    bool Convert(ConversionContext &context) override;
};
} // namespace Marlin
