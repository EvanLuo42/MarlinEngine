#include "Packaging/PakWriter.h"

#include "Concurrency/Jobs.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformCrypto.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSystem.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr uint64 DataAlignment = 4096;
constexpr uint64 EntryAlignment = 16;

int32 Compare(StringView a, StringView b)
{
    const usize count = a.Length() < b.Length() ? a.Length() : b.Length();
    const int32 order = count != 0 ? MemCompare(a.Data(), b.Data(), count) : 0;
    if (order != 0)
    {
        return order;
    }
    return a.Length() < b.Length() ? -1 : a.Length() > b.Length() ? 1 : 0;
}

void CopyName(char *target, usize capacity, StringView name)
{
    MemZero(target, capacity);
    MemCopy(target, name.Data(), name.Length() < capacity ? name.Length() : capacity - 1);
}

uint64 AlignUp(uint64 value, uint64 alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}
} // namespace

PakWriter::PakWriter(StringView chunk, const CompressionCodec *codec, int32 level, uint32 blockSize,
                     const PakEncryption *encryption)
    : _chunk(chunk), _codec(codec), _level(level), _blockSize(blockSize != 0 ? blockSize : PakDefaultBlockSize),
      _encryption(encryption)
{
}

void PakWriter::AddFile(StringView mountPoint, StringView layer, StringView path, Span<const uint8> bytes)
{
    File &file = _files.AddDefaulted();
    file.MountPoint = mountPoint;
    file.Layer = layer;
    file.Path = path;
    file.Bytes.AddRange(bytes);
}

void PakWriter::AddDeletedFile(StringView mountPoint, StringView layer, StringView path)
{
    File &file = _files.AddDefaulted();
    file.MountPoint = mountPoint;
    file.Layer = layer;
    file.Path = path;
    file.Deleted = true;
}

void PakWriter::AddAsset(const Guid &uuid, Span<const uint8> block, Span<const uint8> bulk,
                         Span<const Guid> dependencies)
{
    List<uint8> content;
    content.Reserve(block.Count() + bulk.Count());
    content.AddRange(block);
    content.AddRange(bulk);
    AddAsset(uuid, Move(content), block.Count(), dependencies);
}

void PakWriter::AddAsset(const Guid &uuid, List<uint8> content, usize blockSize, Span<const Guid> dependencies)
{
    MARLIN_ASSERT(blockSize <= content.Count());
    Asset &asset = _assets.AddDefaulted();
    asset.Uuid = uuid;
    asset.Content = Move(content);
    asset.BlockSize = blockSize;
    asset.Dependencies.AddRange(dependencies);
}

void PakWriter::AddDeletedAsset(const Guid &uuid)
{
    Asset &asset = _assets.AddDefaulted();
    asset.Uuid = uuid;
    asset.Deleted = true;
}

