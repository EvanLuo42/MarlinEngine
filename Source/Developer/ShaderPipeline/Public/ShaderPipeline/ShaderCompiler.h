#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Shader/GlobalShader.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
struct ShaderCompileOptions
{
    /// Whether `Debug.Print` and `Debug.Assert` generate code (MarlinLanguage.md §9.11).
    bool DebugOutput = false;
    /// With DebugOutput, Span accesses out of range or through a null Span are reported (§9.11).
    bool CheckSpans = false;
    /// Whether the code keeps what debuggers need to map it back to the source.
    bool DebugInfo = false;
    /// For a material pass (a generic entry point, MarlinLanguage.md §9.4): the `[Material]` struct it is compiled
    /// for, by qualified name, and the values of its variant switches; empty for a global shader.
    String Material;
    List<ShaderVariantValue> Variant;
};

/// A source file a compile read, as a virtual path, with the hash of its text as it was read.
struct ShaderSourceDependency
{
    String Path;
    Hash128 Hash;
};

/// The names a compile looked up outside local scopes, as the front end's LanguageNameDependencies: hashes of a
/// container's qualified name with a member name, sorted. Names found a namespace in NamespaceNames, the others in
/// Names; Extensions when the extensions of some type were matched.
struct ShaderNameDependencies
{
    List<uint64> Names;
    List<uint64> NamespaceNames;
    bool Extensions = false;

    /// Adds the names of *other*, as when one result covers several compiles.
    SHADERPIPELINE_API void Merge(const ShaderNameDependencies &other);
};

struct ShaderCompileResult
{
    bool Succeeded = false;
    /// The entry point requires a feature the profile lacks (MarlinLanguage.md §9.7): it has no variant there.
    bool Unavailable = false;
    String Errors;
    ShaderVariant Variant;
    /// The files whose declarations the compile used.
    List<ShaderSourceDependency> Dependencies;
    /// The names it looked up and every file of the source set, for ShaderNameRecord (ShaderSources.h).
    ShaderNameDependencies Names;
    List<ShaderSourceDependency> SourceSet;
    /// The features the result depends on, sorted: those `Device.Supports` asked about and those the entry point
    /// requires. Another profile with these features or their absence alike gets the same result (SharesVariant).
    List<String> Features;
};

/// A material parameter: a public field of a `[Material]` struct at its place in the struct's GPU layout
/// (MarlinLanguage.md §9.4, §12).
struct ShaderMaterialParameter
{
    String Name;
    /// The type as the language writes it, such as `half3` or `Marlin.Shading.MaterialTexture2D<half4>`.
    String Type;
    uint32 Offset = 0;
    uint32 Size = 0;
    /// A texture parameter: its texture handle at Offset, its sampler after it, and the engine texture it defaults to
    /// (Marlin.Shading.DefaultTexture).
    bool IsTexture = false;
    uint32 DefaultTexture = 0;
    bool HasRange = false;
    float64 RangeMin = 0;
    float64 RangeMax = 0;
    bool IsColor = false;
};

/// A variant switch of a material, its default and how many values it takes.
struct ShaderMaterialSwitch
{
    String Name;
    int64 Default = 0;
    uint32 ValueCount = 0;
};

/// What the material converter needs of a `[Material]` struct (§12): its `[Id]`, its parameters and their defaults
/// in the layout the material table holds, its switches, and the source files it was read from.
struct ShaderMaterialReflection
{
    String Name;
    String Id;
    uint32 Size = 0;
    List<uint8> Defaults;
    List<ShaderMaterialParameter> Parameters;
    List<ShaderMaterialSwitch> Switches;
    /// The custom BSDFs its closures can carry, by qualified name.
    List<String> CustomBsdfs;
    /// As in ShaderCompileResult.
    List<ShaderSourceDependency> Dependencies;
    ShaderNameDependencies Names;
    List<ShaderSourceDependency> SourceSet;
};

/// Reflects the `[Material]` struct *material* of the mounted source set with *variant* applied; false with *errors*.
SHADERPIPELINE_API bool ReflectMaterialShader(StringView material, Span<const ShaderVariantValue> variant,
                                              ShaderMaterialReflection &reflection, String &errors);

/// True if compiling for *profile* gives what compiling for *compiled* gave, where *features* are the result's
/// Features: the same target, and each of those features in both profiles or in neither.
[[nodiscard]] SHADERPIPELINE_API bool SharesVariant(const ShaderProfile &compiled, const ShaderProfile &profile,
                                                    Span<const String> features);

/// The source roots of global shaders: /Engine, and /Project if one is mounted.
SHADERPIPELINE_API void GetShaderSourceRoots(List<String> &roots);

/// The global shaders (MarlinLanguage.md §1.6) of the mounted source set: the entry points under
/// `Language/Shaders/Passes/` of every source root, by qualified name; false with the errors if the set does not load.
SHADERPIPELINE_API bool GetGlobalShaderEntryPoints(List<String> &entryPoints, String &errors);

/// Compiles one entry point of the mounted source set for one profile: front end, the MIR shader pipeline, the
/// profile's target's legalization and emission, and the reflection the engine runs it with.
SHADERPIPELINE_API ShaderCompileResult CompileShader(StringView entryPoint, const ShaderProfile &profile,
                                                     const ShaderCompileOptions &options);
} // namespace Marlin
