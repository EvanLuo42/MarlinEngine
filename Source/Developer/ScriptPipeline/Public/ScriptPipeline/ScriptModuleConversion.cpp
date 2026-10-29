#include "ScriptPipeline/ScriptModuleConversion.h"

#include "Asset/AssetBlock.h"
#include "Concurrency/ScopeLock.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "ScriptVM/ScriptBytecode.h"
#include "ScriptVM/ScriptModuleAsset.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"
#include "LanguageFrontend.SourceHash.gen.h"
#include "LanguageIR.SourceHash.gen.h"
#include "ScriptBackend.SourceHash.gen.h"
#include "ScriptPipeline.SourceHash.gen.h"

namespace Marlin
{
namespace
{
/// The key of a result: the input key and each source file with the hash of its text.
Hash128 MakeKey(const Hash128 &inputKey, Span<const ScriptSourceDependency> files)
{
    StringBuilder<4096> text;
    text.Append(Format("{0:x}{1:x}\n", inputKey.High, inputKey.Low));
    for (const ScriptSourceDependency &file : files)
    {
        text.Append(file.Path).Append(Format("\n{0:x}{1:x}\n", file.Hash.High, file.Hash.Low));
    }
    return ConversionStore::ResultKey(text.AsView());
}

/// The record that leads from the key of the sources, MakeKey, to the result's key by its content.
Hash128 ContentRecordKey(const Hash128 &sourceKey)
{
    return ConversionStore::DependencyKey(Format("ScriptModule\n{0:x}{1:x}", sourceKey.High, sourceKey.Low).AsView());
}

/// The result key of the ScriptModuleAsset *block*.
Hash128 ContentKey(Span<const uint8> block)
{
    const Hash128 hash = Xxh3Hash128(block.Data(), block.Count());
    return ConversionStore::ResultKey(Format("ScriptModule\n{0:x}{1:x}", hash.High, hash.Low).AsView());
}
} // namespace

bool ScriptModuleConversion::HashFile(const String &path, Hash128 &hash)
{
    FileInfo info;
    const bool known = Vfs::GetInfo(path.AsView(), info) && !info.IsDirectory && info.ModifiedTime != 0;
    if (known)
    {
        const ScopeLock lock(_filesLock);
        if (const KnownFile *file = _files.Find(path);
            file != nullptr && file->Size == info.Size && file->ModifiedTime == info.ModifiedTime)
        {
            hash = file->Hash;
            return true;
        }
    }
    String text;
    if (!Vfs::ReadAll(path.AsView(), text))
    {
        return false;
    }
    hash = Xxh3Hash128(text.Data(), text.Length());
    if (known)
    {
        // The time is from before the read, so a change during it is seen next time.
        const ScopeLock lock(_filesLock);
        _files.Set(path, {info.Size, info.ModifiedTime, hash});
    }
    return true;
}

ScriptModuleConversionResult ScriptModuleConversion::Convert(StringView package, const ConversionTarget &target,
                                                             const ScriptCompileOptions &options)
{
    ScriptModuleConversionResult result;
    List<String> paths;
    GetScriptSourceFiles(paths);
    StringBuilder<4096> description;
    description.Append(Format("ScriptModule {0}\n{1:x} {2:x} {3:x} {4:x}\n", Version, LANGUAGEFRONTEND_SOURCE_HASH,
                              LANGUAGEIR_SOURCE_HASH, SCRIPTBACKEND_SOURCE_HASH, SCRIPTPIPELINE_SOURCE_HASH));
    description.Append(Format("{0:x} {1} {2}\n", AssetBlock::GetLayoutHash(TypeOf<ScriptModuleAsset>()),
                              AssetBlockVersion, ScriptFormatVersion));
    description.Append(Format("{0}\n{1} {2}\n{3} {4}\n", package, target.Platform,
                              target.Kind == TargetKind::Server ? "Server" : "Client", options.DebugOutput,
                              options.DebugInfo));
    for (const String &path : paths)
    {
        description.Append(path).Append('\n');
    }
    const Hash128 inputKey = ConversionStore::DependencyKey(description.AsView());

    List<DependencyInput> inputs;
    if (_store.GetDependencies(inputKey, inputs))
    {
        List<ScriptSourceDependency> files;
        bool readable = true;
        for (const DependencyInput &input : inputs)
        {
            Hash128 hash;
            if (input.Kind != DependencyInputKind::Converter || !HashFile(input.Section, hash))
            {
                readable = false;
                break;
            }
            files.Add({input.Section, hash});
        }
        List<uint8> content;
        Hash128 key;
        if (readable && _store.GetRecord(ContentRecordKey(MakeKey(inputKey, files.AsSpan())), content) &&
            content.Count() == sizeof(Hash128))
        {
            MemCopy(&key, content.Data(), sizeof(Hash128));
            if (_store.LocateResult(key, result.Location))
            {
                result.Succeeded = true;
                result.CacheHit = true;
                result.Key = key;
                for (ScriptSourceDependency &file : files)
                {
                    result.Files.Add(Move(file.Path));
                }
                return result;
            }
        }
    }

    // A package without scripts, such as a project's that has none yet, gets an empty module rather than an error,
    // so loading it is quiet; a script added to it changes the file list and so the key.
    ScriptCompileResult compiled;
    List<uint8> bytes;
    const bool scripts = HasScripts(package, paths.AsSpan());
    if (scripts)
    {
        compiled = CompileScriptPackage(package, options);
    }
    else
    {
        compiled.Succeeded = true;
        for (String &path : paths)
        {
            Hash128 hash;
            if (HashFile(path, hash))
            {
                compiled.Dependencies.Add({Move(path), hash});
            }
        }
    }
    for (const ScriptSourceDependency &file : compiled.Dependencies)
    {
        result.Files.Add(file.Path);
    }
    if (!compiled.Succeeded)
    {
        result.Errors = Format("{0}:\n{1}", package, compiled.Errors);
        return result;
    }
    if (scripts)
    {
        WriteScriptModule(compiled.Module, bytes);
    }
    ScriptModuleAsset module;
    module.Name = FrozenString(package);
    module.Package = FrozenString(package);
    module.Bytecode = FrozenArray<uint8>(bytes.AsSpan());
    List<uint8> block;
    if (!AssetBlock::Write(module, GetScriptModuleGuid(package), block))
    {
        result.Errors = Format("{0}: the ScriptModuleAsset block could not be written", package);
        return result;
    }
    inputs.Clear();
    for (const ScriptSourceDependency &file : compiled.Dependencies)
    {
        DependencyInput &input = inputs.AddDefaulted();
        input.Kind = DependencyInputKind::Converter;
        input.Section = file.Path;
    }
    const Hash128 key = ContentKey(block.AsSpan());
    const Hash128 sourceKey = MakeKey(inputKey, compiled.Dependencies.AsSpan());
    // The result goes in before the records that lead to it.
    if (!_store.PutResult(key, block.AsSpan(), {}, result.Location) ||
        !_store.PutRecord(ContentRecordKey(sourceKey),
                          Span<const uint8>(reinterpret_cast<const uint8 *>(&key), sizeof(Hash128))) ||
        !_store.PutDependencies(inputKey, inputs.AsSpan()))
    {
        result.Errors = Format("{0}: GlobalStore could not store the result", package);
        return result;
    }
    result.Succeeded = true;
    result.Key = key;
    return result;
}
} // namespace Marlin
