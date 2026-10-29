#include "AssetPipeline/AssetIndex.h"

#include "Concurrency/Jobs.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashSet.h"
#include "HAL/PlatformFile.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "String/Ascii.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

MARLIN_DEFINE_LOG_CATEGORY(LogAssetIndex);

namespace Marlin
{
namespace
{
constexpr uint32 LogMagic = 0x324C414Du;

class ByteWriter
{
public:
    explicit ByteWriter(List<uint8> &bytes) : _bytes(&bytes)
    {
    }

    void Raw(const void *data, usize size)
    {
        _bytes->AddRange(Span<const uint8>(static_cast<const uint8 *>(data), size));
    }

    template <typename T> void Value(const T &value)
    {
        Raw(&value, sizeof(T));
    }

    void Text(StringView text)
    {
        Value(static_cast<uint32>(text.Length()));
        Raw(text.Data(), text.Length());
    }

    template <typename T> void Column(const List<T> &column)
    {
        Value(static_cast<uint64>(column.Count()));
        Raw(column.Data(), column.Count() * sizeof(T));
    }

private:
    List<uint8> *_bytes;
};

class ByteReader
{
public:
    ByteReader(const uint8 *data, usize size) : _data(data), _size(size)
    {
    }

    bool Raw(void *target, usize size)
    {
        if (size > _size - _position)
        {
            _failed = true;
            return false;
        }
        MemCopy(target, _data + _position, size);
        _position += size;
        return true;
    }

    template <typename T> bool Value(T &value)
    {
        return Raw(&value, sizeof(T));
    }

    bool Text(String &text)
    {
        uint32 length = 0;
        if (!Value(length) || length > _size - _position)
        {
            _failed = true;
            return false;
        }
        text = StringView(reinterpret_cast<const char *>(_data + _position), length);
        _position += length;
        return true;
    }

    template <typename T> bool Column(List<T> &column)
    {
        uint64 count = 0;
        if (!Value(count) || count > (_size - _position) / (sizeof(T) != 0 ? sizeof(T) : 1))
        {
            _failed = true;
            return false;
        }
        column.Resize(static_cast<usize>(count));
        return Raw(column.Data(), static_cast<usize>(count) * sizeof(T));
    }

    [[nodiscard]] bool Failed() const
    {
        return _failed;
    }

