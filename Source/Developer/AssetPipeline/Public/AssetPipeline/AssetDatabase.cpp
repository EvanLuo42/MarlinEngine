#include "AssetPipeline/AssetDatabase.h"

#include "AssetPipeline/AssetIndex.h"
#include "Containers/BitArray.h"
#include "Containers/HashMap.h"
#include "Hash/Xxh3.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
enum Section : uint32
{
    DirectoryPool,
    FileNamePool,
    ExtensionPool,
    NamePool,
    TypePool,
    TagPool,
    RemovedPool,
    FileDirectory,
    FileName,
    FileExtension,
    FileHashHigh,
    FileHashLow,
    FileSize,
    FileModified,
    FileObjectStart,
    FileObjectCount,
    FileBinaryHigh,
    FileBinaryLow,
    FileKind,
    ObjectHigh,
    ObjectLow,
    ObjectName,
    ObjectType,
    ObjectFile,
    ObjectLine,
    ObjectLinkStart,
    ObjectLinkCount,
    ObjectBinaryStart,
    ObjectBinaryCount,
    ObjectTagStart,
    ObjectTagCount,
    ObjectCanonicalHigh,
    ObjectCanonicalLow,
    LinkHigh,
    LinkLow,
    BinaryHigh,
    BinaryLow,
    Tags,
    ReverseKeyHigh,
    ReverseKeyLow,
    ReverseStart,
    ReverseCount,
    ReverseSource,
    PathTable,
    ObjectTable,
    BinaryTable,
    ReverseTable,
    SectionCount,
};

struct Header
{
    uint32 Magic;
    uint32 Version;
    uint64 Generation;
    uint32 Sections;
    uint32 Padding;
};

struct SectionEntry
{
    uint64 Offset;
    uint64 Size;
};

struct PoolHeader
{
    uint32 TableCapacity;
    uint32 Padding;
    uint64 OffsetsSize;
    uint64 CharsSize;
};

uint64 HashPath(StringView text)
{
    return Xxh3Hash64(text.Data(), text.Length());
}

uint64 HashGuid(uint64 high, uint64 low)
{
    uint64 hash = high * 0x9E3779B185EBCA87ull ^ low;
    hash ^= hash >> 29;
    hash *= 0xBF58476D1CE4E5B9ull;
    return hash ^ (hash >> 32);
}

uint64 High(const Guid &guid)
{
    return (static_cast<uint64>(guid.A) << 32) | guid.B;
}

uint64 Low(const Guid &guid)
{
    return (static_cast<uint64>(guid.C) << 32) | guid.D;
}

uint32 Capacity(usize count)
{
    uint32 capacity = 16;
    while (capacity < count * 2)
    {
        capacity <<= 1;
    }
    return capacity;
}

/// Splits /Project/Content/Rock.masset into /Project/Content, Rock and .masset.
void SplitPath(StringView path, StringView &directory, StringView &name, StringView &extension)
{
    StringView file = path;
    if (!path.SplitLast('/', directory, file))
    {
        directory = {};
    }
    const usize dot = file.LastIndexOf('.');
    if (dot == StringView::NotFound || dot == 0)
    {
        name = file;
        extension = {};
        return;
    }
    name = file.Substring(0, dot);
    extension = file.Substring(dot);
}

class PoolBuilder
{
public:
    PoolBuilder()
    {
        _offsets.Add(0);
        _table.Resize(1024);
    }

    uint32 Add(StringView text)
    {
        const uint64 hash = HashPath(text);
        auto mask = static_cast<uint32>(_table.Count() - 1);
        uint32 slot = static_cast<uint32>(hash) & mask;
        while (_table[slot] != 0)
        {
            const uint32 id = _table[slot] - 1;
            if (_hashes[id] == hash && Get(id) == text)
            {
                return id;
            }
            slot = (slot + 1) & mask;
        }
        const auto id = static_cast<uint32>(_hashes.Count());
        _chars.AddRange(Span<const char>(text.Data(), text.Length()));
        _offsets.Add(_chars.Count());
        _hashes.Add(hash);
        _table[slot] = id + 1;
        if (_hashes.Count() * 2 > _table.Count())
        {
            Rehash(_table.Count() * 2);
        }
        return id;
    }

