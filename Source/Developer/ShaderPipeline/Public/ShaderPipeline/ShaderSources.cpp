#include "ShaderPipeline/ShaderSources.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashSet.h"
#include "Hash/Xxh3.h"
#include "LanguageFrontend/NameDependencies.h"
#include "LanguageFrontend/Source.h"
#include "Memory/MemoryOps.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
constexpr uint32 NameRecordMagic = 0x4E4D5253; // 'SRMN'

void Append(List<uint8> &bytes, const void *data, usize size)
{
    bytes.AddRange(Span<const uint8>(static_cast<const uint8 *>(data), size));
}

void AppendHashes(List<uint8> &bytes, const List<uint64> &hashes)
{
    const auto count = static_cast<uint32>(hashes.Count());
    Append(bytes, &count, sizeof(count));
    Append(bytes, hashes.Data(), hashes.Count() * sizeof(uint64));
}

struct Reader
{
    Span<const uint8> Bytes;
    usize Position = 0;

    bool Take(void *target, usize size)
    {
        if (size > Bytes.Count() - Position)
        {
            return false;
        }
        MemCopy(target, Bytes.Data() + Position, size);
        Position += size;
        return true;
    }

    bool TakeHashes(List<uint64> &hashes)
    {
        uint32 count = 0;
        if (!Take(&count, sizeof(count)) || count > (Bytes.Count() - Position) / sizeof(uint64))
        {
            return false;
        }
        hashes.Resize(count);
        return Take(hashes.Data(), count * sizeof(uint64));
    }
};
} // namespace

struct ShaderSourceTracker::Declared
{
    LanguageDeclaredNames Names;
};

void WriteShaderNameRecord(const ShaderNameRecord &record, List<uint8> &bytes)
{
    Append(bytes, &NameRecordMagic, sizeof(NameRecordMagic));
    const uint8 extensions = record.Names.Extensions ? 1 : 0;
    Append(bytes, &extensions, 1);
    AppendHashes(bytes, record.Names.Names);
    AppendHashes(bytes, record.Names.NamespaceNames);
    const auto count = static_cast<uint32>(record.SourceSet.Count());
    Append(bytes, &count, sizeof(count));
    for (const ShaderSourceDependency &file : record.SourceSet)
    {
        Append(bytes, &file.Hash, sizeof(Hash128));
        const auto length = static_cast<uint32>(file.Path.Length());
        Append(bytes, &length, sizeof(length));
        Append(bytes, file.Path.Data(), length);
    }
}

bool ReadShaderNameRecord(Span<const uint8> bytes, ShaderNameRecord &record)
{
    record = {};
    Reader reader{bytes};
    uint32 magic = 0;
    uint8 extensions = 0;
    uint32 count = 0;
    if (!reader.Take(&magic, sizeof(magic)) || magic != NameRecordMagic || !reader.Take(&extensions, 1) ||
        !reader.TakeHashes(record.Names.Names) || !reader.TakeHashes(record.Names.NamespaceNames) ||
        !reader.Take(&count, sizeof(count)))
    {
        return false;
    }
    record.Names.Extensions = extensions != 0;
    for (uint32 i = 0; i < count; ++i)
    {
        ShaderSourceDependency &file = record.SourceSet.AddDefaulted();
        uint32 length = 0;
        if (!reader.Take(&file.Hash, sizeof(Hash128)) || !reader.Take(&length, sizeof(length)) ||
            length > bytes.Count() - reader.Position)
        {
            return false;
        }
        file.Path = StringView(reinterpret_cast<const char *>(bytes.Data() + reader.Position), length);
        reader.Position += length;
    }
    return true;
}

ShaderSourceTracker::ShaderSourceTracker() = default;
ShaderSourceTracker::~ShaderSourceTracker() = default;

bool ShaderSourceTracker::HashFile(StringView path, Hash128 &hash)
{
    return HashFile(path, hash, nullptr);
}