    [[nodiscard]] bool AtEnd() const
    {
        return _position == _size;
    }

private:
    const uint8 *_data;
    usize _size;
    usize _position = 0;
    bool _failed = false;
};

bool ReadWhole(StringView path, List<uint8> &bytes)
{
    const String terminated(path);
    const FileHandle file = PlatformFile::Open(terminated.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    bool ok = PlatformFile::GetSize(file, size);
    if (ok)
    {
        bytes.ResizeUninitialized(static_cast<usize>(size));
        usize read = 0;
        ok = size == 0 || (PlatformFile::Read(file, bytes.Data(), bytes.Count(), read) && read == size);
    }
    PlatformFile::Close(file);
    return ok;
}

/// Matches *text* against *pattern* where '*' is any run; ASCII case-insensitive.
bool MatchPattern(StringView pattern, StringView text)
{
    usize p = 0;
    usize t = 0;
    usize star = StringView::NotFound;
    usize resume = 0;
    while (t < text.Length())
    {
        if (p < pattern.Length() && pattern[p] == '*')
        {
            star = p++;
            resume = t;
        }
        else if (p < pattern.Length() && Ascii::ToLower(pattern[p]) == Ascii::ToLower(text[t]))
        {
            ++p;
            ++t;
        }
        else if (star != StringView::NotFound)
        {
            p = star + 1;
            t = ++resume;
        }
        else
        {
            return false;
        }
    }
    while (p < pattern.Length() && pattern[p] == '*')
    {
        ++p;
    }
    return p == pattern.Length();
}

void WriteRecord(ByteWriter &writer, const AssetFileRecord &record)
{
    writer.Text(record.Path);
    writer.Value(record.ContentHash);
    writer.Value(record.Size);
    writer.Value(record.ModifiedTime);
    writer.Value(static_cast<uint8>(record.IsAsset ? 1 : 0));
    writer.Value(record.BinaryGuid);
    writer.Value(static_cast<uint32>(record.Objects.Count()));
    for (const AssetObjectRecord &object : record.Objects)
    {
        writer.Value(object.Uuid);
        writer.Text(object.Name);
        writer.Text(object.Type);
        writer.Value(object.Line);
        writer.Value(object.Canonical);
        writer.Column(object.Links);
        writer.Column(object.BinaryLinks);
        writer.Value(static_cast<uint32>(object.Tags.Count()));
        for (const String &tag : object.Tags)
        {
            writer.Text(tag);
        }
    }
}

bool ReadRecord(ByteReader &reader, AssetFileRecord &record)
{
    uint8 isAsset = 0;
    uint32 objects = 0;
    if (!reader.Text(record.Path) || !reader.Value(record.ContentHash) || !reader.Value(record.Size) ||
        !reader.Value(record.ModifiedTime) || !reader.Value(isAsset) || !reader.Value(record.BinaryGuid) ||
        !reader.Value(objects))
    {
        return false;
    }
    record.IsAsset = isAsset != 0;
    for (uint32 i = 0; i < objects; ++i)
    {
        AssetObjectRecord &object = record.Objects.AddDefaulted();
        uint32 tags = 0;
        if (!reader.Value(object.Uuid) || !reader.Text(object.Name) || !reader.Text(object.Type) ||
            !reader.Value(object.Line) || !reader.Value(object.Canonical) || !reader.Column(object.Links) ||
            !reader.Column(object.BinaryLinks) || !reader.Value(tags))
        {
            return false;
        }
        for (uint32 j = 0; j < tags; ++j)
        {
            if (!reader.Text(object.Tags.AddDefaulted()))
            {
                return false;
            }
        }
    }
    return true;
}

/// True if *path* is *prefix* or below it; every path is below an empty prefix.
bool IsAtOrBelow(StringView path, StringView prefix)
{
    return prefix.IsEmpty() || path == prefix ||
           (path.Length() > prefix.Length() && path.StartsWith(prefix) && path[prefix.Length()] == '/');
}

/// Adds the counts of *file* of *image* to *counts*, or takes them away.
void CountFile(const AssetDatabaseView &image, uint32 file, AssetIndexCounts &counts, bool subtract)
{
    AssetIndexCounts added;
    added.Files = 1;
    added.BinaryFiles = image.IsAsset(file) ? 0 : 1;
    added.Objects = image.GetObjectCount(file);
    const uint32 first = image.GetObjectStart(file);
    for (uint32 i = 0; i < image.GetObjectCount(file); ++i)
    {
        uint32 start = 0;
        uint32 links = 0;
        uint32 binaries = 0;
        image.GetLinks(first + i, start, links);
        image.GetBinaryLinks(first + i, start, binaries);
        added.Links += links + binaries;
    }
    const auto apply = [subtract](uint64 &total, uint64 value) { total = subtract ? total - value : total + value; };
    apply(counts.Files, added.Files);
    apply(counts.BinaryFiles, added.BinaryFiles);
    apply(counts.Objects, added.Objects);
    apply(counts.Links, added.Links);
}

/// Writes *image* with its checksum to *path* through a temporary file, then maps it.
bool WriteAndMap(Span<const uint8> image, StringView path, MappedRegion &mapping, FileHandle &mapped)
{
    StringBuilder<512> temporary(path);
    temporary.Append(".tmp");
    const FileHandle out = PlatformFile::Open(temporary.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!out.IsValid())
    {
        return false;
    }
    const uint64 checksum = Xxh3Hash64(image.Data(), image.Count());
    bool ok = PlatformFile::Write(out, image.Data(), image.Count()) &&
              PlatformFile::Write(out, &checksum, sizeof(checksum)) && PlatformFile::Flush(out);
    PlatformFile::Close(out);
    const String target(path);
    ok = ok && PlatformFile::Move(temporary.CStr(), target.CStr(), true);
    if (!ok)
    {
        return false;
    }
    const FileHandle file = PlatformFile::Open(target.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid() || !PlatformFile::Map(file, FileAccess::Read, 0, image.Count() + 8, mapping))
    {
        if (file.IsValid())
        {
            PlatformFile::Close(file);
        }
        return false;
    }
    mapping.Size = image.Count();
    mapped = file;
    return true;
}

void FormatHash(const Hash128 &hash, StringWriter &output)
{
    constexpr char Digits[] = "0123456789abcdef";
    for (int32 shift = 60; shift >= 0; shift -= 4)
    {
        output.Append(Digits[(hash.High >> shift) & 15]);
    }
    for (int32 shift = 60; shift >= 0; shift -= 4)
    {
        output.Append(Digits[(hash.Low >> shift) & 15]);
    }
}
} // namespace

void AssetIndexView::Open(Span<const uint8> base, Span<const uint8> delta)
{
    _base = {};
    _delta = {};
    if (!base.IsEmpty())
    {
        (void)_base.Open(base);
    }
    if (!delta.IsEmpty())
    {
        (void)_delta.Open(delta);
    }
    _removed = BitArray<>(_base.FileCount());
    // The base's counts are taken once per base image; a new delta only adjusts them.
    const uint64 baseGeneration = _base.IsOpen() ? _base.GetGeneration() : 0;
    if (base.Data() != _countedBase || baseGeneration != _countedGeneration)
    {
        _baseCounts = {};
        for (uint32 file = 0; file < _base.FileCount(); ++file)
        {
            CountFile(_base, file, _baseCounts, false);
        }
        _countedBase = base.Data();
        _countedGeneration = baseGeneration;
    }
    _counts = _baseCounts;
    const AssetStringPool &removed = _delta.GetRemovedPaths();
    for (uint32 i = 0; i < removed.Count(); ++i)
    {
        const uint32 file = _base.FindFile(removed[i]);
        if (file != AssetDatabaseNone && !_removed[file])
        {
            _removed.Set(file);
            CountFile(_base, file, _counts, true);
        }
    }
    for (uint32 file = 0; file < _delta.FileCount(); ++file)
    {
        CountFile(_delta, file, _counts, false);
    }
}

bool AssetIndexView::Locate(const Guid &uuid, uint32 &object, bool &delta) const
{
    object = _delta.FindObject(uuid);
    if (object != AssetDatabaseNone)
    {
        delta = true;
        return true;
    }
    object = _base.FindObject(uuid);
    delta = false;
    return object != AssetDatabaseNone && !_removed[_base.GetFile(object)];
}

void AssetIndexView::FillInfo(const AssetDatabaseView &image, uint32 object, AssetObjectInfo &info) const
{
    info.Uuid = image.GetUuid(object);
    info.Name = image.GetName(object);
    info.Type = image.GetType(object);
    StringBuilder<512> path;
    image.GetPath(image.GetFile(object), path);
    info.Path = path.AsView();
    info.Line = image.GetLine(object);
}

bool AssetIndexView::GetFileStamp(StringView path, uint64 &size, int64 &modifiedTime, Hash128 &contentHash) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        const uint32 file = image->FindFile(path);
        if (file != AssetDatabaseNone && (image == &_delta || !_removed[file]))
        {
            size = image->GetSize(file);
            modifiedTime = image->GetModifiedTime(file);
            contentHash = image->GetContentHash(file);
            return true;
        }
    }
    return false;
}

