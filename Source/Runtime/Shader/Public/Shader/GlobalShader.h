#pragma once

#include "Asset/Asset.h"
#include "Containers/FrozenArray.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Hash/Hash128.h"
#include "Misc/Guid.h"
#include "RHI/RhiPipeline.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Shader/GlobalShader.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(SHADER_API, LogShader, Info, Warning);

namespace Marlin
{
MENUM()
enum class ShaderStage : uint8
{
    Compute,
    Vertex,
    Pixel,
    Task,
    Mesh,
    RayGeneration,
    Miss,
    ClosestHit,
    AnyHit,
    Intersection,
    Callable,
};

[[nodiscard]] SHADER_API RhiShaderStage ToRhiShaderStage(ShaderStage stage);

/// A device profile (MarlinLanguage.md §9.7): a compiler target and the optional features it may use. Profiles are
/// the Engine.Shaders config section's, best first.
MSTRUCT()
struct ShaderProfile
{
    MGENERATED_BODY()

    /// Such as Vulkan.Baseline; part of every variant and DDC key.
    MPROPERTY()
    String Name;
    /// The RHI backend that runs it, as RhiDevice::GetBackendName gives it, such as Vulkan.
    MPROPERTY()
    String Rhi;
    /// The compiler target that writes its code, such as spirv.
    MPROPERTY()
    String Target;
    /// Feature names, the same as RhiDevice::HasFeature takes.
    MPROPERTY()
    List<String> Features;
};

/// The Engine.Shaders config section, as the converter reads it for a platform.
MSTRUCT()
struct ShaderSettings
{
    MGENERATED_BODY()

    MPROPERTY()
    List<ShaderProfile> Profiles;
};

/// A member of an entry point's [Uniform] parameter block (MarlinLanguage.md §9.3).
MSTRUCT()
struct ShaderArgumentField
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Name;
    MPROPERTY()
    uint32 Offset = 0;
    MPROPERTY()
    uint32 Size = 0;
};

/// A `[Specialization]` constant of an entry point (MarlinLanguage.md §9.4): its pipeline's value is the default unless
/// the declaration it is dispatched with sets another (ShaderLibrary::GetSpecializedShader).
MSTRUCT()
struct ShaderSpecialization
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Name;
    /// The constant's id, dense from zero in a variant.
    MPROPERTY()
    uint32 Id = 0;
    /// The default value's 32 bits: a float's bits, or 0 / 1 for a bool.
    MPROPERTY()
    uint32 DefaultBits = 0;
};

/// One entry point compiled once for the profiles that share its code, with what the engine needs to run it
/// (MarlinLanguage.md §12).
MSTRUCT()
struct ShaderVariant
{
    MGENERATED_BODY()

    /// The profiles it runs in: those with its target that agree on each feature its code asks about.
    MPROPERTY()
    FrozenArray<FrozenString> Profiles;
    MPROPERTY()
    ShaderStage Stage = ShaderStage::Compute;
    MPROPERTY()
    uint32 GroupSizeX = 1;
    MPROPERTY()
    uint32 GroupSizeY = 1;
    MPROPERTY()
    uint32 GroupSizeZ = 1;
    MPROPERTY()
    uint32 ArgumentsSize = 0;
    MPROPERTY()
    FrozenArray<ShaderArgumentField> Arguments;
    /// The block is larger than the arguments passed directly: ShaderLibrary writes it into upload memory and passes its
    /// address (§9.3).
    MPROPERTY()
    bool ArgumentsIndirect = false;
    /// The formats of `Debug.Print`, by the index its output records.
    MPROPERTY()
    FrozenArray<FrozenString> DebugFormats;
    /// Where among the arguments passed directly ShaderLibrary writes the debug output buffer's address (§9.11);
    /// ~0u if the variant prints nothing.
    MPROPERTY()
    uint32 DebugBufferOffset = ~0u;
    /// Its `[Specialization]` constants by id.
    MPROPERTY()
    FrozenArray<ShaderSpecialization> Specializations;
    /// Hash of Code: names the variant's pipelines.
    MPROPERTY()
    uint64 CodeHashLow = 0;
    MPROPERTY()
    uint64 CodeHashHigh = 0;
    MPROPERTY()
    FrozenArray<uint8> Code;

    [[nodiscard]] Hash128 GetCodeHash() const
    {
        return {CodeHashLow, CodeHashHigh};
    }
};

/// A global shader (MarlinLanguage.md §1.6): one entry point of the engine's or the project's
/// `Language/Shaders/Passes/` (or `Language/Shaders/Editor/`) by its qualified name, compiled for the profiles of the
/// platform it was converted for, once per group of profiles that share code. Profiles the entry point is unavailable
/// in (`[Requires]`) have no variant. Its Guid is GetGlobalShaderGuid of the name; it is not an asset of the content
/// tree but loads like one.
MCLASS()
class SHADER_API GlobalShader final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    FrozenString EntryPoint;
    MPROPERTY()
    FrozenArray<ShaderVariant> Variants;

    GlobalShader() = default;
    ~GlobalShader() override = default;
    MARLIN_NONCOPYABLE(GlobalShader);

    [[nodiscard]] const ShaderVariant *FindVariant(StringView profile) const;
};

/// A material's variant switch and its value (MarlinLanguage.md §9.4).
MSTRUCT()
struct ShaderVariantValue
{
    MGENERATED_BODY()

    MPROPERTY()
    String Name;
    MPROPERTY()
    int64 Value = 0;
};

/// A `[Specialization]` constant's value for a pipeline, by the constant's qualified name or its own name: its 32 bits
/// (BitCast a float).
struct ShaderSpecializationValue
{
    String Name;
    uint32 Bits = 0;
};

/// The key of a material pass compiled for one material and variant (§9.4): `entry|material|switch=value;…`, the
/// switches sorted by name. A global shader's key is its entry point; every shader loads under GetGlobalShaderGuid of
/// its key, and the stores and the indexer know it by its key.
[[nodiscard]] SHADER_API String MakeMaterialShaderKey(StringView entryPoint, StringView material,
                                                      Span<const ShaderVariantValue> variant);
/// *key* with specialization constant values: `key#name=bits;…`, sorted by name. It names a pipeline, not a shader:
/// the shader loads under the key without them (GetShaderKeyAsset).
[[nodiscard]] SHADER_API String MakeSpecializedShaderKey(StringView key, Span<const ShaderSpecializationValue> values);
/// The key of the shader a key loads: its part before the specialization values.
[[nodiscard]] SHADER_API StringView GetShaderKeyAsset(StringView key);
/// The entry point of a shader key, its part up to the first `|` or `#`.
[[nodiscard]] SHADER_API StringView GetShaderKeyEntryPoint(StringView key);
/// The material and variant of a material pass's key; an empty material for a global shader's. False if the key is
/// malformed.
SHADER_API bool ParseShaderKey(StringView key, StringView &material, List<ShaderVariantValue> &variant);
/// The specialization constant values of a key, none without them. False if they are malformed.
SHADER_API bool ParseShaderKeySpecializations(StringView key, List<ShaderSpecializationValue> &values);

/// The namespace of global shader Guids.
inline constexpr Guid GlobalShaderNamespace{0x5d1c7a0bu, 0x3e9f4c12u, 0x9a6b0f27u, 0xc84e1d35u};

/// The Guid a global shader loads under: the name-based Guid of its entry point's qualified name, the same in the
/// derived data cache and in packages.
[[nodiscard]] SHADER_API Guid GetGlobalShaderGuid(StringView entryPoint);
} // namespace Marlin
