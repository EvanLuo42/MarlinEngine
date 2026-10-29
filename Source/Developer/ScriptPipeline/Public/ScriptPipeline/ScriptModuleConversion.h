#pragma once

#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "GlobalStoreAssetStore/AssetLocate.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "ScriptPipeline/ScriptCompiler.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
struct ScriptModuleConversionResult
{
    bool Succeeded = false;
    bool CacheHit = false;
    Hash128 Key;
    ConvertedResultLocation Location;
    String Errors;
    /// The source files the result depends on, as virtual paths, so their changes can invalidate it.
    List<String> Files;
};

/// Converts the script module of a package (MarlinScript §11.1) through GlobalStore into a ScriptModuleAsset block.
/// The input key covers the converter version, the source hashes of the compiler modules, the bytecode format, the
/// package, the target, the options and the list of script source files, so adding a file misses; under it a
/// dependency record keeps those files, and the key adds the hashes of their texts as they are now, so a hit needs
/// no compile and an edit to any of them misses. That key leads to the result by its content, so a compile that
/// writes the same module again, as one of the Engine package after an edit under /Project does, gives the same
/// result key, and a loaded module has nothing to reload.
class SCRIPTPIPELINE_API ScriptModuleConversion
{
public:
    /// Bump when the output changes for the same sources without a change to the compiler modules.
    static constexpr uint32 Version = 2;

    explicit ScriptModuleConversion(ConversionStore &store) : _store(store)
    {
    }

    ScriptModuleConversionResult Convert(StringView package, const ConversionTarget &target,
                                         const ScriptCompileOptions &options);

private:
    /// The hash of a source file's text when it had this size and modification time.
    struct KnownFile
    {
        uint64 Size = 0;
        int64 ModifiedTime = 0;
        Hash128 Hash;
    };

    /// The hash of the text of *path* as it is now; false if it cannot be read. Unchanged files are not read again.
    bool HashFile(const String &path, Hash128 &hash);

    ConversionStore &_store;
    Mutex _filesLock;
    HashMap<String, KnownFile> _files;
};
} // namespace Marlin
