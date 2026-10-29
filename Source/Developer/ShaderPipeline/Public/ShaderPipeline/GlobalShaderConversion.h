#pragma once

#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "GlobalStoreAssetStore/AssetLocate.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "ShaderPipeline/ShaderCompiler.h"
#include "ShaderPipeline/ShaderSources.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
struct GlobalShaderConversionResult
{
    bool Succeeded = false;
    bool CacheHit = false;
    Hash128 Key;
    ConvertedResultLocation Location;
    String Errors;
    /// The source files the result depends on, as virtual paths, so their changes can invalidate it.
    List<String> Files;
};

/// Converts a global shader (MarlinLanguage.md §1.6) through GlobalStore: its GlobalShader block holds a variant for every
/// profile of Engine.Shaders.Profiles. The input key covers the converter version, the source hashes of the compiler
/// modules, the entry point, the target, the profiles and the options; under it a dependency record keeps the source files the last
/// compile read, and the key adds the hashes of those files as they are now, so a hit needs no compile, and an edit to
/// the compiler or to any file the entry point used, down to the core library, misses. Beside it a ShaderNameRecord
/// keeps the names the compile looked up, so a new or changed file it did not use misses only when it declares one
/// of them.
class SHADERPIPELINE_API GlobalShaderConversion
{
public:
    /// Bump when the output changes for the same sources without a change to the compiler modules, such as a library
    /// update.
    static constexpr uint32 Version = 3;

    explicit GlobalShaderConversion(ConversionStore &store) : _store(store)
    {
    }

    /// Converts the shader of *key*: a global shader's entry point, or a material pass's MakeMaterialShaderKey.
    GlobalShaderConversionResult Convert(StringView key, const ConversionTarget &target,
                                         const ShaderCompileOptions &options);

    /// The key of the ShaderNameRecord of the result *resultKey*.
    [[nodiscard]] static Hash128 NameRecordKey(const Hash128 &resultKey);
    /// The key of the record that says *resultKey* is right for the source set with ShaderSourceTracker::HashSourceSet
    /// *sourceSet*.
    [[nodiscard]] static Hash128 ExactRecordKey(const Hash128 &resultKey, const Hash128 &sourceSet);

private:
    ConversionStore &_store;
    ShaderSourceTracker _sources;
};
} // namespace Marlin
