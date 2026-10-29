#include "ShaderPipeline/ShaderCompiler.h"

#include "Hash/Xxh3.h"
#include "LanguageFrontend/Compilation.h"
#include "LanguageFrontend/Source.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirReflection.h"
#include "LanguageIR/MirTarget.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
ShaderStage ToShaderStage(MirStage stage)
{
    switch (stage)
    {
    case MirStage::Compute:
        return ShaderStage::Compute;
    case MirStage::Amplification:
        return ShaderStage::Task;
    case MirStage::Mesh:
        return ShaderStage::Mesh;
    case MirStage::Vertex:
        return ShaderStage::Vertex;
    case MirStage::Pixel:
        return ShaderStage::Pixel;
    case MirStage::RayGeneration:
        return ShaderStage::RayGeneration;
    case MirStage::ClosestHit:
        return ShaderStage::ClosestHit;
    case MirStage::AnyHit:
        return ShaderStage::AnyHit;
    case MirStage::Miss:
        return ShaderStage::Miss;
    case MirStage::Intersection:
        return ShaderStage::Intersection;
    case MirStage::Callable:
        return ShaderStage::Callable;
    }
    return ShaderStage::Compute;
}

/// The files a compile used, the names it looked up and its whole source set.
void GetCompileDependencies(const LanguageCompilation &compilation, List<ShaderSourceDependency> &used,
                            ShaderNameDependencies &names, List<ShaderSourceDependency> &sourceSet)
{
    List<const SourceFile *> files;
    compilation.GetDependencies(files);
    for (const SourceFile *file : files)
    {
        used.Add({String(file->GetPath()), file->GetHash()});
    }
    LanguageNameDependencies looked;
    compilation.GetNameDependencies(looked);
    names.Names = Move(looked.Names);
    names.NamespaceNames = Move(looked.NamespaceNames);
    names.Extensions = looked.Extensions;
    files.Clear();
    compilation.GetSourceFiles(files);
    for (const SourceFile *file : files)
    {
        sourceSet.Add({String(file->GetPath()), file->GetHash()});
    }
}
} // namespace

void ShaderNameDependencies::Merge(const ShaderNameDependencies &other)
{
    LanguageNameDependencies merged;
    merged.Names = Names;
    merged.NamespaceNames = NamespaceNames;
    LanguageNameDependencies added;
    added.Names = other.Names;
    added.NamespaceNames = other.NamespaceNames;
    merged.Merge(added);
    Names = Move(merged.Names);
    NamespaceNames = Move(merged.NamespaceNames);
    Extensions = Extensions || other.Extensions;
}

void GetShaderSourceRoots(List<String> &roots)
{
    roots.Add(String("/Engine"));
    FileInfo project;
    if (Vfs::GetInfo("/Project", project))
    {
        roots.Add(String("/Project"));
    }
}

bool SharesVariant(const ShaderProfile &compiled, const ShaderProfile &profile, Span<const String> features)
{
    if (compiled.Target != profile.Target)
    {
        return false;
    }
    for (const String &feature : features)
    {
        if (compiled.Features.Contains(feature) != profile.Features.Contains(feature))
        {
            return false;
        }
    }
    return true;
}

