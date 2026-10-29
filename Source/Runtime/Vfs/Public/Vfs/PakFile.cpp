#include "Vfs/PakFile.h"

#include "Concurrency/ThreadLocal.h"
#include "Containers/HashSet.h"
#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
/// Where Decompress decrypts a block it may not write, one per thread and kept, so a block needs no allocation.
ThreadLocal<List<uint8>> DecryptScratch;

template <typename T> bool ReadTable(FileHandle file, uint64 fileSize, uint64 offset, uint64 count, List<T> &table)
{
    if (count > fileSize / sizeof(T) || offset > fileSize || count * sizeof(T) > fileSize - offset)
    {
        return false;
    }
    table.Clear();
    table.Resize(static_cast<usize>(count));
    usize read = 0;
    return count == 0 || (PlatformFile::ReadAt(file, offset, table.Data(), table.Count() * sizeof(T), read) &&
                          read == table.Count() * sizeof(T));
}

StringView FixedName(const char *text, usize capacity)
{
    usize length = 0;
    while (length < capacity && text[length] != '\0')
    {
        ++length;
    }
    return {text, length};
}

/// Reads the .msig beside package *path*; false if there is none or it is not one.
bool ReadSignature(StringView path, uint64 blockCount, PakSignatureHeader &signature, List<uint8> &hashes)
{
    const String native = PakSigning::SignaturePath(path);
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    bool ok = PlatformFile::GetSize(file, size) && PlatformFile::ReadAt(file, 0, &signature, sizeof(signature), read) &&
              read == sizeof(signature) && signature.Magic == PakSignatureMagic &&
              signature.Version == PakSignatureVersion && signature.SignatureSize <= sizeof(signature.Signature) &&
              signature.BlockCount == blockCount && size == sizeof(signature) + blockCount * PakBlockHashSize;
    if (ok)
    {
        hashes.ResizeUninitialized(static_cast<usize>(blockCount * PakBlockHashSize));
        ok = hashes.IsEmpty() || (PlatformFile::ReadAt(file, sizeof(signature), hashes.Data(), hashes.Count(), read) &&
                                  read == hashes.Count());
    }
    PlatformFile::Close(file);
    return ok;
}

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
} // namespace