    void Write(List<uint8> &output) const
    {
        const uint32 capacity = Capacity(_hashes.Count());
        List<uint32> table;
        table.Resize(capacity);
        for (usize i = 0; i < _hashes.Count(); ++i)
        {
            uint32 slot = static_cast<uint32>(_hashes[i]) & (capacity - 1);
            while (table[slot] != 0)
            {
                slot = (slot + 1) & (capacity - 1);
            }
            table[slot] = static_cast<uint32>(i + 1);
        }
        List<uint8> column;
        PackedColumnWriter::Write(_offsets.AsSpan(), column);
        PoolHeader header{capacity, 0, column.Count(), _chars.Count()};
        const usize start = output.Count();
        output.Resize(start + sizeof(header) + capacity * sizeof(uint32));
        MemCopy(output.Data() + start, &header, sizeof(header));
        MemCopy(output.Data() + start + sizeof(header), table.Data(), capacity * sizeof(uint32));
        output.AddRange(column.AsSpan());
        output.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(_chars.Data()), _chars.Count()));
    }

private:
    [[nodiscard]] StringView Get(uint32 id) const
    {
        const auto begin = static_cast<usize>(_offsets[id]);
        return {_chars.Data() + begin, static_cast<usize>(_offsets[id + 1]) - begin};
    }

    void Rehash(usize capacity)
    {
        _table.Clear();
        _table.Resize(capacity);
        const auto mask = static_cast<uint32>(capacity - 1);
        for (usize i = 0; i < _hashes.Count(); ++i)
        {
            uint32 slot = static_cast<uint32>(_hashes[i]) & mask;
            while (_table[slot] != 0)
            {
                slot = (slot + 1) & mask;
            }
            _table[slot] = static_cast<uint32>(i + 1);
        }
    }

    List<char> _chars;
    List<uint64> _offsets;
    List<uint64> _hashes;
    List<uint32> _table;
};

void WriteTable(List<uint32> &table, List<uint8> &output)
{
    output.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(table.Data()), table.Count() * sizeof(uint32)));
}

void InsertRow(List<uint32> &table, uint64 hash, uint32 row)
{
    const auto mask = static_cast<uint32>(table.Count() - 1);
    uint32 slot = static_cast<uint32>(hash) & mask;
    while (table[slot] != 0)
    {
        slot = (slot + 1) & mask;
    }
    table[slot] = row + 1;
}

/// Maps the string ids of a source image's pool to ids of a PoolBuilder, adding each string once.
class PoolRemap
{
public:
    PoolRemap(const AssetStringPool &source, PoolBuilder &target) : _source(&source), _target(&target)
    {
        _ids.Resize(source.Count(), AssetDatabaseNone);
    }

    uint32 operator()(uint32 id)
    {
        if (_ids[id] == AssetDatabaseNone)
        {
            _ids[id] = _target->Add((*_source)[id]);
        }
        return _ids[id];
    }

private:
    const AssetStringPool *_source;
    PoolBuilder *_target;
    List<uint32> _ids;
};

