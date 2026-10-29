#pragma once

#include "AssetPipeline/PackedColumn.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;
struct AssetFileRecord;

inline constexpr uint32 AssetDatabaseMagic = 0x4244414Du;
inline constexpr uint32 AssetDatabaseVersion = 2;
inline constexpr uint32 AssetDatabaseNone = ~0u;

/// Unique strings with a hash table for exact lookup; ids follow first appearance.
class ASSETPIPELINE_API AssetStringPool
{
public:
    bool Open(const uint8 *data, usize size);

    [[nodiscard]] uint32 Count() const
    {
        return _offsets.Count() != 0 ? _offsets.Count() - 1 : 0;
    }

    [[nodiscard]] StringView operator[](uint32 id) const
    {
        const auto begin = static_cast<usize>(_offsets[id]);
        return {_chars + begin, static_cast<usize>(_offsets[id + 1]) - begin};
    }

    /// The id of *text*, or AssetDatabaseNone.
    [[nodiscard]] uint32 Find(StringView text) const;

private:
    PackedColumnView _offsets;
    const char *_chars = nullptr;
    const uint32 *_table = nullptr;
    uint32 _mask = 0;
};

/// An immutable asset database image (Documentation/Architecture/06 §2): what AssetDatabaseBuilder writes, read in
/// place from memory, a mapped file or shared memory. Paths are split into directory, file name and extension
/// pools; every column is bit-packed; hash tables find files by path, objects by UUID and binary files by GUID;
/// incoming links are a precomputed table. Files are sorted by path and own contiguous object rows, objects own
/// contiguous link rows.
class ASSETPIPELINE_API AssetDatabaseView
{
public:
    /// False if *image* is not a valid database.
    bool Open(Span<const uint8> image);

    [[nodiscard]] bool IsOpen() const
    {
        return _image.Count() != 0;
    }

    [[nodiscard]] uint64 GetGeneration() const;
    [[nodiscard]] Span<const uint8> GetImage() const
    {
        return _image;
    }

    [[nodiscard]] uint32 FileCount() const
    {
        return _fileDirectory.Count();
    }

    [[nodiscard]] uint32 ObjectCount() const
    {
        return _objectHigh.Count();
    }

    [[nodiscard]] uint32 LinkCount() const
    {
        return _linkHigh.Count();
    }

    // Files.
    void GetPath(uint32 file, StringWriter &output) const;
    [[nodiscard]] StringView GetDirectory(uint32 file) const
    {
        return _directories[static_cast<uint32>(_fileDirectory[file])];
    }
    [[nodiscard]] Hash128 GetContentHash(uint32 file) const
    {
        return {_fileHashLow[file], _fileHashHigh[file]};
    }
    [[nodiscard]] uint64 GetSize(uint32 file) const
    {
        return _fileSize[file];
    }
    [[nodiscard]] int64 GetModifiedTime(uint32 file) const
    {
        return static_cast<int64>(_fileModified[file]);
    }
    [[nodiscard]] bool IsAsset(uint32 file) const
    {
        return (_fileFlags[file] & 1) != 0;
    }
    [[nodiscard]] Guid GetBinaryGuid(uint32 file) const
    {
        return MakeGuid(_fileBinaryHigh[file], _fileBinaryLow[file]);
    }
    [[nodiscard]] uint32 GetObjectStart(uint32 file) const
    {
        return static_cast<uint32>(_fileObjectStart[file]);
    }
    [[nodiscard]] uint32 GetObjectCount(uint32 file) const
    {
        return static_cast<uint32>(_fileObjectCount[file]);
    }
    [[nodiscard]] uint32 FindFile(StringView path) const;
    [[nodiscard]] uint32 FindBinary(const Guid &uuid) const;

    // Objects.
    [[nodiscard]] Guid GetUuid(uint32 object) const
    {
        return MakeGuid(_objectHigh[object], _objectLow[object]);
    }
    [[nodiscard]] StringView GetName(uint32 object) const
    {
        return _names[static_cast<uint32>(_objectName[object])];
    }
    [[nodiscard]] uint32 GetNameId(uint32 object) const
    {
        return static_cast<uint32>(_objectName[object]);
    }
    [[nodiscard]] StringView GetType(uint32 object) const
    {
        return _types[static_cast<uint32>(_objectType[object])];
    }
    [[nodiscard]] uint32 GetTypeId(uint32 object) const
    {
        return static_cast<uint32>(_objectType[object]);
    }
    [[nodiscard]] uint32 GetFile(uint32 object) const
    {
        return static_cast<uint32>(_objectFile[object]);
    }
    [[nodiscard]] uint32 GetLine(uint32 object) const
    {
        return static_cast<uint32>(_objectLine[object]);
    }
    /// The canonical hash of the object's source (AssetCanonical), what a conversion depends on.
    [[nodiscard]] Hash128 GetCanonicalHash(uint32 object) const
    {
        return {_objectCanonicalLow[object], _objectCanonicalHigh[object]};
    }
    [[nodiscard]] uint32 FindObject(const Guid &uuid) const;

    /// Object links, binary links and tags of an object, as row ranges.
    void GetLinks(uint32 object, uint32 &start, uint32 &count) const
    {
        start = static_cast<uint32>(_objectLinkStart[object]);
        count = static_cast<uint32>(_objectLinkCount[object]);
    }
    [[nodiscard]] Guid GetLink(uint32 link) const
    {
        return MakeGuid(_linkHigh[link], _linkLow[link]);
    }
    void GetBinaryLinks(uint32 object, uint32 &start, uint32 &count) const
    {
        start = static_cast<uint32>(_objectBinaryStart[object]);
        count = static_cast<uint32>(_objectBinaryCount[object]);
    }
    [[nodiscard]] Guid GetBinaryLink(uint32 link) const
    {
        return MakeGuid(_binaryHigh[link], _binaryLow[link]);
    }
    void GetTags(uint32 object, uint32 &start, uint32 &count) const
    {
        start = static_cast<uint32>(_objectTagStart[object]);
        count = static_cast<uint32>(_objectTagCount[object]);
    }
    [[nodiscard]] uint32 GetTagId(uint32 tag) const
    {
        return static_cast<uint32>(_tags[tag]);
    }

