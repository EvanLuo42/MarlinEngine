#include "ShaderPipeline/GlobalShaderConversion.h"

#include "Asset/AssetBlock.h"
#include "AssetPipeline/AssetConverter.h"
#include "Concurrency/ScopeLock.h"
#include "Config/Config.h"
#include "Hash/Xxh3.h"
#include "LanguageFrontend.SourceHash.gen.h"
#include "LanguageIR.SourceHash.gen.h"
#include "ShaderBackendSpirv.SourceHash.gen.h"
#include "ShaderPipeline.SourceHash.gen.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
/// The key of a result: the input key and each source file with the hash of its text.
Hash128 MakeKey(const Hash128 &inputKey, Span<const ShaderSourceDependency> files)
{
    StringBuilder<4096> text;
    text.Append(Format("{0:x}{1:x}\n", inputKey.High, inputKey.Low));
    for (const ShaderSourceDependency &file : files)
    {
        text.Append(file.Path).Append(Format("\n{0:x}{1:x}\n", file.Hash.High, file.Hash.Low));
    }
    return ConversionStore::ResultKey(text.AsView());
}
} // namespace

Hash128 GlobalShaderConversion::NameRecordKey(const Hash128 &resultKey)
{
    return ConversionStore::DependencyKey(Format("ShaderNames\n{0:x}{1:x}", resultKey.High, resultKey.Low).AsView());
}

Hash128 GlobalShaderConversion::ExactRecordKey(const Hash128 &resultKey, const Hash128 &sourceSet)
{
    return ConversionStore::DependencyKey(Format("ShaderExact\n{0:x}{1:x}\n{2:x}{3:x}", resultKey.High,
                                                 resultKey.Low, sourceSet.High, sourceSet.Low)
                                              .AsView());
}

