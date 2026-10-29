#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "Memory/UniquePtr.h"
#include "ShaderPipeline/ShaderCompiler.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// What a compile depended on besides the files it used, whose hashes go into its result key: the names it looked
/// up, and every file of the source set as it was. A file the compile did not use only invalidates the result when
/// it is new or changed and now declares one of those names (LanguageNameDependencies), so adding a file that
/// shadows a name the compile found further out misses, and editing an unrelated one does not.
struct ShaderNameRecord
{
    ShaderNameDependencies Names;
    List<ShaderSourceDependency> SourceSet;
};

SHADERPIPELINE_API void WriteShaderNameRecord(const ShaderNameRecord &record, List<uint8> &bytes);
[[nodiscard]] SHADERPIPELINE_API bool ReadShaderNameRecord(Span<const uint8> bytes, ShaderNameRecord &record);

/// Hashes the shader source set and checks name records against it, remembering what it read: a file whose size and
/// modification time did not change is not read again, and the names a file declares are parsed once per text.
/// Safe to share between threads.
class SHADERPIPELINE_API ShaderSourceTracker
{
public:
    ShaderSourceTracker();
    ~ShaderSourceTracker();
    MARLIN_NONCOPYABLE(ShaderSourceTracker);

    /// The hash of the text of *path* as it is now; false if it cannot be read.
    bool HashFile(StringView path, Hash128 &hash);

    /// Every file of the shader source set as it is now, sorted by path, and HashSourceSet of them; false if one
    /// cannot be read.
    bool GetSourceSet(List<ShaderSourceDependency> &files, Hash128 &hash);

    /// Whether the source set *current* (GetSourceSet), apart from the files in *used* (which the caller checks by
    /// hash), still gives what *record* found: every new or changed file declares none of its names. Deleted files
    /// cannot change a lookup the compile did not resolve to them.
    bool KeepsNames(const ShaderNameRecord &record, Span<const ShaderSourceDependency> used,
                    Span<const ShaderSourceDependency> current);

    /// The hash of a source set sorted by path: each file's path and the hash of its text.
    [[nodiscard]] static Hash128 HashSourceSet(Span<const ShaderSourceDependency> files);

private:
    struct KnownFile
    {
        uint64 Size = 0;
        int64 ModifiedTime = 0;
        Hash128 Hash;
    };

    /// HashFile, also giving the text when it had to be read.
    bool HashFile(StringView path, Hash128 &hash, String *text);

    /// The names a text declares (LanguageDeclaredNames).
    struct Declared;

    Mutex _lock;
    HashMap<String, KnownFile> _files;
    /// By the hash of the text.
    HashMap<Hash128, UniquePtr<Declared>> _declared;
};
} // namespace Marlin
