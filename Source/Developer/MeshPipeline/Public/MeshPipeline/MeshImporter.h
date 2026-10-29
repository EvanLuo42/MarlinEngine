#pragma once

#include "AssetPipeline/AssetImporter.h"
#include "CoreTypes.h"
#include "MeshPipeline/MeshImporter.gen.h"

namespace Marlin
{
/// What importing a scene file produces (Documentation/Architecture/06 §3.5), as UE's Combine Static Meshes.
MSTRUCT()
struct MeshImportOptions
{
    MGENERATED_BODY()

    /// One MeshSource for the whole file, every placed mesh merged in scene space, with a material slot per part (node)
    /// and material. Otherwise a MeshSource per mesh of the scene and a Prefab that places them as the file's nodes do.
    MPROPERTY()
    bool Combine = true;
};

/// Imports every scene format a SceneDecoder reads, as MeshImportOptions says: one combined MeshSource named after the
/// file, or a MeshSource per mesh (named after the file, and after the mesh when the scene has several) and a Prefab
/// with an entity per node.
MCLASS()
class MESHPIPELINE_API MeshImporter final : public AssetImporter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] bool CanImport(const ImportContext &context) const override;
    bool Import(ImportContext &context) const override;
    [[nodiscard]] const TypeInfo *GetOptionsType() const override
    {
        return &TypeOf<MeshImportOptions>();
    }
};

/// The part names of *scene*'s nodes, as both the importer and the converter give them: a node's own name, Node{n}
/// for the n-th node if it has none, and a _2, _3, ... suffix on later nodes of a name already taken.
MESHPIPELINE_API void GetScenePartNames(const DecodedScene &scene, List<String> &names);
} // namespace Marlin