bool AssetIndexView::GetRecord(StringView path, AssetFileRecord &record) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        const uint32 file = image->FindFile(path);
        if (file != AssetDatabaseNone && (image == &_delta || !_removed[file]))
        {
            image->ToRecord(file, record);
            return true;
        }
    }
    return false;
}

void AssetIndexView::ForEachFile(FunctionRef<void(StringView path)> visitor) const
{
    ForEachFileBelow({}, visitor);
}

void AssetIndexView::ForEachFileBelow(StringView prefix, FunctionRef<void(StringView path)> visitor) const
{
    StringBuilder<512> path;
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        // Files are sorted by path, so the ones below *prefix* are one run.
        for (uint32 file = prefix.IsEmpty() ? 0 : image->LowerBound(prefix); file < image->FileCount(); ++file)
        {
            path.Clear();
            image->GetPath(file, path);
            if (!path.AsView().StartsWith(prefix))
            {
                break;
            }
            if ((image == &_delta || !_removed[file]) && IsAtOrBelow(path.AsView(), prefix))
            {
                visitor(path.AsView());
            }
        }
    }
}

bool AssetIndexView::Find(const Guid &uuid, AssetObjectInfo &info) const
{
    uint32 object = 0;
    bool delta = false;
    if (!Locate(uuid, object, delta))
    {
        return false;
    }
    FillInfo(delta ? _delta : _base, object, info);
    return true;
}

void AssetIndexView::FindMany(Span<const Guid> uuids, List<AssetObjectInfo> &infos, List<bool> &found) const
{
    infos.Resize(uuids.Count());
    found.Resize(uuids.Count());
    Jobs::ParallelFor(static_cast<uint32>(uuids.Count()), 256, [&](uint32 begin, uint32 end) {
        for (uint32 i = begin; i < end; ++i)
        {
            found[i] = Find(uuids[i], infos[i]);
        }
    });
}

bool AssetIndexView::FindBinary(const Guid &uuid, AssetBinaryInfo &info) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        const uint32 file = image->FindBinary(uuid);
        if (file != AssetDatabaseNone && (image == &_delta || !_removed[file]))
        {
            info.Uuid = uuid;
            StringBuilder<512> path;
            image->GetPath(file, path);
            info.Path = path.AsView();
            StringBuilder<40> hash;
            FormatHash(image->GetContentHash(file), hash);
            info.ContentHash = hash.AsView();
            info.Size = image->GetSize(file);
            info.ModifiedTime = image->GetModifiedTime(file);
            return true;
        }
    }
    return false;
}

bool AssetIndexView::FindBinaryAt(StringView path, AssetBinaryInfo &info) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        const uint32 file = image->FindFile(path);
        if (file != AssetDatabaseNone && (image == &_delta || !_removed[file]))
        {
            if (image->IsAsset(file) || !image->GetBinaryGuid(file).IsValid())
            {
                return false;
            }
            info.Uuid = image->GetBinaryGuid(file);
            info.Path = path;
            StringBuilder<40> hash;
            FormatHash(image->GetContentHash(file), hash);
            info.ContentHash = hash.AsView();
            info.Size = image->GetSize(file);
            info.ModifiedTime = image->GetModifiedTime(file);
            return true;
        }
    }
    return false;
}

bool AssetIndexView::GetCanonicalHash(const Guid &uuid, Hash128 &hash) const
{
    uint32 object = 0;
    bool delta = false;
    if (!Locate(uuid, object, delta))
    {
        return false;
    }
    hash = (delta ? _delta : _base).GetCanonicalHash(object);
    return true;
}

bool AssetIndexView::GetBinaryHash(const Guid &uuid, Hash128 &hash) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        const uint32 file = image->FindBinary(uuid);
        if (file != AssetDatabaseNone && (image == &_delta || !_removed[file]))
        {
            hash = image->GetContentHash(file);
            return true;
        }
    }
    return false;
}