/// Builds an image file by file, in path order: from records, or by copying rows of another image column by column.
class ImageBuilder
{
public:
    void AddRecord(const AssetFileRecord &file)
    {
        StringView directory;
        StringView name;
        StringView extension;
        SplitPath(file.Path.AsView(), directory, name, extension);
        AddFileRow(_directories.Add(directory), _fileNames.Add(name), _extensions.Add(extension), file.ContentHash,
                   file.Size, file.ModifiedTime, file.Objects.Count(), file.BinaryGuid, file.IsAsset);
        InsertPath(HashPath(file.Path.AsView()));
        for (const AssetObjectRecord &object : file.Objects)
        {
            const auto row = static_cast<uint32>(_columns[ObjectHigh].Count());
            AddObjectRow(object.Uuid, _names.Add(object.Name.AsView()), _types.Add(object.Type.AsView()), object.Line,
                         object.Canonical);
            _columns[ObjectLinkStart].Add(_columns[LinkHigh].Count());
            _columns[ObjectLinkCount].Add(object.Links.Count());
            for (const Guid &link : object.Links)
            {
                AddLink(LinkHigh, LinkLow, High(link), Low(link), row);
            }
            _columns[ObjectBinaryStart].Add(_columns[BinaryHigh].Count());
            _columns[ObjectBinaryCount].Add(object.BinaryLinks.Count());
            for (const Guid &link : object.BinaryLinks)
            {
                AddLink(BinaryHigh, BinaryLow, High(link), Low(link), row);
            }
            _columns[ObjectTagStart].Add(_columns[Tags].Count());
            _columns[ObjectTagCount].Add(object.Tags.Count());
            for (const String &tag : object.Tags)
            {
                _columns[Tags].Add(_tagPool.Add(tag.AsView()));
            }
        }
    }

    /// Copies *file* of *source*, whose pools *remaps* maps into this builder's; *path* is its path.
    void AddImageFile(const AssetDatabaseView &source, uint32 file, StringView path, PoolRemap &directories,
                      PoolRemap &fileNames, PoolRemap &extensions, PoolRemap &names, PoolRemap &types, PoolRemap &tags)
    {
        const uint32 objectStart = source.GetObjectStart(file);
        const uint32 objectCount = source.GetObjectCount(file);
        AddFileRow(directories(source.GetDirectoryId(file)), fileNames(source.GetFileNameId(file)),
                   extensions(source.GetExtensionId(file)), source.GetContentHash(file), source.GetSize(file),
                   source.GetModifiedTime(file), objectCount, source.GetBinaryGuid(file), source.IsAsset(file));
        InsertPath(HashPath(path));
        for (uint32 i = 0; i < objectCount; ++i)
        {
            const uint32 object = objectStart + i;
            const auto row = static_cast<uint32>(_columns[ObjectHigh].Count());
            AddObjectRow(source.GetUuid(object), names(source.GetNameId(object)), types(source.GetTypeId(object)),
                         source.GetLine(object), source.GetCanonicalHash(object));
            uint32 start = 0;
            uint32 count = 0;
            source.GetLinks(object, start, count);
            _columns[ObjectLinkStart].Add(_columns[LinkHigh].Count());
            _columns[ObjectLinkCount].Add(count);
            for (uint32 l = 0; l < count; ++l)
            {
                const Guid link = source.GetLink(start + l);
                AddLink(LinkHigh, LinkLow, High(link), Low(link), row);
            }
            source.GetBinaryLinks(object, start, count);
            _columns[ObjectBinaryStart].Add(_columns[BinaryHigh].Count());
            _columns[ObjectBinaryCount].Add(count);
            for (uint32 l = 0; l < count; ++l)
            {
                const Guid link = source.GetBinaryLink(start + l);
                AddLink(BinaryHigh, BinaryLow, High(link), Low(link), row);
            }
            source.GetTags(object, start, count);
            _columns[ObjectTagStart].Add(_columns[Tags].Count());
            _columns[ObjectTagCount].Add(count);
            for (uint32 t = 0; t < count; ++t)
            {
                _columns[Tags].Add(tags(source.GetTagId(start + t)));
            }
        }
    }

    [[nodiscard]] PoolBuilder &Directories()
    {
        return _directories;
    }
    [[nodiscard]] PoolBuilder &FileNames()
    {
        return _fileNames;
    }
    [[nodiscard]] PoolBuilder &Extensions()
    {
        return _extensions;
    }
    [[nodiscard]] PoolBuilder &Names()
    {
        return _names;
    }
    [[nodiscard]] PoolBuilder &Types()
    {
        return _types;
    }
    [[nodiscard]] PoolBuilder &TagPool()
    {
        return _tagPool;
    }

