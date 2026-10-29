#pragma once

#include "Compression/CompressionCodec.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Vfs/PakCipher.h"
#include "Vfs/PakFormat.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
/// An open package (PakFormat.h): its tables in memory; its regions are read through Vfs::ReadAsync, which
/// decompresses block by block. Mounts and asset stores share it; immutable once open.
class VFS_API PakFile final : public IRegionCompression
{
public:
    /// Opens native path *path*; null if it is not a valid package of this version, a block's codec is not in
    /// *codecs*, or it is encrypted and *crypto* lacks its cipher or key. A package with a .msig beside it whose
    /// scheme and public key *crypto* has is verified: null if the signature does not hold, and each block read after
    /// is checked against its hash. Without that it opens unsigned; whether to mount it is the caller's call.
    [[nodiscard]] static SharedPtr<PakFile> Open(StringView path, const CompressionCodecSet &codecs,
                                                 const PakCrypto *crypto = nullptr);

    [[nodiscard]] bool IsEncrypted() const
    {
        return _cipher != nullptr;
    }

    /// Whether the signature was verified, so every block is checked as it is read.
    [[nodiscard]] bool IsSigned() const
    {
        return _signed;
    }

    [[nodiscard]] StringView GetChunk() const
    {
        return _chunk.AsView();
    }

    [[nodiscard]] StringView GetPath() const
    {
        return _path.AsView();
    }

    [[nodiscard]] Span<const PakFileEntry> GetFiles() const
    {
        return _files.AsSpan();
    }

    [[nodiscard]] Span<const PakAssetEntry> GetAssets() const
    {
        return _assets.AsSpan();
    }

    /// The Guids the block of *entry* references, in the order of the block's own list.
    [[nodiscard]] Span<const Guid> GetDependencies(const PakAssetEntry &entry) const
    {
        return _dependencies.AsSpan().Slice(entry.DependencyIndex, entry.DependencyCount);
    }

    /// True if every block is stored as it is, one after another, and neither encrypted nor signed: regions are
    /// then plain parts of the file, read at their exact offset without going block by block.
    [[nodiscard]] bool ReadsDirectly() const
    {
        return _direct;
    }

    [[nodiscard]] StringView GetString(const PakString &text) const;
    /// The entry of *path* in *layer* ("" for the root layer) under *mountPoint*, or null; deleted entries too, which
    /// callers check with PakFileDeleted.
    [[nodiscard]] const PakFileEntry *FindFile(StringView mountPoint, StringView layer, StringView path) const;
    /// Whether a file that is not deleted lies under *directory* ("" for the root) in *layer* under *mountPoint*: a
    /// binary search over the sorted entries, then a look at those under it until one is not deleted.
    [[nodiscard]] bool HasFilesIn(StringView mountPoint, StringView layer, StringView directory) const;
    /// The entry of object *uuid*, or null; a zero BlockSize means a patch removed it.
    [[nodiscard]] const PakAssetEntry *FindAsset(const Guid &uuid) const;
    /// The bytes at *offset* in the logical stream, for Vfs::ReadAsync.
    [[nodiscard]] FileRegion GetRegion(uint64 offset, uint64 size) const;

    [[nodiscard]] uint32 GetBlockSize() const override
    {
        return _header.BlockSize;
    }
    bool GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const override;
    [[nodiscard]] bool IsPlain(uint64 /*index*/) const override
    {
        return _cipher == nullptr && !_signed;
    }
    bool Decompress(uint64 index, Span<const uint8> stored, Span<uint8> output) const override;
    bool DecompressInPlace(uint64 index, Span<uint8> stored, Span<uint8> output) const override;
    [[nodiscard]] SharedPtr<const CachedRegionBlock> FindCachedBlock(uint64 index) const override;
    void CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const override;

private:
    PakFile() = default;

    bool Verify(uint64 index, Span<const uint8> stored) const;

    PakHeader _header;
    String _path;
    String _chunk;
    SharedPtr<SharedFile> _file;
    const CompressionCodec *_codec = nullptr;
    const PakCipher *_cipher = nullptr;
    List<uint8> _key;
    bool _signed = false;
    /// PakBlockHashSize bytes per block, from the verified .msig.
    List<uint8> _blockHashes;
    List<PakBlock> _blocks;
    List<char> _strings;
    List<PakFileEntry> _files;
    List<PakAssetEntry> _assets;
    List<Guid> _dependencies;
    /// ReadsDirectly, and where logical offset zero lies in the file when it does.
    bool _direct = false;
    uint64 _directBase = 0;
    /// Blocks partial reads decompressed and verified.
    mutable RegionBlockCache _cache;
};

/// One mount point and layer of a package, read-only (02 §5.3). Its files have no native
/// handle; reads go through OpenRegion, and Vfs::ReadAll does that for them.
class VFS_API PakMount final : public IFileMount
{
public:
    PakMount(SharedPtr<const PakFile> pak, StringView mountPoint, StringView layer);

    [[nodiscard]] FileHandle Open(StringView path, FileAccess access, FileMode mode, FileFlags flags) override;
    bool GetInfo(StringView path, FileInfo &info) override;
    void Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor) override;
    [[nodiscard]] bool IsWritable() const override
    {
        return false;
    }
    [[nodiscard]] bool IsDeleted(StringView path) const override;
    bool OpenRegion(StringView path, FileRegion &region) override;

private:
    SharedPtr<const PakFile> _pak;
    String _mountPoint;
    String _layer;
};
} // namespace Marlin