    /// The objects linking to *uuid*, an object or a binary file: rows [start, start + count) of GetReferencer.
    void GetReferencers(const Guid &uuid, uint32 &start, uint32 &count) const;
    [[nodiscard]] uint32 GetReferencer(uint32 index) const
    {
        return static_cast<uint32>(_reverseSource[index]);
    }

    [[nodiscard]] const AssetStringPool &GetNamePool() const
    {
        return _names;
    }
    [[nodiscard]] const AssetStringPool &GetTypePool() const
    {
        return _types;
    }
    [[nodiscard]] const AssetStringPool &GetTagPool() const
    {
        return _tagPool;
    }
    [[nodiscard]] const AssetStringPool &GetDirectoryPool() const
    {
        return _directories;
    }
    [[nodiscard]] uint32 GetDirectoryId(uint32 file) const
    {
        return static_cast<uint32>(_fileDirectory[file]);
    }
    [[nodiscard]] const AssetStringPool &GetFileNamePool() const
    {
        return _fileNames;
    }
    [[nodiscard]] uint32 GetFileNameId(uint32 file) const
    {
        return static_cast<uint32>(_fileName[file]);
    }
    [[nodiscard]] const AssetStringPool &GetExtensionPool() const
    {
        return _extensions;
    }
    [[nodiscard]] uint32 GetExtensionId(uint32 file) const
    {
        return static_cast<uint32>(_fileExtension[file]);
    }
    /// The first file whose path is not less than *path*: files are sorted by path, so the files below a directory
    /// are the run that starts here.
    [[nodiscard]] uint32 LowerBound(StringView path) const;

    /// Paths of the base this delta image overrides or removes; empty for a base image.
    [[nodiscard]] const AssetStringPool &GetRemovedPaths() const
    {
        return _removed;
    }

    /// Rebuilds the record of a file, for merging into a new image.
    void ToRecord(uint32 file, AssetFileRecord &record) const;

    [[nodiscard]] static Guid MakeGuid(uint64 high, uint64 low)
    {
        return {static_cast<uint32>(high >> 32), static_cast<uint32>(high), static_cast<uint32>(low >> 32),
                static_cast<uint32>(low)};
    }

private:
    [[nodiscard]] uint32 Probe(const uint32 *table, uint32 mask, uint64 hash, const auto &matches) const;

    Span<const uint8> _image;
    AssetStringPool _directories;
    AssetStringPool _fileNames;
    AssetStringPool _extensions;
    AssetStringPool _names;
    AssetStringPool _types;
    AssetStringPool _tagPool;
    PackedColumnView _fileDirectory;
    PackedColumnView _fileName;
    PackedColumnView _fileExtension;
    PackedColumnView _fileHashHigh;
    PackedColumnView _fileHashLow;
    PackedColumnView _fileSize;
    PackedColumnView _fileModified;
    PackedColumnView _fileObjectStart;
    PackedColumnView _fileObjectCount;
    PackedColumnView _fileBinaryHigh;
    PackedColumnView _fileBinaryLow;
    PackedColumnView _fileFlags;
    PackedColumnView _objectHigh;
    PackedColumnView _objectLow;
    PackedColumnView _objectName;
    PackedColumnView _objectType;
    PackedColumnView _objectFile;
    PackedColumnView _objectLine;
    PackedColumnView _objectLinkStart;
    PackedColumnView _objectLinkCount;
    PackedColumnView _objectBinaryStart;
    PackedColumnView _objectBinaryCount;
    PackedColumnView _objectTagStart;
    PackedColumnView _objectTagCount;
    PackedColumnView _objectCanonicalHigh;
    PackedColumnView _objectCanonicalLow;
    PackedColumnView _linkHigh;
    PackedColumnView _linkLow;
    PackedColumnView _binaryHigh;
    PackedColumnView _binaryLow;
    PackedColumnView _tags;
    PackedColumnView _reverseKeyHigh;
    PackedColumnView _reverseKeyLow;
    PackedColumnView _reverseStart;
    PackedColumnView _reverseCount;
    PackedColumnView _reverseSource;
    AssetStringPool _removed;
    const uint32 *_pathTable = nullptr;
    const uint32 *_objectTable = nullptr;
    const uint32 *_binaryTable = nullptr;
    const uint32 *_reverseTable = nullptr;
    uint32 _pathMask = 0;
    uint32 _objectMask = 0;
    uint32 _binaryMask = 0;
    uint32 _reverseMask = 0;

    friend class AssetIndexView;
};

namespace AssetDatabaseBuilder
{
/// Writes an image of *records*, sorted by path; *removed* lists paths a delta takes away from its base.
ASSETPIPELINE_API void Build(Span<const AssetFileRecord *const> records, Span<const String> removed, uint64 generation,
                             List<uint8> &image);
/// Writes a base image of *base*'s files, less those *replaced* names, plus *records*, sorted by path. Rows of *base*
/// are copied column by column, never turned into records.
ASSETPIPELINE_API void Merge(const AssetDatabaseView &base, Span<const String> replaced,
                             Span<const AssetFileRecord *const> records, uint64 generation, List<uint8> &image);
} // namespace AssetDatabaseBuilder
} // namespace Marlin