    void Finish(Span<const String> removed, uint64 generation, List<uint8> &image)
    {
        PoolBuilder removedPool;
        for (const String &path : removed)
        {
            (void)removedPool.Add(path.AsView());
        }
        _reverse.Sort([](const ReverseLink &a, const ReverseLink &b) {
            return a.High != b.High ? a.High < b.High : (a.Low != b.Low ? a.Low < b.Low : a.Source < b.Source);
        });
        for (usize i = 0; i < _reverse.Count();)
        {
            usize end = i;
            _columns[ReverseKeyHigh].Add(_reverse[i].High);
            _columns[ReverseKeyLow].Add(_reverse[i].Low);
            _columns[ReverseStart].Add(_columns[ReverseSource].Count());
            const usize before = _columns[ReverseSource].Count();
            while (end < _reverse.Count() && _reverse[end].High == _reverse[i].High &&
                   _reverse[end].Low == _reverse[i].Low)
            {
                if (end == i || _reverse[end].Source != _reverse[end - 1].Source)
                {
                    _columns[ReverseSource].Add(_reverse[end].Source);
                }
                ++end;
            }
            _columns[ReverseCount].Add(_columns[ReverseSource].Count() - before);
            i = end;
        }
        const usize files = _columns[FileDirectory].Count();
        const usize reverseKeys = _columns[ReverseKeyHigh].Count();

        List<uint32> pathTable;
        pathTable.Resize(Capacity(files));
        List<uint32> objectTable;
        objectTable.Resize(Capacity(_columns[ObjectHigh].Count()));
        List<uint32> binaryTable;
        binaryTable.Resize(Capacity(files));
        List<uint32> reverseTable;
        reverseTable.Resize(Capacity(reverseKeys));
        for (usize f = 0; f < files; ++f)
        {
            InsertRow(pathTable, _pathHashes[f], static_cast<uint32>(f));
            const bool binary =
                _columns[FileKind][f] == 0 && (_columns[FileBinaryHigh][f] | _columns[FileBinaryLow][f]) != 0;
            if (binary)
            {
                InsertRow(binaryTable, HashGuid(_columns[FileBinaryHigh][f], _columns[FileBinaryLow][f]),
                          static_cast<uint32>(f));
            }
        }
        for (usize o = 0; o < _columns[ObjectHigh].Count(); ++o)
        {
            InsertRow(objectTable, HashGuid(_columns[ObjectHigh][o], _columns[ObjectLow][o]), static_cast<uint32>(o));
        }
        for (usize r = 0; r < reverseKeys; ++r)
        {
            InsertRow(reverseTable, HashGuid(_columns[ReverseKeyHigh][r], _columns[ReverseKeyLow][r]),
                      static_cast<uint32>(r));
        }

        image.Clear();
        image.Resize(sizeof(Header) + SectionCount * sizeof(SectionEntry));
        SectionEntry entries[SectionCount] = {};
        const auto begin = [&](Section section) {
            while ((image.Count() & 7) != 0)
            {
                image.Add(0);
            }
            entries[section].Offset = image.Count();
        };
        const auto end = [&](Section section) { entries[section].Size = image.Count() - entries[section].Offset; };
        const PoolBuilder *pools[] = {&_directories, &_fileNames, &_extensions, &_names,
                                      &_types,       &_tagPool,   &removedPool};
        for (uint32 p = 0; p <= RemovedPool; ++p)
        {
            begin(static_cast<Section>(p));
            pools[p]->Write(image);
            end(static_cast<Section>(p));
        }
        for (uint32 c = FileDirectory; c <= ReverseSource; ++c)
        {
            begin(static_cast<Section>(c));
            PackedColumnWriter::Write(_columns[c].AsSpan(), image);
            end(static_cast<Section>(c));
        }
        List<uint32> *tables[] = {&pathTable, &objectTable, &binaryTable, &reverseTable};
        for (uint32 t = 0; t < 4; ++t)
        {
            const auto section = static_cast<Section>(PathTable + t);
            begin(section);
            WriteTable(*tables[t], image);
            end(section);
        }
        const Header header{AssetDatabaseMagic, AssetDatabaseVersion, generation, SectionCount, 0};
        MemCopy(image.Data(), &header, sizeof(header));
        MemCopy(image.Data() + sizeof(Header), entries, sizeof(entries));
    }

private:
    struct ReverseLink
    {
        uint64 High;
        uint64 Low;
        uint32 Source;
    };