ShaderCompileResult CompileShader(StringView entryPoint, const ShaderProfile &profile,
                                  const ShaderCompileOptions &options)
{
    ShaderCompileResult result;
    MirTargetRegistry targets;
    RegisterSpirvTarget(targets);
    const MirTarget *target = targets.Find(profile.Target.AsView());
    if (target == nullptr)
    {
        result.Errors = Format("profile {0} names the unknown target {1}", profile.Name, profile.Target);
        return result;
    }

    List<String> roots;
    GetShaderSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    LanguageCompileOptions compileOptions;
    compileOptions.Language = LanguageDialect::Shader;
    compileOptions.Features = profile.Features;
    compileOptions.DebugOutput = options.DebugOutput;
    compileOptions.CheckSpans = options.CheckSpans;
    compileOptions.DebugInfo = options.DebugInfo;
    compileOptions.Material = options.Material;
    for (const ShaderVariantValue &value : options.Variant)
    {
        LanguageVariantValue &variant = compileOptions.Variant.AddDefaulted();
        variant.Name = value.Name;
        variant.Value = value.Value;
    }
    LanguageCompilation compilation(sources, compileOptions);
    MirModule module;
    const bool compiled = compilation.Load() && compilation.CompileEntryPoint(entryPoint, module);
    GetCompileDependencies(compilation, result.Dependencies, result.Names, result.SourceSet);
    if (!compiled)
    {
        result.Errors = compilation.GetDiagnostics().ToString();
        return result;
    }

    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes;
    String error;
    if (!registry.ParsePipeline(MirShaderPipeline, passes, error))
    {
        result.Errors = Move(error);
        return result;
    }
    List<String> errors;
    target->AddLegalizationPasses(passes, errors);
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    if (!errors.IsEmpty() || !passes.GetErrors().IsEmpty())
    {
        result.Errors = String(!errors.IsEmpty() ? errors[0].AsView() : passes.GetErrors()[0].AsView());
        return result;
    }
    const Span<MirEntryPoint *const> entries = module.GetEntryPoints();
    if (entries.IsEmpty())
    {
        result.Errors = Format("{0} is not an entry point", entryPoint);
        return result;
    }
    MirReflection reflection;
    ReflectMirEntryPoint(module, *entries[0], reflection);
    compilation.GetQueriedFeatures(result.Features);
    for (const StringView feature : reflection.Features)
    {
        if (!result.Features.Contains(String(feature)))
        {
            result.Features.Add(String(feature));
        }
    }
    result.Features.Sort();
    for (const StringView feature : reflection.Features)
    {
        if (!profile.Features.Contains(String(feature)))
        {
            result.Unavailable = true;
            result.Succeeded = true;
            return result;
        }
    }
    MirTargetOptions targetOptions;
    targetOptions.DebugInfo = options.DebugInfo;
    List<uint8> code;
    if (!target->Emit(module, targetOptions, code, errors))
    {
        result.Errors = String(errors.IsEmpty() ? StringView("emission failed") : errors[0].AsView());
        return result;
    }

    ShaderVariant &variant = result.Variant;
    const FrozenString name(profile.Name.AsView());
    variant.Profiles = FrozenArray<FrozenString>(Span<const FrozenString>(&name, 1));
    variant.Stage = ToShaderStage(reflection.Stage);
    variant.GroupSizeX = reflection.GroupSize[0];
    variant.GroupSizeY = reflection.GroupSize[1];
    variant.GroupSizeZ = reflection.GroupSize[2];
    variant.ArgumentsSize = reflection.ArgumentsSize;
    variant.ArgumentsIndirect = reflection.ArgumentsIndirect;
    List<ShaderArgumentField> constants;
    for (const MirReflectedField &field : reflection.Arguments)
    {
        ShaderArgumentField &constant = constants.AddDefaulted();
        constant.Name = FrozenString(field.Name);
        constant.Offset = field.Offset;
        constant.Size = field.Size;
    }
    variant.Arguments = FrozenArray<ShaderArgumentField>(constants.AsSpan());
    List<FrozenString> formats;
    for (const StringView format : reflection.DebugFormats)
    {
        formats.Add(FrozenString(format));
    }
    variant.DebugFormats = FrozenArray<FrozenString>(formats.AsSpan());
    variant.DebugBufferOffset = reflection.DebugBufferOffset;
    List<ShaderSpecialization> specializations;
    for (const MirReflectedSpecialization &reflected : reflection.Specializations)
    {
        if (reflected.Type->GetSize() > 4)
        {
            result.Errors = Format("specialization constant {0} is wider than 32 bits", reflected.Name);
            return result;
        }
        ShaderSpecialization &specialization = specializations.AddDefaulted();
        specialization.Name = FrozenString(reflected.Name);
        specialization.Id = reflected.Id;
        specialization.DefaultBits = static_cast<uint32>(reflected.DefaultBits);
    }
    specializations.Sort([](const ShaderSpecialization &a, const ShaderSpecialization &b) { return a.Id < b.Id; });
    variant.Specializations = FrozenArray<ShaderSpecialization>(specializations.AsSpan());
    const Hash128 hash = Xxh3Hash128(code.Data(), code.Count());
    variant.CodeHashLow = hash.Low;
    variant.CodeHashHigh = hash.High;
    variant.Code = FrozenArray<uint8>(code.AsSpan());
    result.Succeeded = true;
    return result;
}

bool GetGlobalShaderEntryPoints(List<String> &entryPoints, String &errors)
{
    List<String> roots;
    GetShaderSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    LanguageCompileOptions options;
    options.Language = LanguageDialect::Shader;
    LanguageCompilation compilation(sources, options);
    if (!compilation.Load())
    {
        errors = compilation.GetDiagnostics().ToString();
        return false;
    }
    compilation.GetEntryPoints("/Language/Shaders/Passes/", entryPoints);
    return true;
}

bool ReflectMaterialShader(StringView material, Span<const ShaderVariantValue> variant,
                           ShaderMaterialReflection &reflection, String &errors)
{
    List<String> roots;
    GetShaderSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    LanguageCompileOptions options;
    options.Language = LanguageDialect::Shader;
    options.Material = String(material);
    for (const ShaderVariantValue &value : variant)
    {
        LanguageVariantValue &entry = options.Variant.AddDefaulted();
        entry.Name = value.Name;
        entry.Value = value.Value;
    }
    LanguageCompilation compilation(sources, options);
    LanguageMaterialReflection reflected;
    const bool ok = compilation.Load() && compilation.ReflectMaterial(reflected);
    GetCompileDependencies(compilation, reflection.Dependencies, reflection.Names, reflection.SourceSet);
    if (!ok)
    {
        errors = compilation.GetDiagnostics().ToString();
        return false;
    }
    reflection.Name = Move(reflected.Name);
    reflection.Id = Move(reflected.Id);
    reflection.Size = reflected.Size;
    reflection.Defaults = Move(reflected.Defaults);
    for (const LanguageMaterialParameter &source : reflected.Parameters)
    {
        ShaderMaterialParameter &parameter = reflection.Parameters.AddDefaulted();
        parameter.Name = source.Name;
        parameter.Type = source.Type;
        parameter.Offset = source.Offset;
        parameter.Size = source.Size;
        parameter.IsTexture = source.IsTexture;
        parameter.DefaultTexture = source.DefaultTexture;
        parameter.HasRange = source.HasRange;
        parameter.RangeMin = source.RangeMin;
        parameter.RangeMax = source.RangeMax;
        parameter.IsColor = source.IsColor;
    }
    for (const LanguageMaterialSwitch &source : reflected.Switches)
    {
        reflection.Switches.Add({source.Name, source.Default, source.ValueCount});
    }
    reflection.CustomBsdfs = Move(reflected.CustomBsdfs);
    return true;
}
} // namespace Marlin
