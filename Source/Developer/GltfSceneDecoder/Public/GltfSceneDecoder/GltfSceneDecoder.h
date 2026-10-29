#pragma once

#include "AssetPipeline/SourceDecoder.h"
#include "CoreTypes.h"
#include "GltfSceneDecoder/GltfSceneDecoder.gen.h"

namespace Marlin
{
/// glTF 2.0 through cgltf: .glb files, and .gltf files whose buffers are data URIs or separate files beside it, of
/// which only the ranges used are read (through ISourceFileReader). EXT_meshopt_compression is decompressed and
/// KHR_mesh_quantization's integer attributes become floats; EXT_mesh_gpu_instancing is not read. Of a material only the name is read: materials and their textures are Marlin's own. glTF already
/// is right-handed, Y up and in metres. Triangle strips and fans become lists; points and lines are skipped.
MCLASS()
class GLTFSCENEDECODER_API GltfSceneDecoder final : public SceneDecoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const StringView> GetExtensions() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 3;
    }
    [[nodiscard]] bool CanDecode(Span<const uint8> header) const override;
    /// Given a *mesh*, reads and decompresses only the buffer ranges that mesh's primitives use.
    bool Decode(Span<const uint8> bytes, ISourceFileReader &files, StringView mesh, DecodedScene &scene,
                StringWriter &error) const override;
    /// Skips the geometry.
    bool DecodeOutline(Span<const uint8> bytes, ISourceFileReader &files, DecodedScene &scene,
                       StringWriter &error) const override;
};
} // namespace Marlin