    void AddFileRow(uint32 directory, uint32 name, uint32 extension, const Hash128 &hash, uint64 size, int64 modified,
                    usize objects, const Guid &binary, bool isAsset)
    {
        _columns[FileDirectory].Add(directory);
        _columns[FileName].Add(name);
        _columns[FileExtension].Add(extension);
        _columns[FileHashHigh].Add(hash.High);
        _columns[FileHashLow].Add(hash.Low);
        _columns[FileSize].Add(size);
        _columns[FileModified].Add(static_cast<uint64>(modified));
        _columns[FileObjectStart].Add(_columns[ObjectHigh].Count());
        _columns[FileObjectCount].Add(objects);
        _columns[FileBinaryHigh].Add(High(binary));
        _columns[FileBinaryLow].Add(Low(binary));
        _columns[FileKind].Add(isAsset ? 1 : 0);
    }

    void AddObjectRow(const Guid &uuid, uint32 name, uint32 type, uint32 line, const Hash128 &canonical)
    {
        _columns[ObjectHigh].Add(High(uuid));
        _columns[ObjectLow].Add(Low(uuid));
        _columns[ObjectName].Add(name);
        _columns[ObjectType].Add(type);
        _columns[ObjectFile].Add(_columns[FileDirectory].Count() - 1);
        _columns[ObjectLine].Add(line);
        _columns[ObjectCanonicalHigh].Add(canonical.High);
        _columns[ObjectCanonicalLow].Add(canonical.Low);
    }

    void AddLink(Section high, Section low, uint64 targetHigh, uint64 targetLow, uint32 source)
    {
        _columns[high].Add(targetHigh);
        _columns[low].Add(targetLow);
        _reverse.Add({targetHigh, targetLow, source});
    }

    void InsertPath(uint64 hash)
    {
        _pathHashes.Add(hash);
    }

    PoolBuilder _directories;
    PoolBuilder _fileNames;
    PoolBuilder _extensions;
    PoolBuilder _names;
    PoolBuilder _types;
    PoolBuilder _tagPool;
    List<uint64> _columns[SectionCount];
    List<uint64> _pathHashes;
    List<ReverseLink> _reverse;
};
} // namespace

bool AssetStringPool::Open(const uint8 *data, usize size)
{
    PoolHeader header{};
    if (size < sizeof(header))
    {
        return false;
    }
    MemCopy(&header, data, sizeof(header));
    const usize tableSize = static_cast<usize>(header.TableCapacity) * sizeof(uint32);
    if ((header.TableCapacity & (header.TableCapacity - 1)) != 0 ||
        size < sizeof(header) + tableSize + header.OffsetsSize + header.CharsSize)
    {
        return false;
    }
    _table = reinterpret_cast<const uint32 *>(data + sizeof(header));
    _mask = header.TableCapacity - 1;
    if (!_offsets.Open(data + sizeof(header) + tableSize, static_cast<usize>(header.OffsetsSize)))
    {
        return false;
    }
    _chars = reinterpret_cast<const char *>(data + sizeof(header) + tableSize + header.OffsetsSize);
    return true;
}