void AssetIndexView::Query(const AssetQuery &query, List<AssetObjectInfo> &results) const
{
    struct Filter
    {
        const AssetDatabaseView *Image = nullptr;
        uint32 Type = AssetDatabaseNone;
        uint32 Tag = AssetDatabaseNone;
        BitArray<> Names;
        BitArray<> Directories;
        bool Empty = false;
    };
    StringView prefix = query.PathPrefix.AsView();
    if (!prefix.IsEmpty() && prefix.Last() == '/')
    {
        prefix = prefix.Substring(0, prefix.Length() - 1);
    }
    const auto prepare = [&](const AssetDatabaseView &image, Filter &filter) {
        filter.Image = &image;
        if (!query.Type.IsEmpty())
        {
            filter.Type = image.GetTypePool().Find(query.Type);
            filter.Empty = filter.Empty || filter.Type == AssetDatabaseNone;
        }
        if (!query.Tag.IsEmpty())
        {
            filter.Tag = image.GetTagPool().Find(query.Tag);
            filter.Empty = filter.Empty || filter.Tag == AssetDatabaseNone;
        }
        if (!query.Name.IsEmpty())
        {
            const AssetStringPool &names = image.GetNamePool();
            filter.Names = BitArray<>(names.Count());
            List<uint8> matched;
            matched.Resize(names.Count());
            Jobs::ParallelFor(names.Count(), 4096, [&](uint32 begin, uint32 end) {
                for (uint32 id = begin; id < end; ++id)
                {
                    matched[id] = MatchPattern(query.Name, names[id]) ? 1 : 0;
                }
            });
            for (uint32 id = 0; id < names.Count(); ++id)
            {
                if (matched[id] != 0)
                {
                    filter.Names.Set(id);
                }
            }
        }
        if (!prefix.IsEmpty())
        {
            const AssetStringPool &directories = image.GetDirectoryPool();
            filter.Directories = BitArray<>(directories.Count());
            for (uint32 id = 0; id < directories.Count(); ++id)
            {
                const StringView directory = directories[id];
                if (directory == prefix || (directory.StartsWith(prefix) && directory[prefix.Length()] == '/'))
                {
                    filter.Directories.Set(id);
                }
            }
        }
    };
    const auto matches = [&](const Filter &filter, uint32 object) {
        const AssetDatabaseView &image = *filter.Image;
        if (filter.Type != AssetDatabaseNone && image.GetTypeId(object) != filter.Type)
        {
            return false;
        }
        const uint32 file = image.GetFile(object);
        if (&image == &_base && _removed[file])
        {
            return false;
        }
        if (!query.Name.IsEmpty() && !filter.Names[image.GetNameId(object)])
        {
            return false;
        }
        if (!prefix.IsEmpty() && !filter.Directories[image.GetDirectoryId(file)])
        {
            return false;
        }
        if (filter.Tag != AssetDatabaseNone)
        {
            uint32 start = 0;
            uint32 count = 0;
            image.GetTags(object, start, count);
            for (uint32 t = 0; t < count; ++t)
            {
                if (image.GetTagId(start + t) == filter.Tag)
                {
                    return true;
                }
            }
            return false;
        }
        return true;
    };
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        if (!image->IsOpen() || results.Count() >= query.Limit)
        {
            continue;
        }
        Filter filter;
        prepare(*image, filter);
        if (filter.Empty)
        {
            continue;
        }
        const uint32 objects = image->ObjectCount();
        const uint32 pages = (objects + PackedColumnPageRows - 1) >> PackedColumnPageShift;
        // Pages are matched a chunk at a time, in order, so a query stops scanning once it has its limit.
        constexpr uint32 ChunkPages = 16;
        List<List<uint32>> hits;
        for (uint32 chunk = 0; chunk < pages && results.Count() < query.Limit; chunk += ChunkPages)
        {
            const uint32 count = pages - chunk < ChunkPages ? pages - chunk : ChunkPages;
            hits.Clear();
            hits.Resize(count);
            Jobs::ParallelFor(count, 1, [&](uint32 begin, uint32 end) {
                for (uint32 index = begin; index < end; ++index)
                {
                    const uint32 first = (chunk + index) << PackedColumnPageShift;
                    const uint32 last = first + PackedColumnPageRows < objects ? first + PackedColumnPageRows : objects;
                    for (uint32 object = first; object < last; ++object)
                    {
                        if (matches(filter, object))
                        {
                            hits[index].Add(object);
                        }
                    }
                }
            });
            for (const List<uint32> &page : hits)
            {
                for (const uint32 object : page)
                {
                    if (results.Count() >= query.Limit)
                    {
                        break;
                    }
                    FillInfo(*image, object, results.AddDefaulted());
                }
            }
        }
    }
}

void AssetIndexView::GetReferences(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const
{
    uint32 object = 0;
    bool delta = false;
    if (!Locate(uuid, object, delta))
    {
        return;
    }
    const AssetDatabaseView &image = delta ? _delta : _base;
    uint32 start = 0;
    uint32 count = 0;
    image.GetLinks(object, start, count);
    for (uint32 i = 0; i < count; ++i)
    {
        objects.Add(image.GetLink(start + i));
    }
    image.GetBinaryLinks(object, start, count);
    for (uint32 i = 0; i < count; ++i)
    {
        binaries.Add(image.GetBinaryLink(start + i));
    }
}

void AssetIndexView::GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results) const
{
    for (const AssetDatabaseView *image : {&_delta, &_base})
    {
        uint32 start = 0;
        uint32 count = 0;
        image->GetReferencers(uuid, start, count);
        for (uint32 i = 0; i < count; ++i)
        {
            const uint32 object = image->GetReferencer(start + i);
            if (image == &_base && _removed[image->GetFile(object)])
            {
                continue;
            }
            FillInfo(*image, object, results.AddDefaulted());
        }
    }
}