SharedPtr<PakFile> PakFile::Open(StringView path, const CompressionCodecSet &codecs, const PakCrypto *crypto)
{
    const String native(path);
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return {};
    }
    SharedPtr<PakFile> pak(new PakFile());
    uint64 size = 0;
    usize read = 0;
    PakHeader &header = pak->_header;
    bool ok = PlatformFile::GetSize(file, size) && PlatformFile::ReadAt(file, 0, &header, sizeof(header), read) &&
              read == sizeof(header) && header.Magic == PakMagic && header.Version == PakVersion &&
              header.BlockSize != 0 &&
              header.BlockCount == (header.LogicalSize + header.BlockSize - 1) / header.BlockSize &&
              ReadTable(file, size, header.BlockTableOffset, header.BlockCount, pak->_blocks) &&
              ReadTable(file, size, header.StringTableOffset, header.StringTableSize, pak->_strings) &&
              ReadTable(file, size, header.FileTableOffset, header.FileCount, pak->_files) &&
              ReadTable(file, size, header.AssetTableOffset, header.AssetCount, pak->_assets) &&
              ReadTable(file, size, header.DependencyTableOffset, header.DependencyCount, pak->_dependencies);
    PlatformFile::Close(file);

    // The signature covers the header and tables as read here, before decrypting, so what is checked is what is used.
    PakSignatureHeader signature;
    List<uint8> hashes;
    if (ok && crypto != nullptr && ReadSignature(path, header.BlockCount, signature, hashes))
    {
        const PakSignatureScheme *scheme = crypto->FindScheme(FixedName(signature.Scheme, sizeof(signature.Scheme)));
        List<uint8> publicKey;
        if (scheme != nullptr && crypto->GetPublicKey(signature.KeyId, publicKey))
        {
            const Span<const uint8> signedParts[] = {
                Span<const uint8>(reinterpret_cast<const uint8 *>(&header), sizeof(header)),
                AsBytes(pak->_blocks.AsSpan()),
                AsBytes(pak->_strings.AsSpan()),
                AsBytes(pak->_files.AsSpan()),
                AsBytes(pak->_assets.AsSpan()),
                AsBytes(pak->_dependencies.AsSpan()),
            };
            uint8 digest[PlatformCrypto::Sha256Size];
            ok = PakSigning::ComputeDigest(Span<const Span<const uint8>>(signedParts), signature, hashes.AsSpan(),
                                           digest) &&
                 scheme->Verify(publicKey.AsSpan(), digest,
                                Span<const uint8>(signature.Signature, signature.SignatureSize));
            pak->_signed = ok;
            pak->_blockHashes = Move(hashes);
        }
    }

    const StringView codec = FixedName(header.Codec, sizeof(header.Codec));
    pak->_codec = codec.IsEmpty() ? nullptr : codecs.Find(codec);
    ok = ok && (codec.IsEmpty() || pak->_codec != nullptr);
    const StringView cipher = FixedName(header.Cipher, sizeof(header.Cipher));
    if (ok && !cipher.IsEmpty())
    {
        // Each table is encrypted whole from its own position, so it decrypts before anything below reads it.
        pak->_cipher = crypto != nullptr ? crypto->FindCipher(cipher) : nullptr;
        ok = pak->_cipher != nullptr && crypto->GetKey(header.KeyId, pak->_key) &&
             pak->_key.Count() == pak->_cipher->GetKeySize();
        const auto decrypt = [&](auto &table, uint64 position) {
            return pak->_cipher->Apply(pak->_key.AsSpan(), header.Salt, position, AsWritableBytes(table.AsSpan()));
        };
        ok = ok && decrypt(pak->_blocks, PakBlockTablePosition) && decrypt(pak->_strings, PakStringTablePosition) &&
             decrypt(pak->_files, PakFileTablePosition) && decrypt(pak->_assets, PakAssetTablePosition) &&
             decrypt(pak->_dependencies, PakDependencyTablePosition);
    }
    // Stored blocks one after another, unencrypted and unsigned, are the logical stream itself.
    pak->_direct = ok && pak->_cipher == nullptr && !pak->_signed && !pak->_blocks.IsEmpty();
    pak->_directBase = pak->_direct ? pak->_blocks[0].Offset : 0;
    for (usize i = 0; ok && i < pak->_blocks.Count(); ++i)
    {
        const PakBlock &block = pak->_blocks[i];
        const uint64 start = uint64{i} * header.BlockSize;
        const uint64 logical =
            header.LogicalSize - start < header.BlockSize ? header.LogicalSize - start : header.BlockSize;
        ok = block.Offset <= size && block.StoredSize <= size - block.Offset &&
             (block.Stored != 0 ? block.StoredSize == logical : pak->_codec != nullptr);
        pak->_direct = pak->_direct && block.Stored != 0 && block.Offset == pak->_directBase + start;
    }
    const auto validString = [&](const PakString &text) {
        return text.Offset <= pak->_strings.Count() && text.Length <= pak->_strings.Count() - text.Offset;
    };
    for (const PakFileEntry &entry : pak->_files)
    {
        ok = ok && validString(entry.MountPoint) && validString(entry.Layer) && validString(entry.Path) &&
             entry.Offset <= header.LogicalSize && entry.Size <= header.LogicalSize - entry.Offset;
    }
    for (const PakAssetEntry &entry : pak->_assets)
    {
        ok = ok && entry.BlockOffset <= header.LogicalSize &&
             entry.BlockSize <= header.LogicalSize - entry.BlockOffset && entry.BulkOffset <= header.LogicalSize &&
             entry.BulkSize <= header.LogicalSize - entry.BulkOffset &&
             entry.DependencyIndex <= pak->_dependencies.Count() &&
             entry.DependencyCount <= pak->_dependencies.Count() - entry.DependencyIndex;
    }
    if (!ok)
    {
        return {};
    }
    const FileHandle async =
        PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting, FileFlags::Async);
    if (!async.IsValid())
    {
        return {};
    }
    pak->_file = SharedPtr<SharedFile>(new SharedFile(async, size));
    pak->_path = path;
    pak->_chunk = FixedName(header.Chunk, sizeof(header.Chunk));
    return pak;
}

StringView PakFile::GetString(const PakString &text) const
{
    return {_strings.Data() + text.Offset, text.Length};
}

