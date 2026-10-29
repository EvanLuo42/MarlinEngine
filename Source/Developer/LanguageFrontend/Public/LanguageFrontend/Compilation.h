#pragma once

#include "Containers/List.h"
#include "LanguageFrontend/Diagnostics.h"
#include "LanguageFrontend/NameDependencies.h"
#include "LanguageFrontend/Source.h"
#include "LanguageIR/MirModule.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"

namespace Marlin
{
class Sema;

/// The value of a constant generic parameter of the material being compiled, for the variant being compiled (§9.4).
struct LanguageVariantValue
{
    /// The name of the material's generic parameter.
    String Name;
    int64 Value = 0;
};

/// A material parameter (MarlinLanguage §9.4, §12): a public field of the material struct, at its place in the
/// struct's GPU layout, the material table's element.
struct LanguageMaterialParameter
{
    String Name;
    /// The type as the language writes it, such as `half3` or `Marlin.Shading.MaterialTexture2D<half4>`.
    String Type;
    uint32 Offset = 0;
    uint32 Size = 0;
    /// A `MaterialTexture2D`: its texture handle at Offset, its sampler handle after it; the engine texture it
    /// defaults to (Marlin.Shading.DefaultTexture) in DefaultTexture.
    bool IsTexture = false;
    uint32 DefaultTexture = 0;
    /// `[Range(min, max)]`.
    bool HasRange = false;
    double RangeMin = 0;
    double RangeMax = 0;
    /// `[Color]`.
    bool IsColor = false;
};

/// A variant switch of a material: a constant generic parameter, its default and how many values it takes.
struct LanguageMaterialSwitch
{
    String Name;
    int64 Default = 0;
    uint32 ValueCount = 0;
};

/// What the material converter and the renderer need of a material struct (§12): its parameters and their
/// defaults in the layout the material table holds, its `[Id]` and its variant switches.
struct LanguageMaterialReflection
{
    /// The fully qualified name and the `[Id]` text.
    String Name;
    String Id;
    /// The struct's size in GPU memory, and its default value there: every field's initializer, or zero.
    uint32 Size = 0;
    List<uint8> Defaults;
    List<LanguageMaterialParameter> Parameters;
    List<LanguageMaterialSwitch> Switches;
    /// The custom BSDFs its closures can carry, by qualified name, found by compiling its material pass (07 §4.3).
    List<String> CustomBsdfs;
};

struct LanguageCompileOptions
{
    /// The language being compiled: Shader or Script. The source set is that dialect's files and the Common ones.
    // RECOVERY-DECISION lang-lower-D11: Shader by default (lang-fe-syntax Q-syntax-1), as ShaderCompiler and rev-84
    // callers assume; script compilations set Script.
    LanguageDialect Language = LanguageDialect::Shader;
    /// The features of the profile being compiled for (§9.7); `Device.Supports` is true for exactly these.
    List<String> Features;
    /// The fully qualified name of the `[Material]` struct that a generic entry point, a material pass, is
    /// instantiated with; empty for other entry points.
    String Material;
    /// The values of the material's constant generic parameters; omitted ones take their defaults.
    List<LanguageVariantValue> Variant;
    /// Most `static` parameter combinations a material may have.
    uint32 VariantBudget = 256;
    /// Steps a single compile-time evaluation may take (§6.2).
    uint32 EvaluationLimit = 1000000;
    /// Whether `Debug.Print` and `Debug.Assert` generate code (§9.11); when false they are removed with their
    /// arguments.
    bool DebugOutput = false;
    /// With DebugOutput, a Span access past the Span's end or through a null one is reported like a failed
    /// `Debug.Assert`, besides being clamped (§4.5, §9.11).
    bool CheckSpans = false;
    /// Whether the lowered MIR keeps source locations and local variables for debuggers.
    // RECOVERY-DECISION lang-lower-D11: off by default (lang-fe-syntax Q-syntax-1), like ShaderCompilerOptions.
    bool DebugInfo = false;
};

/// One compilation of a source set: scans every file once, then checks and lowers entry points on demand, parsing
/// and checking only the declarations they use (§1.3, §11.1).
class LANGUAGEFRONTEND_API LanguageCompilation
{
public:
    LanguageCompilation(ISourceProvider &sources, const LanguageCompileOptions &options);
    ~LanguageCompilation();
    MARLIN_NONCOPYABLE(LanguageCompilation);

    /// Reads and scans the source set; false if a file could not be read or declarations clash.
    bool Load();

    [[nodiscard]] LanguageDiagnostics &GetDiagnostics();

    /// Checks the entry point function *qualifiedName*, such as `Marlin.Post.Tonemap`, with everything it uses,
    /// and lowers it into *module*. False with diagnostics if anything is wrong.
    bool CompileEntryPoint(StringView qualifiedName, MirModule &module);

    /// Checks every declaration of the script package *package*, its mount root such as `Engine` or `Project`
    /// (MarlinScript), without lowering it. False with diagnostics if anything is wrong.
    bool CheckScriptPackage(StringView package);

    /// Checks the script package *package* like CheckScriptPackage and lowers all of it, with the core library it
    /// uses, into *module*.
    bool CompileScriptPackage(StringView package, MirModule &module);

    /// The files whose declarations the compiled entry points used, for the DDC key (§1.3).
    void GetDependencies(List<const SourceFile *> &files) const;
    /// The names the compiled entry points looked up, so files outside GetDependencies can be checked for
    /// declarations that would change the result (NameDependencies.h).
    void GetNameDependencies(LanguageNameDependencies &names) const;
    /// Every file of the source set, as Load read it.
    void GetSourceFiles(List<const SourceFile *> &files) const;
    /// The features `Device.Supports` was asked about by the compiled entry points, sorted: the code depends on the
    /// profile only through them, so profiles that agree on each of them get the same code (§9.7).
    void GetQueriedFeatures(List<String> &features) const;
    /// The fully qualified names of the entry points declared in the files under *folder*, such as
    /// `/Language/Shaders/Passes/` in any mount root, after Load.
    void GetEntryPoints(StringView folder, List<String> &names) const;
    /// The fully qualified names of the `[Material]` structs declared in the files under *folder*, after Load.
    void GetMaterials(StringView folder, List<String> &names) const;
    /// Reflects the material LanguageCompileOptions::Material names, with LanguageCompileOptions::Variant applied.
    /// False with diagnostics if it is not a valid material.
    bool ReflectMaterial(LanguageMaterialReflection &reflection);

private:
    UniquePtr<Sema> _sema;
};
} // namespace Marlin
