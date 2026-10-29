#pragma once

#include "AssetPipeline/SourceDecoder.h"
#include "CoreTypes.h"
#include "FbxSceneDecoder/FbxSceneDecoder.gen.h"

namespace Marlin
{
/// FBX, binary or ASCII, through ufbx, which converts the file's axes and units to engine space and bakes geometric
/// transforms into the vertices. Polygons are triangulated and UVs flipped to a top-left origin, as glTF.
MCLASS()
class FBXSCENEDECODER_API FbxSceneDecoder final : public SceneDecoder
{
    MGENERATED_BODY()
public:
    [[nodiscard]] Span<const StringView> GetExtensions() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    [[nodiscard]] bool CanDecode(Span<const uint8> header) const override;
    bool Decode(Span<const uint8> bytes, ISourceFileReader &files, StringView mesh, DecodedScene &scene,
                StringWriter &error) const override;
    /// Skips the geometry.
    bool DecodeOutline(Span<const uint8> bytes, ISourceFileReader &files, DecodedScene &scene,
                       StringWriter &error) const override;
};
} // namespace Marlin