const PakFileEntry *PakFile::FindFile(StringView mountPoint, StringView layer, StringView path) const
{
    usize low = 0;
    usize high = _files.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        const PakFileEntry &entry = _files[middle];
        int32 order = Compare(GetString(entry.MountPoint), mountPoint);
        order = order != 0 ? order : Compare(GetString(entry.Layer), layer);
        order = order != 0 ? order : Compare(GetString(entry.Path), path);
        if (order == 0)
        {
            return &entry;
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return nullptr;
}

bool PakFile::HasFilesIn(StringView mountPoint, StringView layer, StringView directory) const
{
    // The entries are sorted, so those under `directory/` follow the first that does not come before it.
    const auto under = [&](StringView path) {
        return directory.IsEmpty() ||
               (path.Length() > directory.Length() && path.StartsWith(directory) && path[directory.Length()] == '/');
    };
    // How *path* orders against `directory/`; every path follows the root's empty prefix.
    const auto order = [&](StringView path) -> int32 {
        if (directory.IsEmpty())
        {
            return 1;
        }
        const usize count = path.Length() < directory.Length() ? path.Length() : directory.Length();
        const int32 shared = count != 0 ? MemCompare(path.Data(), directory.Data(), count) : 0;
        if (shared != 0 || path.Length() <= directory.Length())
        {
            return shared != 0 ? shared : -1;
        }
        const uint8 next = static_cast<uint8>(path[directory.Length()]);
        return next != '/' ? (next < '/' ? -1 : 1) : (path.Length() == directory.Length() + 1 ? 0 : 1);
    };
    usize low = 0;
    usize high = _files.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        const PakFileEntry &entry = _files[middle];
        int32 compared = Compare(GetString(entry.MountPoint), mountPoint);
        compared = compared != 0 ? compared : Compare(GetString(entry.Layer), layer);
        compared = compared != 0 ? compared : order(GetString(entry.Path));
        if (compared < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    for (usize i = low; i < _files.Count(); ++i)
    {
        const PakFileEntry &entry = _files[i];
        if (GetString(entry.MountPoint) != mountPoint || GetString(entry.Layer) != layer ||
            !under(GetString(entry.Path)))
        {
            return false;
        }
        if ((entry.Flags & PakFileDeleted) == 0)
        {
            return true;
        }
    }
    return false;
}

const PakAssetEntry *PakFile::FindAsset(const Guid &uuid) const
{
    usize low = 0;
    usize high = _assets.Count();
    while (low < high)
    {
        const usize middle = (low + high) / 2;
        const int32 order = MemCompare(&_assets[middle].Uuid, &uuid, sizeof(Guid));
        if (order == 0)
        {
            return &_assets[middle];
        }
        if (order < 0)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return nullptr;
}

FileRegion PakFile::GetRegion(uint64 offset, uint64 size) const
{
    FileRegion region;
    region.File = _file;
    region.Size = size;
    if (_direct)
    {
        region.Offset = _directBase + offset;
        return region;
    }
    region.Offset = offset;
    region.Compression = SharedPtr<const IRegionCompression>(this);
    return region;
}

bool PakFile::GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const
{
    if (index >= _blocks.Count())
    {
        return false;
    }
    const PakBlock &block = _blocks[static_cast<usize>(index)];
    offset = block.Offset;
    storedSize = block.StoredSize;
    const uint64 start = index * _header.BlockSize;
    logicalSize = static_cast<uint32>(_header.LogicalSize - start < _header.BlockSize ? _header.LogicalSize - start
                                                                                      : _header.BlockSize);
    return true;
}

bool PakFile::Verify(uint64 index, Span<const uint8> stored) const
{
    if (!_signed)
    {
        return true;
    }
    uint8 hash[PlatformCrypto::Sha256Size];
    return PlatformCrypto::Sha256(stored, hash) &&
           MemCompare(hash, _blockHashes.Data() + index * PakBlockHashSize, PakBlockHashSize) == 0;
}

bool PakFile::DecompressInPlace(uint64 index, Span<uint8> stored, Span<uint8> output) const
{
    if (index >= _blocks.Count() || _blocks[static_cast<usize>(index)].Stored != 0 || _cipher == nullptr)
    {
        return Decompress(index, stored, output);
    }
    // The read owns its buffer, so a compressed, encrypted block decrypts where it lies instead of into a copy.
    return Verify(index, stored) && _cipher->Apply(_key.AsSpan(), _header.Salt, index, stored) &&
           _codec->Decompress(stored, output);
}

SharedPtr<const CachedRegionBlock> PakFile::FindCachedBlock(uint64 index) const
{
    return _cache.Find(index);
}

void PakFile::CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const
{
    _cache.Add(index, Move(block));
}

bool PakFile::Decompress(uint64 index, Span<const uint8> stored, Span<uint8> output) const
{
    if (index >= _blocks.Count())
    {
        return false;
    }
    const PakBlock &block = _blocks[static_cast<usize>(index)];
    if (!Verify(index, stored))
    {
        return false;
    }
    if (block.Stored != 0)
    {
        if (stored.Count() != output.Count())
        {
            return false;
        }
        MemCopy(output.Data(), stored.Data(), stored.Count());
        return _cipher == nullptr || _cipher->Apply(_key.AsSpan(), _header.Salt, index, output);
    }
    if (_cipher == nullptr)
    {
        return _codec->Decompress(stored, output);
    }
    // The stored bytes are the I/O thread's read buffer, which callers may not write, so they decrypt into a copy.
    List<uint8> &plain = DecryptScratch.Get();
    plain.ResizeUninitialized(stored.Count());
    MemCopy(plain.Data(), stored.Data(), stored.Count());
    if (!_cipher->Apply(_key.AsSpan(), _header.Salt, index, plain.AsSpan()))
    {
        return false;
    }
    return _codec->Decompress(plain.AsSpan(), output);
}

PakMount::PakMount(SharedPtr<const PakFile> pak, StringView mountPoint, StringView layer)
    : _pak(Move(pak)), _mountPoint(mountPoint), _layer(layer)
{
}

FileHandle PakMount::Open(StringView /*path*/, FileAccess /*access*/, FileMode /*mode*/, FileFlags /*flags*/)
{
    return {};
}

bool PakMount::GetInfo(StringView path, FileInfo &info)
{
    info = {};
    if (const PakFileEntry *entry = _pak->FindFile(_mountPoint.AsView(), _layer.AsView(), path))
    {
        if ((entry->Flags & PakFileDeleted) != 0)
        {
            return false;
        }
        info.Size = entry->Size;
        info.IsReadOnly = true;
        return true;
    }
    const bool directory = _pak->HasFilesIn(_mountPoint.AsView(), _layer.AsView(), path);
    info.IsDirectory = directory;
    info.IsReadOnly = true;
    return directory;
}

void PakMount::Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor)
{
    HashSet<String> directories;
    for (const PakFileEntry &entry : _pak->GetFiles())
    {
        if (_pak->GetString(entry.MountPoint) != _mountPoint.AsView() ||
            _pak->GetString(entry.Layer) != _layer.AsView() || (entry.Flags & PakFileDeleted) != 0)
        {
            continue;
        }
        StringView path = _pak->GetString(entry.Path);
        if (!directory.IsEmpty())
        {
            if (!path.StartsWith(directory) || path.Length() <= directory.Length() || path[directory.Length()] != '/')
            {
                continue;
            }
            path = path.RemoveStart(directory.Length() + 1);
        }
        StringView name;
        StringView rest;
        if (path.SplitFirst('/', name, rest))
        {
            if (directories.Add(String(name)))
            {
                FileInfo info;
                info.IsDirectory = true;
                info.IsReadOnly = true;
                visitor(name, info);
            }
            continue;
        }
        FileInfo info;
        info.Size = entry.Size;
        info.IsReadOnly = true;
        visitor(path, info);
    }
}

bool PakMount::IsDeleted(StringView path) const
{
    const PakFileEntry *entry = _pak->FindFile(_mountPoint.AsView(), _layer.AsView(), path);
    return entry != nullptr && (entry->Flags & PakFileDeleted) != 0;
}

bool PakMount::OpenRegion(StringView path, FileRegion &region)
{
    const PakFileEntry *entry = _pak->FindFile(_mountPoint.AsView(), _layer.AsView(), path);
    if (entry == nullptr || (entry->Flags & PakFileDeleted) != 0)
    {
        return false;
    }
    region = _pak->GetRegion(entry->Offset, entry->Size);
    return true;
}
} // namespace Marlin
