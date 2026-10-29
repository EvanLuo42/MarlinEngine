#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Texture/Texture.h"
#include "Material/Material.gen.h"

namespace Marlin
{
/// The engine texture a texture parameter falls back to when the material binds none (Marlin.Shading.DefaultTexture).
MENUM()
enum class MaterialDefaultTexture : uint32
{
    None,
    White,
    Black,
    FlatNormal,
};

/// A variant switch of the material's struct and the value the material gives it (MarlinLanguage.md §9.4).
MSTRUCT()
struct MaterialSwitchValue
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Name;
    MPROPERTY()
    int64 Value = 0;
};

/// A texture parameter of the block: the renderer writes the texture's bindless index at Offset and the material
/// sampler's after it. Without a texture, the engine's Default texture.
MSTRUCT()
struct MaterialTextureBinding
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetRef<Texture> Texture;
    MPROPERTY()
    uint32 Offset = 0;
    MPROPERTY()
    MaterialDefaultTexture Default = MaterialDefaultTexture::White;
};

/// A material (Documentation/Architecture/07 §4.2, MarlinLanguage.md §1.6): the `[Material]` struct it draws with,
/// by qualified name, the values of the struct's variant switches, and its parameters as one element of the struct's
/// material table in GPU memory. Material and MaterialInstance sources both convert to it; an instance's parameters
/// are its parent's with its own overrides applied.
MCLASS()
class MATERIAL_API Material final : public Asset
{
    MGENERATED_BODY()
public:
    /// The `[Material]` struct, such as `Project.Materials.Rock`.
    MPROPERTY()
    FrozenString Shader;
    /// Every switch of the struct, in declaration order, with its value.
    MPROPERTY()
    FrozenArray<MaterialSwitchValue> Variant;
    /// The struct's value: what the material passes read from its table.
    MPROPERTY()
    FrozenArray<uint8> Parameters;
    MPROPERTY()
    FrozenArray<MaterialTextureBinding> Textures;
    /// The custom BSDFs its closures can carry, by qualified name: the lighting passes compiled for them (07 §4.3).
    MPROPERTY()
    FrozenArray<FrozenString> CustomBsdfs;

    Material() = default;
    ~Material() override = default;
    MARLIN_NONCOPYABLE(Material);
};
} // namespace Marlin