uint32 AssetStringPool::Find(StringView text) const
{
    if (_table == nullptr)
    {
        return AssetDatabaseNone;
    }
    uint32 slot = static_cast<uint32>(HashPath(text)) & _mask;
    while (_table[slot] != 0)
    {
        const uint32 id = _table[slot] - 1;
        if ((*this)[id] == text)
        {
            return id;
        }
        slot = (slot + 1) & _mask;
    }
    return AssetDatabaseNone;
}

bool AssetDatabaseView::Open(Span<const uint8> image)
{
    _image = {};
    Header header{};
    if (image.Count() < sizeof(Header))
    {
        return false;
    }
    MemCopy(&header, image.Data(), sizeof(header));
    if (header.Magic != AssetDatabaseMagic || header.Version != AssetDatabaseVersion ||
        header.Sections != SectionCount || image.Count() < sizeof(Header) + SectionCount * sizeof(SectionEntry))
    {
        return false;
    }
    SectionEntry entries[SectionCount];
    MemCopy(entries, image.Data() + sizeof(Header), sizeof(entries));
    for (const SectionEntry &entry : entries)
    {
        if (entry.Offset > image.Count() || entry.Size > image.Count() - entry.Offset)
        {
            return false;
        }
    }
    const auto data = [&](Section section) { return image.Data() + entries[section].Offset; };
    const auto size = [&](Section section) { return static_cast<usize>(entries[section].Size); };
    const auto pool = [&](AssetStringPool &target, Section section) {
        return target.Open(data(section), size(section));
    };
    const auto column = [&](PackedColumnView &target, Section section) {
        return target.Open(data(section), size(section));
    };
    const auto table = [&](const uint32 *&target, uint32 &mask, Section section) {
        const usize count = size(section) / sizeof(uint32);
        if (count == 0 || (count & (count - 1)) != 0)
        {
            return false;
        }
        target = reinterpret_cast<const uint32 *>(data(section));
        mask = static_cast<uint32>(count - 1);
        return true;
    };
    const bool ok =
        pool(_directories, DirectoryPool) && pool(_fileNames, FileNamePool) && pool(_extensions, ExtensionPool) &&
        pool(_names, NamePool) && pool(_types, TypePool) && pool(_tagPool, TagPool) && pool(_removed, RemovedPool) &&
        column(_fileDirectory, FileDirectory) && column(_fileName, FileName) && column(_fileExtension, FileExtension) &&
        column(_fileHashHigh, FileHashHigh) && column(_fileHashLow, FileHashLow) && column(_fileSize, FileSize) &&
        column(_fileModified, FileModified) && column(_fileObjectStart, FileObjectStart) &&
        column(_fileObjectCount, FileObjectCount) && column(_fileBinaryHigh, FileBinaryHigh) &&
        column(_fileBinaryLow, FileBinaryLow) && column(_fileFlags, FileKind) && column(_objectHigh, ObjectHigh) &&
        column(_objectLow, ObjectLow) && column(_objectName, ObjectName) && column(_objectType, ObjectType) &&
        column(_objectFile, ObjectFile) && column(_objectLine, ObjectLine) &&
        column(_objectLinkStart, ObjectLinkStart) && column(_objectLinkCount, ObjectLinkCount) &&
        column(_objectBinaryStart, ObjectBinaryStart) && column(_objectBinaryCount, ObjectBinaryCount) &&
        column(_objectTagStart, ObjectTagStart) && column(_objectTagCount, ObjectTagCount) &&
        column(_objectCanonicalHigh, ObjectCanonicalHigh) && column(_objectCanonicalLow, ObjectCanonicalLow) &&
        column(_linkHigh, LinkHigh) && column(_linkLow, LinkLow) && column(_binaryHigh, BinaryHigh) &&
        column(_binaryLow, BinaryLow) && column(_tags, Tags) && column(_reverseKeyHigh, ReverseKeyHigh) &&
        column(_reverseKeyLow, ReverseKeyLow) && column(_reverseStart, ReverseStart) &&
        column(_reverseCount, ReverseCount) && column(_reverseSource, ReverseSource) &&
        table(_pathTable, _pathMask, PathTable) && table(_objectTable, _objectMask, ObjectTable) &&
        table(_binaryTable, _binaryMask, BinaryTable) && table(_reverseTable, _reverseMask, ReverseTable);
    if (!ok)
    {
        return false;
    }
    _image = image;
    return true;
}