GlobalShaderConversionResult GlobalShaderConversion::Convert(StringView shaderKey, const ConversionTarget &target,
                                                             const ShaderCompileOptions &givenOptions)
{
    GlobalShaderConversionResult result;
    // A material pass's key names its entry point, material and variant; the key is the shader's name throughout.
    const StringView entryPoint = shaderKey;
    const StringView function = GetShaderKeyEntryPoint(shaderKey);
    ShaderCompileOptions options = givenOptions;
    StringView material;
    if (!ParseShaderKey(shaderKey, material, options.Variant))
    {
        result.Errors = Format("{0} is not a shader key", shaderKey);
        return result;
    }
    options.Material = String(material);
    const ConfigView *config = GetConversionConfig(target);
    if (config == nullptr)
    {
        result.Errors = Format("the config of {0} cannot be read", target.Platform);
        return result;
    }
    ShaderSettings settings;
    (void)config->Apply("Engine.Shaders", TypeOf<ShaderSettings>(), &settings);
    if (settings.Profiles.IsEmpty())
    {
        result.Errors = "Engine.Shaders.Profiles names no profile";
        return result;
    }

    StringBuilder<1024> description;
    description.Append(Format("GlobalShader {0}\n{1:x} {2:x} {3:x} {4:x}\n", Version, LANGUAGEFRONTEND_SOURCE_HASH,
                              LANGUAGEIR_SOURCE_HASH, SHADERBACKENDSPIRV_SOURCE_HASH, SHADERPIPELINE_SOURCE_HASH));
    description.Append(Format("{0:x} {1}\n", AssetBlock::GetLayoutHash(TypeOf<GlobalShader>()), AssetBlockVersion));
    description.Append(Format("{0}\n{1} {2}\n", entryPoint, target.Platform,
                              target.Kind == TargetKind::Server ? "Server" : "Client"));
    for (const ShaderProfile &profile : settings.Profiles)
    {
        description.Append(Format("{0} {1}", profile.Name, profile.Target));
        for (const String &feature : profile.Features)
        {
            description.Append(' ').Append(feature);
        }
        description.Append('\n');
    }
    description.Append(Format("{0} {1} {2}\n", options.DebugOutput, options.DebugInfo, options.CheckSpans));
    const Hash128 inputKey = ConversionStore::DependencyKey(description.AsView());

    // The record under the input key, then the redirects from each key that turned out missing or wrong
    // (ConversionStore::RedirectKey). A result is right when it was compiled from exactly the source set there is
    // now, or when no file the compile did not use now declares a name it looked up.
    List<ShaderSourceDependency> sourceSet;
    Hash128 sourceSetHash;
    const bool listed = _sources.GetSourceSet(sourceSet, sourceSetHash);
    Hash128 missed;
    bool hasMissed = false;
    List<DependencyInput> inputs;
    for (uint32 round = 0; round <= ConversionStore::MaxRedirects; ++round)
    {
        if (!_store.GetDependencies(hasMissed ? ConversionStore::RedirectKey(missed) : inputKey, inputs))
        {
            break;
        }
        List<ShaderSourceDependency> files;
        bool readable = true;
        for (const DependencyInput &input : inputs)
        {
            Hash128 hash;
            if (input.Kind != DependencyInputKind::Converter || !_sources.HashFile(input.Section.AsView(), hash))
            {
                readable = false;
                break;
            }
            files.Add({input.Section, hash});
        }
        if (!readable)
        {
            break;
        }
        const Hash128 key = MakeKey(inputKey, files.AsSpan());
        List<uint8> bytes;
        ShaderNameRecord record;
        bool right = false;
        if (_store.LocateResult(key, result.Location))
        {
            right = (listed && _store.GetRecord(ExactRecordKey(key, sourceSetHash), bytes)) ||
                    (listed && _store.GetRecord(NameRecordKey(key), bytes) &&
                     ReadShaderNameRecord(bytes.AsSpan(), record) &&
                     _sources.KeepsNames(record, files.AsSpan(), sourceSet.AsSpan()));
        }
        if (right)
        {
            result.Succeeded = true;
            result.CacheHit = true;
            result.Key = key;
            for (ShaderSourceDependency &file : files)
            {
                result.Files.Add(Move(file.Path));
            }
            return result;
        }
        missed = key;
        hasMissed = true;
    }

    GlobalShader shader;
    shader.Name = FrozenString(entryPoint);
    shader.EntryPoint = FrozenString(entryPoint);
    /// A profile compiled for, the features its result depends on, and its variant, or -1 if unavailable there.
    struct Compiled
    {
        const ShaderProfile *Profile = nullptr;
        List<String> Features;
        int32 Variant = -1;
    };
    List<Compiled> compiledProfiles;
    List<ShaderVariant> variants;
    List<List<FrozenString>> variantProfiles;
    List<ShaderSourceDependency> files;
    ShaderNameRecord names;
    for (const ShaderProfile &profile : settings.Profiles)
    {
        const Compiled *shared = nullptr;
        for (const Compiled &entry : compiledProfiles)
        {
            if (SharesVariant(*entry.Profile, profile, entry.Features.AsSpan()))
            {
                shared = &entry;
                break;
            }
        }
        if (shared != nullptr)
        {
            if (shared->Variant >= 0)
            {
                variantProfiles[static_cast<usize>(shared->Variant)].Add(FrozenString(profile.Name.AsView()));
            }
            continue;
        }
        ShaderCompileResult compiled = CompileShader(function, profile, options);
        for (ShaderSourceDependency &dependency : compiled.Dependencies)
        {
            bool known = false;
            for (const ShaderSourceDependency &file : files)
            {
                known = known || file.Path == dependency.Path;
            }
            if (!known)
            {
                files.Add(Move(dependency));
            }
        }
        names.Names.Merge(compiled.Names);
        if (names.SourceSet.IsEmpty())
        {
            names.SourceSet = Move(compiled.SourceSet);
        }
        if (!compiled.Succeeded)
        {
            result.Errors = Format("{0} for {1}:\n{2}", entryPoint, profile.Name, compiled.Errors);
            for (ShaderSourceDependency &file : files)
            {
                result.Files.Add(Move(file.Path));
            }
            return result;
        }
        Compiled &entry = compiledProfiles.AddDefaulted();
        entry.Profile = &profile;
        entry.Features = Move(compiled.Features);
        if (!compiled.Unavailable)
        {
            entry.Variant = static_cast<int32>(variants.Count());
            variants.Add(Move(compiled.Variant));
            variantProfiles.AddDefaulted().Add(FrozenString(profile.Name.AsView()));
        }
    }
    for (usize index = 0; index < variants.Count(); ++index)
    {
        variants[index].Profiles = FrozenArray<FrozenString>(variantProfiles[index].AsSpan());
    }
    shader.Variants = FrozenArray<ShaderVariant>(variants.AsSpan());
    files.Sort([](const ShaderSourceDependency &a, const ShaderSourceDependency &b) { return a.Path < b.Path; });

    List<uint8> block;
    if (!AssetBlock::Write(shader, GetGlobalShaderGuid(entryPoint), block))
    {
        result.Errors = Format("{0}: the GlobalShader block could not be written", entryPoint);
        return result;
    }
    inputs.Clear();
    for (const ShaderSourceDependency &file : files)
    {
        DependencyInput &input = inputs.AddDefaulted();
        input.Kind = DependencyInputKind::Converter;
        input.Section = file.Path;
    }
    const Hash128 key = MakeKey(inputKey, files.AsSpan());
    List<uint8> nameBytes;
    WriteShaderNameRecord(names, nameBytes);
    const uint8 exact = 1;
    // The result and its records go in before the dependency record that leads to them. The one under the input key
    // is the first compile's; a compile that used other files than the record that led here redirects from the key
    // that record gave. The exact record says the result is right for the source set it was compiled from, so a
    // compile that a new file only looked like it could change is not repeated.
    if (!_store.PutResult(key, block.AsSpan(), {}, result.Location) ||
        !_store.PutRecord(NameRecordKey(key), nameBytes.AsSpan()) ||
        !_store.PutRecord(ExactRecordKey(key, ShaderSourceTracker::HashSourceSet(names.SourceSet.AsSpan())),
                          Span<const uint8>(&exact, 1)) ||
        (hasMissed && missed != key &&
         !_store.PutDependencies(ConversionStore::RedirectKey(missed), inputs.AsSpan())) ||
        !_store.PutDependencies(inputKey, inputs.AsSpan()))
    {
        result.Errors = Format("{0}: GlobalStore could not store the result", entryPoint);
        return result;
    }
    result.Succeeded = true;
    result.Key = key;
    for (ShaderSourceDependency &file : files)
    {
        result.Files.Add(Move(file.Path));
    }
    return result;
}
} // namespace Marlin