void AssetIndexView::GetDependencies(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const
{
    HashSet<Guid> seen;
    HashSet<Guid> seenBinaries;
    List<Guid> stack;
    stack.Add(uuid);
    seen.Add(uuid);
    List<Guid> links;
    List<Guid> binaryLinks;
    while (!stack.IsEmpty())
    {
        const Guid current = stack.Pop();
        links.Clear();
        binaryLinks.Clear();
        GetReferences(current, links, binaryLinks);
        for (const Guid &link : links)
        {
            if (seen.Add(link))
            {
                objects.Add(link);
                stack.Add(link);
            }
        }
        for (const Guid &binary : binaryLinks)
        {
            if (seenBinaries.Add(binary))
            {
                binaries.Add(binary);
            }
        }
    }
}

AssetIndexCounts AssetIndexView::GetCounts() const
{
    return _counts;
}

struct AssetIndex::State
{
    /// The base image: owned bytes, or a mapped snapshot.
    List<uint8> BaseBytes;
    MappedRegion BaseMapping;
    FileHandle BaseFile;
    List<uint8> DeltaBytes;
    AssetIndexView View;
    String SnapshotDirectory;
    String BasePath;
    uint64 Generation = 0;
    /// Files changed since the base, as they are now; a removal is a record with an empty path. Only the writer,
    /// which holds _writeLock, changes it or the base, so the writer reads both without _lock.
    HashMap<String, AssetFileRecord> Changes;
    /// Updates since the last Commit, the same way.
    HashMap<String, AssetFileRecord> Pending;

    State() = default;
    MARLIN_NONCOPYABLE(State);

    ~State()
    {
        ReleaseBase();
    }

    void ReleaseBase()
    {
        if (BaseMapping.Data != nullptr)
        {
            PlatformFile::Unmap(BaseMapping);
            BaseMapping = {};
        }
        if (BaseFile.IsValid())
        {
            PlatformFile::Close(BaseFile);
            BaseFile = {};
        }
        BaseBytes.Clear();
        BasePath.Clear();
    }

    /// Deletes snapshots other than the current one; Windows refuses while another process maps them.
    void DeleteOldSnapshots() const
    {
        if (SnapshotDirectory.IsEmpty())
        {
            return;
        }
        DirectoryIterator iterator;
        DirectoryEntry entry;
        if (!iterator.Open(SnapshotDirectory.CStr()))
        {
            return;
        }
        List<String> old;
        while (iterator.Next(entry))
        {
            const StringView name(entry.Name);
            if (name.StartsWith("Index.") && name.EndsWith(".snapshot"))
            {
                StringBuilder<512> path(SnapshotDirectory.AsView());
                path.Append('/').Append(name);
                if (path.AsView() != BasePath.AsView())
                {
                    old.Add(path.ToString());
                }
            }
        }
        for (const String &path : old)
        {
            (void)PlatformFile::Delete(path.CStr());
        }
    }

    [[nodiscard]] Span<const uint8> Base() const
    {
        return BaseMapping.Data != nullptr
                   ? Span<const uint8>(static_cast<const uint8 *>(BaseMapping.Data), BaseMapping.Size)
                   : BaseBytes.AsSpan();
    }
};

AssetIndex::AssetIndex() : _state(new State())
{
}

AssetIndex::~AssetIndex()
{
    delete _state;
}

const AssetFileRecord *AssetIndex::FindPending(StringView path, bool &removed) const
{
    for (const HashMap<String, AssetFileRecord> *map : {&_state->Pending, &_state->Changes})
    {
        if (const AssetFileRecord *record = map->Find(path))
        {
            removed = record->Path.IsEmpty();
            return record;
        }
    }
    removed = false;
    return nullptr;
}

void AssetIndex::Update(const AssetFileRecord &record)
{
    const ScopeLock lock(_lock);
    _state->Pending.FindOrAdd(record.Path) = record;
}

void AssetIndex::Update(AssetFileRecord &&record)
{
    const ScopeLock lock(_lock);
    AssetFileRecord &target = _state->Pending.FindOrAdd(record.Path);
    target = Move(record);
}

bool AssetIndex::Remove(StringView path)
{
    const ScopeLock lock(_lock);
    bool removed = false;
    const AssetFileRecord *pending = FindPending(path, removed);
    uint64 size = 0;
    int64 modified = 0;
    Hash128 hash;
    const bool known = pending != nullptr ? !removed : _state->View.GetFileStamp(path, size, modified, hash);
    if (!known)
    {
        return false;
    }
    _state->Pending.FindOrAdd(String(path)) = AssetFileRecord();
    return true;
}

bool AssetIndex::MergePendingLocked()
{
    State &state = *_state;
    if (state.Pending.IsEmpty())
    {
        return false;
    }
    for (auto &entry : state.Pending)
    {
        state.Changes.FindOrAdd(entry.Key) = Move(entry.Value);
    }
    state.Pending.Clear();
    return true;
}

bool AssetIndex::NeedsFold() const
{
    const State &state = *_state;
    return !state.Changes.IsEmpty() || !state.View.GetBase().IsOpen() ||
           (!state.SnapshotDirectory.IsEmpty() && state.BasePath.IsEmpty());
}

void AssetIndex::PublishDelta()
{
    // Built outside the lock; readers keep the old images until the swap.
    State &state = *_state;
    List<const AssetFileRecord *> records;
    List<String> removed;
    for (const auto &entry : state.Changes)
    {
        removed.Add(entry.Key);
        if (!entry.Value.Path.IsEmpty())
        {
            records.Add(&entry.Value);
        }
    }
    List<uint8> delta;
    AssetDatabaseBuilder::Build(records.AsSpan(), removed.AsSpan(), state.Generation + 1, delta);
    const ScopeLock lock(_lock);
    state.DeltaBytes = Move(delta);
    ++state.Generation;
    state.View.Open(state.Base(), state.DeltaBytes.AsSpan());
}

void AssetIndex::Fold()
{
    // The new base is merged from the immutable old one and the changes, then written and mapped, before the lock
    // is taken: readers only wait for the swap.
    State &state = *_state;
    List<String> replaced;
    List<const AssetFileRecord *> records;
    for (const auto &entry : state.Changes)
    {
        replaced.Add(entry.Key);
        if (!entry.Value.Path.IsEmpty())
        {
            records.Add(&entry.Value);
        }
    }
    const uint64 generation = state.Generation + 1;
    List<uint8> image;
    AssetDatabaseBuilder::Merge(state.View.GetBase(), replaced.AsSpan(), records.AsSpan(), generation, image);
    StringBuilder<512> snapshot;
    MappedRegion mapping;
    FileHandle file;
    bool mapped = false;
    if (!state.SnapshotDirectory.IsEmpty())
    {
        char digits[24];
        snapshot.Append(state.SnapshotDirectory.AsView()).Append("/Index.");
        snapshot.Append(StringView(digits, FormatUnsigned(generation, digits))).Append(".snapshot");
        mapped = WriteAndMap(image.AsSpan(), snapshot.AsView(), mapping, file);
    }
    // Opening the view counts the new base, so that happens before the lock too; moving the bytes keeps them put.
    AssetIndexView view;
    view.Open(mapped ? Span<const uint8>(static_cast<const uint8 *>(mapping.Data), mapping.Size) : image.AsSpan(), {});
    {
        const ScopeLock lock(_lock);
        state.ReleaseBase();
        if (mapped)
        {
            state.BaseMapping = mapping;
            state.BaseFile = file;
            state.BasePath = snapshot.AsView();
        }
        else
        {
            state.BaseBytes = Move(image);
        }
        state.Changes.Clear();
        state.DeltaBytes.Clear();
        state.Generation = generation;
        state.View = Move(view);
    }
    state.DeleteOldSnapshots();
}

uint64 AssetIndex::Commit()
{
    const ScopeLock writer(_writeLock);
    {
        const ScopeLock lock(_lock);
        if (!MergePendingLocked())
        {
            return _state->Generation;
        }
    }
    if (_state->Changes.Count() > FoldThreshold)
    {
        Fold();
    }
    else
    {
        PublishDelta();
    }
    return _state->Generation;
}

void AssetIndex::Clear()
{
    const ScopeLock writer(_writeLock);
    const ScopeLock lock(_lock);
    const String directory = _state->SnapshotDirectory;
    delete _state;
    _state = new State();
    _state->SnapshotDirectory = directory;
}

bool AssetIndex::GetFileStamp(StringView path, uint64 &size, int64 &modifiedTime, Hash128 &contentHash) const
{
    const SharedScopeLock lock(_lock);
    bool removed = false;
    if (const AssetFileRecord *record = FindPending(path, removed))
    {
        size = record->Size;
        modifiedTime = record->ModifiedTime;
        contentHash = record->ContentHash;
        return !removed;
    }
    return _state->View.GetFileStamp(path, size, modifiedTime, contentHash);
}

bool AssetIndex::GetRecord(StringView path, AssetFileRecord &record) const
{
    const SharedScopeLock lock(_lock);
    bool removed = false;
    if (const AssetFileRecord *pending = FindPending(path, removed))
    {
        if (!removed)
        {
            record = *pending;
        }
        return !removed;
    }
    return _state->View.GetRecord(path, record);
}

void AssetIndex::ForEachFile(FunctionRef<void(StringView path)> visitor) const
{
    ForEachFileBelow({}, visitor);
}

void AssetIndex::ForEachFileBelow(StringView prefix, FunctionRef<void(StringView path)> visitor) const
{
    List<String> paths;
    {
        const SharedScopeLock lock(_lock);
        const State &state = *_state;
        for (const auto &entry : state.Pending)
        {
            if (!entry.Value.Path.IsEmpty() && IsAtOrBelow(entry.Key.AsView(), prefix))
            {
                paths.Add(entry.Key);
            }
        }
        for (const auto &entry : state.Changes)
        {
            if (!entry.Value.Path.IsEmpty() && IsAtOrBelow(entry.Key.AsView(), prefix) &&
                !state.Pending.Contains(entry.Key))
            {
                paths.Add(entry.Key);
            }
        }
        state.View.ForEachFileBelow(prefix, [&](StringView path) {
            if (!state.Pending.Contains(path) && !state.Changes.Contains(path))
            {
                paths.Add(String(path));
            }
        });
    }
    for (const String &path : paths)
    {
        visitor(path.AsView());
    }
}

bool AssetIndex::Find(const Guid &uuid, AssetObjectInfo &info) const
{
    const SharedScopeLock lock(_lock);
    return _state->View.Find(uuid, info);
}

bool AssetIndex::FindBinary(const Guid &uuid, AssetBinaryInfo &info) const
{
    const SharedScopeLock lock(_lock);
    return _state->View.FindBinary(uuid, info);
}

bool AssetIndex::FindBinaryAt(StringView path, AssetBinaryInfo &info) const
{
    const SharedScopeLock lock(_lock);
    return _state->View.FindBinaryAt(path, info);
}

void AssetIndex::FindMany(Span<const Guid> uuids, List<AssetObjectInfo> &infos, List<bool> &found) const
{
    const SharedScopeLock lock(_lock);
    _state->View.FindMany(uuids, infos, found);
}

bool AssetIndex::GetCanonicalHash(const Guid &uuid, Hash128 &hash) const
{
    const SharedScopeLock lock(_lock);
    return _state->View.GetCanonicalHash(uuid, hash);
}

bool AssetIndex::GetBinaryHash(const Guid &uuid, Hash128 &hash) const
{
    const SharedScopeLock lock(_lock);
    return _state->View.GetBinaryHash(uuid, hash);
}

void AssetIndex::Query(const AssetQuery &query, List<AssetObjectInfo> &results) const
{
    const SharedScopeLock lock(_lock);
    _state->View.Query(query, results);
}

void AssetIndex::GetReferences(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const
{
    const SharedScopeLock lock(_lock);
    _state->View.GetReferences(uuid, objects, binaries);
}

void AssetIndex::GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results) const
{
    const SharedScopeLock lock(_lock);
    _state->View.GetReferencers(uuid, results);
}

void AssetIndex::GetDependencies(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const
{
    const SharedScopeLock lock(_lock);
    _state->View.GetDependencies(uuid, objects, binaries);
}

AssetIndexCounts AssetIndex::GetCounts() const
{
    const SharedScopeLock lock(_lock);
    return _state->View.GetCounts();
}

uint64 AssetIndex::GetGeneration() const
{
    const SharedScopeLock lock(_lock);
    return _state->Generation;
}

void AssetIndex::WithImages(
    FunctionRef<void(Span<const uint8> base, Span<const uint8> delta, uint64 generation, StringView basePath)> visitor)
    const
{
    const SharedScopeLock lock(_lock);
    visitor(_state->Base(), _state->DeltaBytes.AsSpan(), _state->Generation, _state->BasePath.AsView());
}

void AssetIndex::SetSnapshotDirectory(StringView directory)
{
    const ScopeLock writer(_writeLock);
    const ScopeLock lock(_lock);
    _state->SnapshotDirectory = directory;
}

bool AssetIndex::LoadLatest()
{
    String directory;
    {
        const SharedScopeLock lock(_lock);
        directory = _state->SnapshotDirectory;
    }
    DirectoryIterator iterator;
    DirectoryEntry entry;
    if (directory.IsEmpty() || !iterator.Open(directory.CStr()))
    {
        return false;
    }
    List<uint64> generations;
    while (iterator.Next(entry))
    {
        StringView name(entry.Name);
        uint64 generation = 0;
        if (name.StartsWith("Index.") && name.EndsWith(".snapshot") &&
            TryParse(name.Substring(6, name.Length() - 6 - 9), generation))
        {
            generations.Add(generation);
        }
    }
    generations.Sort([](uint64 a, uint64 b) { return a > b; });
    for (const uint64 generation : generations)
    {
        char digits[24];
        StringBuilder<512> path(directory.AsView());
        path.Append("/Index.").Append(StringView(digits, FormatUnsigned(generation, digits))).Append(".snapshot");
        if (Load(path.AsView()))
        {
            return true;
        }
    }
    return false;
}

String AssetIndex::GetBasePath() const
{
    const SharedScopeLock lock(_lock);
    return _state->BasePath;
}

void AssetIndex::Compact()
{
    const ScopeLock writer(_writeLock);
    {
        const ScopeLock lock(_lock);
        (void)MergePendingLocked();
    }
    // Nothing changed since a base that is already in its snapshot: there is nothing to write.
    if (NeedsFold())
    {
        Fold();
    }
}

bool AssetIndex::Save(StringView path)
{
    const ScopeLock writer(_writeLock);
    {
        const ScopeLock lock(_lock);
        (void)MergePendingLocked();
    }
    if (NeedsFold())
    {
        Fold();
    }
    // Only the writer replaces the base, so it stays put without the read lock.
    const Span<const uint8> image = _state->Base();
    StringBuilder<512> temporary(path);
    temporary.Append(".tmp");
    const FileHandle file = PlatformFile::Open(temporary.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const uint64 checksum = Xxh3Hash64(image.Data(), image.Count());
    const bool written = PlatformFile::Write(file, image.Data(), image.Count()) &&
                         PlatformFile::Write(file, &checksum, sizeof(checksum)) && PlatformFile::Flush(file);
    PlatformFile::Close(file);
    return written && PlatformFile::Move(temporary.CStr(), String(path).CStr(), true);
}

bool AssetIndex::Load(StringView path)
{
    const String terminated(path);
    const FileHandle file = PlatformFile::Open(terminated.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    MappedRegion mapping;
    if (!PlatformFile::GetSize(file, size) || size < 16 ||
        !PlatformFile::Map(file, FileAccess::Read, 0, static_cast<usize>(size), mapping))
    {
        PlatformFile::Close(file);
        return false;
    }
    const auto *bytes = static_cast<const uint8 *>(mapping.Data);
    const usize imageSize = static_cast<usize>(size) - 8;
    uint64 checksum = 0;
    MemCopy(&checksum, bytes + imageSize, 8);
    AssetDatabaseView check;
    if (checksum != Xxh3Hash64(bytes, imageSize) || !check.Open(Span<const uint8>(bytes, imageSize)))
    {
        PlatformFile::Unmap(mapping);
        PlatformFile::Close(file);
        return false;
    }
    AssetIndexView view;
    view.Open(Span<const uint8>(bytes, imageSize), {});
    const ScopeLock writer(_writeLock);
    const ScopeLock lock(_lock);
    const String directory = _state->SnapshotDirectory;
    delete _state;
    _state = new State();
    _state->SnapshotDirectory = directory;
    _state->BaseMapping = mapping;
    _state->BaseFile = file;
    _state->BasePath = terminated;
    _state->Generation = check.GetGeneration();
    _state->View = Move(view);
    return true;
}

namespace
{
void AppendLogEntry(List<uint8> &entries, StringView path, const AssetFileRecord *record)
{
    List<uint8> payload;
    ByteWriter writer(payload);
    writer.Value(static_cast<uint8>(record != nullptr ? 1 : 0));
    if (record != nullptr)
    {
        WriteRecord(writer, *record);
    }
    else
    {
        writer.Text(path);
    }
    ByteWriter framing(entries);
    framing.Value(LogMagic);
    framing.Value(static_cast<uint32>(payload.Count()));
    framing.Raw(payload.Data(), payload.Count());
    framing.Value(Xxh3Hash64(payload.Data(), payload.Count()));
}
} // namespace

bool AssetIndex::AppendLog(StringView logPath, StringView path, const AssetFileRecord *record)
{
    List<uint8> entry;
    AppendLogEntry(entry, path, record);
    return AppendLog(logPath, entry.AsSpan());
}

bool AssetIndex::AppendLog(StringView logPath, Span<const uint8> entries)
{
    if (entries.IsEmpty())
    {
        return true;
    }
    const String terminated(logPath);
    const FileHandle file =
        PlatformFile::Open(terminated.CStr(), FileAccess::Write, FileMode::OpenOrCreate, FileFlags::Append);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, entries.Data(), entries.Count());
    PlatformFile::Close(file);
    return ok;
}

void AssetIndex::WriteLogEntries(Span<const String> paths, List<uint8> &entries) const
{
    const SharedScopeLock lock(_lock);
    AssetFileRecord record;
    for (const String &path : paths)
    {
        bool removed = false;
        const AssetFileRecord *current = FindPending(path.AsView(), removed);
        if (current == nullptr)
        {
            current = _state->View.GetRecord(path.AsView(), record) ? &record : nullptr;
        }
        AppendLogEntry(entries, path.AsView(), removed ? nullptr : current);
    }
}

bool AssetIndex::ReplayLog(StringView logPath)
{
    List<uint8> bytes;
    if (!ReadWhole(logPath, bytes))
    {
        return false;
    }
    usize position = 0;
    while (bytes.Count() - position >= 16)
    {
        uint32 magic = 0;
        uint32 size = 0;
        MemCopy(&magic, bytes.Data() + position, 4);
        MemCopy(&size, bytes.Data() + position + 4, 4);
        if (magic != LogMagic || bytes.Count() - position - 8 < uint64{size} + 8)
        {
            break;
        }
        const uint8 *payload = bytes.Data() + position + 8;
        uint64 checksum = 0;
        MemCopy(&checksum, payload + size, 8);
        if (checksum != Xxh3Hash64(payload, size))
        {
            break;
        }
        ByteReader reader(payload, size);
        uint8 kind = 0;
        if (!reader.Value(kind))
        {
            break;
        }
        if (kind == 1)
        {
            AssetFileRecord record;
            if (!ReadRecord(reader, record))
            {
                break;
            }
            Update(record);
        }
        else
        {
            String path;
            if (!reader.Text(path))
            {
                break;
            }
            (void)Remove(path.AsView());
        }
        position += 8 + size + 8;
    }
    (void)Commit();
    return true;
}
} // namespace Marlin