uint64 AssetDatabaseView::GetGeneration() const
{
    Header header{};
    MemCopy(&header, _image.Data(), sizeof(header));
    return header.Generation;
}

void AssetDatabaseView::GetPath(uint32 file, StringWriter &output) const
{
    output.Append(_directories[static_cast<uint32>(_fileDirectory[file])]).Append('/');
    output.Append(_fileNames[static_cast<uint32>(_fileName[file])]);
    output.Append(_extensions[static_cast<uint32>(_fileExtension[file])]);
}

uint32 AssetDatabaseView::Probe(const uint32 *table, uint32 mask, uint64 hash, const auto &matches) const
{
    uint32 slot = static_cast<uint32>(hash) & mask;
    while (table[slot] != 0)
    {
        const uint32 row = table[slot] - 1;
        if (matches(row))
        {
            return row;
        }
        slot = (slot + 1) & mask;
    }
    return AssetDatabaseNone;
}

uint32 AssetDatabaseView::FindFile(StringView path) const
{
    if (!IsOpen())
    {
        return AssetDatabaseNone;
    }
    StringView directory;
    StringView name;
    StringView extension;
    SplitPath(path, directory, name, extension);
    const uint32 directoryId = _directories.Find(directory);
    const uint32 nameId = _fileNames.Find(name);
    const uint32 extensionId = _extensions.Find(extension);
    if (directoryId == AssetDatabaseNone || nameId == AssetDatabaseNone || extensionId == AssetDatabaseNone)
    {
        return AssetDatabaseNone;
    }
    return Probe(_pathTable, _pathMask, HashPath(path), [&](uint32 file) {
        return _fileDirectory[file] == directoryId && _fileName[file] == nameId && _fileExtension[file] == extensionId;
    });
}