bool PakWriter::Write(StringView path, StringWriter &error, List<uint8> *blockHashes) const
{
    if (path.IsEmpty())
    {
        error.Append(Format("{0} cannot be written", path));
        return false;
    }
    const PakCipher *cipher = _encryption != nullptr ? _encryption->Cipher : nullptr;
    if (cipher != nullptr && _encryption->Key.Count() != cipher->GetKeySize())
    {
        error.Append(Format("{0} takes a key of {1} bytes", cipher->GetName(), cipher->GetKeySize()));
        return false;
    }

    // The logical stream: assets in the order added, so a load order lays them out as the game reads them, then
    // files. Tables are sorted for binary search; the stream is not.
    // RECOVERY-DECISION SP-D1: assets before files in the stream; both keep the order they were added in.
    List<uint8> stream;
    {
        usize reserve = 0;
        for (const Asset &asset : _assets)
        {
            reserve += asset.Content.Count() + 2 * EntryAlignment;
        }
        for (const File &file : _files)
        {
            reserve += file.Bytes.Count() + EntryAlignment;
        }
        stream.Reserve(reserve);
    }
    const auto append = [&](Span<const uint8> bytes) {
        const uint64 offset = AlignUp(stream.Count(), EntryAlignment);
        stream.Resize(static_cast<usize>(offset));
        stream.AddRange(bytes);
        return offset;
    };
    List<PakAssetEntry> assets;
    List<Guid> dependencies;
    for (const Asset &asset : _assets)
    {
        PakAssetEntry &entry = assets.AddDefaulted();
        entry.Uuid = asset.Uuid;
        if (asset.Deleted)
        {
            continue;
        }
        const Span<const uint8> content = asset.Content.AsSpan();
        entry.BlockOffset = append(content.Slice(0, asset.BlockSize));
        entry.BlockSize = asset.BlockSize;
        if (content.Count() > asset.BlockSize)
        {
            entry.BulkOffset = append(content.Slice(asset.BlockSize, content.Count() - asset.BlockSize));
            entry.BulkSize = content.Count() - asset.BlockSize;
        }
        entry.ContentHash = Xxh3Hash128(content.Data(), content.Count());
        entry.DependencyIndex = static_cast<uint32>(dependencies.Count());
        entry.DependencyCount = static_cast<uint32>(asset.Dependencies.Count());
        dependencies.AddRange(asset.Dependencies.AsSpan());
    }
    struct FileRef
    {
        const File *Source;
        PakFileEntry Entry;
    };
    List<FileRef> files;
    for (const File &file : _files)
    {
        FileRef &ref = files.AddDefaulted();
        ref.Source = &file;
        if (file.Deleted)
        {
            ref.Entry.Flags = PakFileDeleted;
            continue;
        }
        ref.Entry.Offset = append(file.Bytes.AsSpan());
        ref.Entry.Size = file.Bytes.Count();
        ref.Entry.ContentHash = Xxh3Hash128(file.Bytes.Data(), file.Bytes.Count());
    }
    files.Sort([](const FileRef &a, const FileRef &b) {
        int32 order = Compare(a.Source->MountPoint.AsView(), b.Source->MountPoint.AsView());
        order = order != 0 ? order : Compare(a.Source->Layer.AsView(), b.Source->Layer.AsView());
        return (order != 0 ? order : Compare(a.Source->Path.AsView(), b.Source->Path.AsView())) < 0;
    });
    assets.Sort(
        [](const PakAssetEntry &a, const PakAssetEntry &b) { return MemCompare(&a.Uuid, &b.Uuid, sizeof(Guid)) < 0; });
    for (usize i = 1; i < assets.Count(); ++i)
    {
        if (assets[i].Uuid == assets[i - 1].Uuid)
        {
            error.Append(Format("asset {0} is in the package twice", assets[i].Uuid));
            return false;
        }
    }

    PakHeader header;
    header.BlockSize = _blockSize;
    if (_codec != nullptr)
    {
        CopyName(header.Codec, sizeof(header.Codec), _codec->GetName());
    }
    CopyName(header.Chunk, sizeof(header.Chunk), _chunk.AsView());
    header.LogicalSize = stream.Count();
    header.BlockCount = (header.LogicalSize + _blockSize - 1) / _blockSize;
    if (cipher != nullptr)
    {
        CopyName(header.Cipher, sizeof(header.Cipher), cipher->GetName());
        header.KeyId = _encryption->KeyId;
        PlatformSystem::GetSecureRandomBytes(&header.Salt, sizeof(header.Salt));
    }

    // Each block is compressed on its own, or stored when that does not make it smaller, then encrypted with its
    // index as the cipher's position, as PakFile::Decompress reads it.
    List<List<uint8>> pieces;
    pieces.Resize(static_cast<usize>(header.BlockCount));
    List<uint8> stored;
    stored.Resize(static_cast<usize>(header.BlockCount));
    List<uint8> failed;
    failed.Resize(static_cast<usize>(header.BlockCount));
    const auto compress = [&](uint32 index) {
        const uint64 start = uint64{index} * _blockSize;
        const uint64 size = header.LogicalSize - start < _blockSize ? header.LogicalSize - start : _blockSize;
        const Span<const uint8> block(stream.Data() + start, static_cast<usize>(size));
        List<uint8> &piece = pieces[index];
        if (_codec == nullptr || !_codec->Compress(block, _level, piece) || piece.Count() >= block.Count())
        {
            piece.Clear();
            piece.AddRange(block);
            stored[index] = 1;
        }
        if (cipher != nullptr && !cipher->Apply(_encryption->Key.AsSpan(), header.Salt, index, piece.AsSpan()))
        {
            failed[index] = 1;
        }
        if (blockHashes != nullptr)
        {
            uint8 hash[PlatformCrypto::Sha256Size];
            if (!PlatformCrypto::Sha256(piece.AsSpan(), hash))
            {
                failed[index] = 2;
            }
            MemCopy(blockHashes->Data() + uint64{index} * PakBlockHashSize, hash, PakBlockHashSize);
        }
    };
    if (blockHashes != nullptr)
    {
        blockHashes->Clear();
        blockHashes->ResizeUninitialized(static_cast<usize>(header.BlockCount * PakBlockHashSize));
    }
    Jobs::ParallelFor(static_cast<uint32>(header.BlockCount), 1, [&](uint32 begin, uint32 end) {
        for (uint32 index = begin; index < end; ++index)
        {
            compress(index);
        }
    });
    for (const uint8 blockFailed : failed)
    {
        if (blockFailed == 2)
        {
            error.Append("its blocks could not be hashed");
            return false;
        }
        if (blockFailed != 0)
        {
            error.Append(Format("{0} refused to encrypt the package", cipher->GetName()));
            return false;
        }
    }
    stream = List<uint8>();

    // The blocks go straight into the file's bytes, each piece freed once copied.
    List<uint8> output;
    {
        usize reserve = static_cast<usize>(DataAlignment);
        for (const List<uint8> &piece : pieces)
        {
            reserve += piece.Count();
        }
        output.Reserve(reserve + 2 * 8 + static_cast<usize>(header.BlockCount) * sizeof(PakBlock) +
                       assets.Count() * sizeof(PakAssetEntry) + dependencies.Count() * sizeof(Guid));
    }
    output.Resize(static_cast<usize>(DataAlignment));
    List<PakBlock> blocks;
    uint64 offset = DataAlignment;
    for (usize i = 0; i < pieces.Count(); ++i)
    {
        PakBlock &block = blocks.AddDefaulted();
        block.Offset = offset;
        block.Stored = stored[i];
        block.StoredSize = static_cast<uint32>(pieces[i].Count());
        output.AddRange(pieces[i].AsSpan());
        pieces[i] = List<uint8>();
        offset += block.StoredSize;
    }

    List<char> strings;
    HashMap<String, PakString> interned;
    const auto intern = [&](const String &text) {
        if (const PakString *found = interned.Find(text))
        {
            return *found;
        }
        const PakString entry{static_cast<uint32>(strings.Count()), static_cast<uint32>(text.Length())};
        strings.AddRange(Span<const char>(text.Data(), text.Length()));
        interned.FindOrAdd(text) = entry;
        return entry;
    };
    List<PakFileEntry> fileTable;
    for (const FileRef &file : files)
    {
        PakFileEntry entry = file.Entry;
        entry.MountPoint = intern(file.Source->MountPoint);
        entry.Layer = intern(file.Source->Layer);
        entry.Path = intern(file.Source->Path);
        fileTable.Add(entry);
    }

    // The tables are encrypted whole, each from its own position; the header stays plain so a reader finds them.
    if (cipher != nullptr)
    {
        const Span<const uint8> key = _encryption->Key.AsSpan();
        if (!cipher->Apply(key, header.Salt, PakBlockTablePosition, AsWritableBytes(blocks.AsSpan())) ||
            !cipher->Apply(key, header.Salt, PakStringTablePosition, AsWritableBytes(strings.AsSpan())) ||
            !cipher->Apply(key, header.Salt, PakFileTablePosition, AsWritableBytes(fileTable.AsSpan())) ||
            !cipher->Apply(key, header.Salt, PakAssetTablePosition, AsWritableBytes(assets.AsSpan())) ||
            !cipher->Apply(key, header.Salt, PakDependencyTablePosition, AsWritableBytes(dependencies.AsSpan())))
        {
            error.Append(Format("{0} refused to encrypt the tables", cipher->GetName()));
            return false;
        }
    }

    header.BlockTableOffset = AlignUp(offset, 8);
    header.StringTableOffset = header.BlockTableOffset + blocks.Count() * sizeof(PakBlock);
    header.StringTableSize = strings.Count();
    header.FileTableOffset = AlignUp(header.StringTableOffset + header.StringTableSize, 8);
    header.FileCount = fileTable.Count();
    header.AssetTableOffset = header.FileTableOffset + fileTable.Count() * sizeof(PakFileEntry);
    header.AssetCount = assets.Count();
    header.DependencyTableOffset = header.AssetTableOffset + assets.Count() * sizeof(PakAssetEntry);
    header.DependencyCount = dependencies.Count();

    MemCopy(output.Data(), &header, sizeof(header));
    output.Resize(static_cast<usize>(header.BlockTableOffset));
    output.AddRange(AsBytes(blocks.AsSpan()));
    output.AddRange(AsBytes(strings.AsSpan()));
    output.Resize(static_cast<usize>(header.FileTableOffset));
    output.AddRange(AsBytes(fileTable.AsSpan()));
    output.AddRange(AsBytes(assets.AsSpan()));
    output.AddRange(AsBytes(dependencies.AsSpan()));

    // The temporary file sits beside *path* under its own name, so the only file this deletes is one it created.
    const String target(path);
    const String temporary = Format("{0}.tmp", path);
    const FileHandle file = PlatformFile::Open(temporary.CStr(), FileAccess::Write, FileMode::CreateAlways);
    bool ok = file.IsValid() && PlatformFile::Write(file, output.Data(), output.Count());
    if (file.IsValid())
    {
        ok = PlatformFile::Flush(file) && ok;
        PlatformFile::Close(file);
    }
    if (!ok || !PlatformFile::Move(temporary.CStr(), target.CStr(), true))
    {
        if (file.IsValid())
        {
            (void)PlatformFile::Delete(temporary.CStr());
        }
        error.Append(Format("{0} cannot be written", path));
        return false;
    }
    return true;
}
} // namespace Marlin