bool ShaderSourceTracker::HashFile(StringView path, Hash128 &hash, String *text)
{
    const String key(path);
    FileInfo info;
    const bool known = Vfs::GetInfo(path, info) && !info.IsDirectory && info.ModifiedTime != 0;
    if (known && text == nullptr)
    {
        const ScopeLock lock(_lock);
        if (const KnownFile *file = _files.Find(key);
            file != nullptr && file->Size == info.Size && file->ModifiedTime == info.ModifiedTime)
        {
            hash = file->Hash;
            return true;
        }
    }
    String read;
    if (!Vfs::ReadAll(path, read))
    {
        return false;
    }
    hash = Xxh3Hash128(read.Data(), read.Length());
    if (known)
    {
        // The time is from before the read, so a change during it is seen next time.
        const ScopeLock lock(_lock);
        _files.Set(key, {info.Size, info.ModifiedTime, hash});
    }
    if (text != nullptr)
    {
        *text = Move(read);
    }
    return true;
}

Hash128 ShaderSourceTracker::HashSourceSet(Span<const ShaderSourceDependency> files)
{
    List<uint8> bytes;
    for (const ShaderSourceDependency &file : files)
    {
        Append(bytes, file.Path.Data(), file.Path.Length());
        Append(bytes, "\n", 1);
        Append(bytes, &file.Hash, sizeof(Hash128));
    }
    return Xxh3Hash128(bytes.Data(), bytes.Count());
}

bool ShaderSourceTracker::GetSourceSet(List<ShaderSourceDependency> &files, Hash128 &hash)
{
    files.Clear();
    List<String> roots;
    GetShaderSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    List<String> paths;
    sources.ListFiles([&](StringView path) {
        LanguageDialect dialect;
        LanguageDialect folder;
        // The files a shader compile loads (Sema::Load); one in the wrong folder fails every compile anyway.
        if (GetDialectOf(path, dialect) && IsInSourceSet(LanguageDialect::Shader, dialect) &&
            GetFolderDialectOf(path, folder) && folder == dialect)
        {
            paths.Add(String(path));
        }
    });
    paths.Sort([](const String &a, const String &b) { return a.AsView().CompareTo(b.AsView()) < 0; });
    for (String &path : paths)
    {
        Hash128 fileHash;
        if (!HashFile(path.AsView(), fileHash))
        {
            return false;
        }
        files.Add({Move(path), fileHash});
    }
    hash = HashSourceSet(files.AsSpan());
    return true;
}

bool ShaderSourceTracker::KeepsNames(const ShaderNameRecord &record, Span<const ShaderSourceDependency> used,
                                     Span<const ShaderSourceDependency> current)
{
    LanguageNameDependencies names;
    names.Names = record.Names.Names;
    names.NamespaceNames = record.Names.NamespaceNames;
    names.Extensions = record.Names.Extensions;

    HashSet<StringView> usedPaths;
    for (const ShaderSourceDependency &file : used)
    {
        usedPaths.Add(file.Path.AsView());
    }
    HashMap<StringView, Hash128> recorded;
    for (const ShaderSourceDependency &file : record.SourceSet)
    {
        recorded.Set(file.Path.AsView(), file.Hash);
    }
    for (const ShaderSourceDependency &file : current)
    {
        const StringView path = file.Path.AsView();
        Hash128 hash = file.Hash;
        if (usedPaths.Contains(path))
        {
            continue;
        }
        if (const Hash128 *was = recorded.Find(path); was != nullptr && *was == hash)
        {
            continue;
        }
        const Declared *declared = nullptr;
        {
            const ScopeLock lock(_lock);
            if (const UniquePtr<Declared> *found = _declared.Find(hash))
            {
                declared = found->Get();
            }
        }
        if (declared == nullptr)
        {
            String text;
            if (!HashFile(path, hash, &text))
            {
                return false;
            }
            auto parsed = MakeUnique<Declared>();
            GetDeclaredNames(path, Move(text), parsed->Names);
            const ScopeLock lock(_lock);
            UniquePtr<Declared> &slot = _declared.FindOrAdd(hash);
            if (!slot)
            {
                slot = Move(parsed);
            }
            declared = slot.Get();
        }
        if (AffectsNames(declared->Names, names))
        {
            return false;
        }
    }
    return true;
}
} // namespace Marlin