uint32 AssetDatabaseView::LowerBound(StringView path) const
{
    uint32 low = 0;
    uint32 high = IsOpen() ? FileCount() : 0;
    StringBuilder<512> probe;
    while (low < high)
    {
        const uint32 middle = low + (high - low) / 2;
        probe.Clear();
        GetPath(middle, probe);
        if (probe.AsView() < path)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low;
}

uint32 AssetDatabaseView::FindBinary(const Guid &uuid) const
{
    if (!IsOpen())
    {
        return AssetDatabaseNone;
    }
    const uint64 high = High(uuid);
    const uint64 low = Low(uuid);
    return Probe(_binaryTable, _binaryMask, HashGuid(high, low),
                 [&](uint32 file) { return _fileBinaryHigh[file] == high && _fileBinaryLow[file] == low; });
}

uint32 AssetDatabaseView::FindObject(const Guid &uuid) const
{
    if (!IsOpen())
    {
        return AssetDatabaseNone;
    }
    const uint64 high = High(uuid);
    const uint64 low = Low(uuid);
    return Probe(_objectTable, _objectMask, HashGuid(high, low),
                 [&](uint32 object) { return _objectHigh[object] == high && _objectLow[object] == low; });
}

void AssetDatabaseView::GetReferencers(const Guid &uuid, uint32 &start, uint32 &count) const
{
    start = 0;
    count = 0;
    if (!IsOpen())
    {
        return;
    }
    const uint64 high = High(uuid);
    const uint64 low = Low(uuid);
    const uint32 entry = Probe(_reverseTable, _reverseMask, HashGuid(high, low),
                               [&](uint32 key) { return _reverseKeyHigh[key] == high && _reverseKeyLow[key] == low; });
    if (entry != AssetDatabaseNone)
    {
        start = static_cast<uint32>(_reverseStart[entry]);
        count = static_cast<uint32>(_reverseCount[entry]);
    }
}

void AssetDatabaseView::ToRecord(uint32 file, AssetFileRecord &record) const
{
    StringBuilder<512> path;
    GetPath(file, path);
    record.Path = path.AsView();
    record.ContentHash = GetContentHash(file);
    record.Size = GetSize(file);
    record.ModifiedTime = GetModifiedTime(file);
    record.IsAsset = IsAsset(file);
    record.BinaryGuid = GetBinaryGuid(file);
    record.Objects.Clear();
    for (uint32 i = 0; i < GetObjectCount(file); ++i)
    {
        const uint32 object = GetObjectStart(file) + i;
        AssetObjectRecord &target = record.Objects.AddDefaulted();
        target.Uuid = GetUuid(object);
        target.Name = GetName(object);
        target.Type = GetType(object);
        target.Line = GetLine(object);
        target.Canonical = GetCanonicalHash(object);
        uint32 start = 0;
        uint32 count = 0;
        GetLinks(object, start, count);
        for (uint32 l = 0; l < count; ++l)
        {
            target.Links.Add(GetLink(start + l));
        }
        GetBinaryLinks(object, start, count);
        for (uint32 l = 0; l < count; ++l)
        {
            target.BinaryLinks.Add(GetBinaryLink(start + l));
        }
        GetTags(object, start, count);
        for (uint32 t = 0; t < count; ++t)
        {
            target.Tags.Add(String(_tagPool[GetTagId(start + t)]));
        }
    }
}

namespace AssetDatabaseBuilder
{
void Build(Span<const AssetFileRecord *const> records, Span<const String> removed, uint64 generation,
           List<uint8> &image)
{
    List<const AssetFileRecord *> files(records);
    files.Sort([](const AssetFileRecord *a, const AssetFileRecord *b) { return a->Path.AsView() < b->Path.AsView(); });
    ImageBuilder builder;
    for (const AssetFileRecord *file : files)
    {
        builder.AddRecord(*file);
    }
    builder.Finish(removed, generation, image);
}

void Merge(const AssetDatabaseView &base, Span<const String> replaced, Span<const AssetFileRecord *const> records,
           uint64 generation, List<uint8> &image)
{
    List<const AssetFileRecord *> files(records);
    files.Sort([](const AssetFileRecord *a, const AssetFileRecord *b) { return a->Path.AsView() < b->Path.AsView(); });
    const uint32 baseFiles = base.IsOpen() ? base.FileCount() : 0;
    BitArray<> dropped(baseFiles);
    for (const String &path : replaced)
    {
        const uint32 file = base.FindFile(path.AsView());
        if (file != AssetDatabaseNone)
        {
            dropped.Set(file);
        }
    }
    ImageBuilder builder;
    PoolRemap directories(base.GetDirectoryPool(), builder.Directories());
    PoolRemap fileNames(base.GetFileNamePool(), builder.FileNames());
    PoolRemap extensions(base.GetExtensionPool(), builder.Extensions());
    PoolRemap names(base.GetNamePool(), builder.Names());
    PoolRemap types(base.GetTypePool(), builder.Types());
    PoolRemap tags(base.GetTagPool(), builder.TagPool());
    // Both inputs are sorted by path, so merging them keeps the image sorted.
    usize next = 0;
    StringBuilder<512> path;
    for (uint32 file = 0; file < baseFiles; ++file)
    {
        if (dropped[file])
        {
            continue;
        }
        path.Clear();
        base.GetPath(file, path);
        while (next < files.Count() && files[next]->Path.AsView() < path.AsView())
        {
            builder.AddRecord(*files[next++]);
        }
        builder.AddImageFile(base, file, path.AsView(), directories, fileNames, extensions, names, types, tags);
    }
    while (next < files.Count())
    {
        builder.AddRecord(*files[next++]);
    }
    builder.Finish({}, generation, image);
}
} // namespace AssetDatabaseBuilder
} // namespace Marlin
