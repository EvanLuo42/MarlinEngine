#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformSync.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
class StringWriter;

/// A file opened with FileFlags::Async, closed when the last reference goes; reads in flight hold one.
class VFS_API SharedFile final : public RefCounted
{
public:
    SharedFile(FileHandle file, uint64 size) : File(file), Size(size)
    {
    }
    ~SharedFile() override;
    MARLIN_NONCOPYABLE(SharedFile);

    FileHandle File;
    uint64 Size;
};

/// The logical bytes of one block, decompressed and verified, kept by an IRegionCompression for reads that want only a
/// part of it.
struct CachedRegionBlock final : RefCounted
{
    List<uint8> Bytes;
};

/// A few blocks an IRegionCompression keeps for FindCachedBlock and CacheBlock, the least recently used dropped first:
/// neighbouring small objects share blocks, so each read of one would otherwise decompress them again.
class VFS_API RegionBlockCache
{
public:
    /// A few MiB at the default 64 KiB blocks.
    static constexpr usize DefaultCapacity = 32;

    explicit RegionBlockCache(usize capacity = DefaultCapacity) : _capacity(capacity)
    {
    }
    MARLIN_NONCOPYABLE(RegionBlockCache);

    [[nodiscard]] SharedPtr<const CachedRegionBlock> Find(uint64 index);
    void Add(uint64 index, SharedPtr<const CachedRegionBlock> block);

private:
    struct Entry
    {
        uint64 Index = 0;
        uint64 LastUse = 0;
        SharedPtr<const CachedRegionBlock> Block;
    };

    usize _capacity;
    Mutex _lock;
    List<Entry> _entries;
    uint64 _clock = 0;
};

/// Bytes that are not in a file, such as a value in GlobalStore (02 §5.5). A FileRegion with a source has no File:
/// ReadAsync hands the read to the source, and Compression applies as for a file.
class VFS_API IRegionSource : public RefCounted
{
public:
    /// Reads *size* bytes at *offset* into *buffer*, which stays valid until *done* runs. Calls *done* exactly once,
    /// on an I/O thread or at once, with whether every byte arrived.
    virtual void Read(uint64 offset, uint64 size, uint8 *buffer, Function<void(bool succeeded)> done) const = 0;
};

/// How a FileRegion's bytes are compressed, as in a package (Documentation/Architecture/06 §5): the logical bytes are
/// split into blocks, each compressed on its own and stored one after another. Safe to share between threads.
class VFS_API IRegionCompression : public RefCounted
{
public:
    /// The block *index* if a read kept it, else null. Reads look here for the blocks they need only partly, which
    /// neighbouring small objects in a package share.
    [[nodiscard]] virtual SharedPtr<const CachedRegionBlock> FindCachedBlock(uint64 index) const
    {
        (void)index;
        return {};
    }
    /// Keeps *block*, block *index* as decompressed, for FindCachedBlock; the compression may drop it at any time.
    virtual void CacheBlock(uint64 index, SharedPtr<const CachedRegionBlock> block) const
    {
        (void)index;
        (void)block;
    }

    /// Logical bytes per block; every block but the last of the stream holds this many.
    [[nodiscard]] virtual uint32 GetBlockSize() const = 0;
    /// Where logical block *index* is stored, its stored size and its logical size; a stored size equal to the logical
    /// one means the block is not compressed.
    virtual bool GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const = 0;
    /// True if the stored bytes of an uncompressed block are the logical bytes, so reads copy them without calling
    /// Decompress; false for encrypted data.
    [[nodiscard]] virtual bool IsPlain(uint64 index) const
    {
        (void)index;
        return true;
    }
    /// Decompresses block *index* from its stored bytes into *output*, its logical size.
    virtual bool Decompress(uint64 index, Span<const uint8> stored, Span<uint8> output) const = 0;
    /// Like Decompress, but may overwrite *stored*, as decrypting in place does; reads use it on their own buffer.
    virtual bool DecompressInPlace(uint64 index, Span<uint8> stored, Span<uint8> output) const
    {
        return Decompress(index, stored, output);
    }
};

/// Bytes that an asynchronous read fetches: a whole file, or a part of one such as a file in a package, or the bytes
/// of a Source instead of a File. With Compression, Offset and Size are in the logical stream it describes; reads
/// decompress after reading.
struct FileRegion
{
    SharedPtr<SharedFile> File;
    uint64 Offset = 0;
    uint64 Size = 0;
    SharedPtr<const IRegionCompression> Compression;
    /// Set instead of File for bytes that are not in a file.
    SharedPtr<const IRegionSource> Source;

    [[nodiscard]] bool IsValid() const
    {
        return File.IsValid() || Source.IsValid();
    }
};

/// What a mount point serves: files at paths relative to it, '/'-separated, without a leading '/'.
class VFS_API IFileMount
{
public:
    IFileMount() = default;
    virtual ~IFileMount() = default;
    MARLIN_NONCOPYABLE(IFileMount);

    [[nodiscard]] virtual FileHandle Open(StringView path, FileAccess access, FileMode mode, FileFlags flags) = 0;
    virtual bool GetInfo(StringView path, FileInfo &info) = 0;
    /// Visits the entries of *directory*, "" for the mount's root.
    virtual void Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor) = 0;
    [[nodiscard]] virtual bool IsWritable() const = 0;
    /// True if the mount hides *path* from the mounts below it, as a patch package does for a file the release removed.
    [[nodiscard]] virtual bool IsDeleted(StringView path) const
    {
        (void)path;
        return false;
    }
    virtual bool Delete(StringView path);
    virtual bool CreateDirectory(StringView path);
    /// Writes the native path of *path* if the mount maps onto the file system.
    virtual bool GetPhysicalPath(StringView path, StringWriter &output) const;
    /// Resolves *path* to the bytes an asynchronous read fetches; mounts translate, the I/O thread reads.
    virtual bool OpenRegion(StringView path, FileRegion &region);
};

/// A directory on disk.
class VFS_API DirectoryMount final : public IFileMount
{
public:
    DirectoryMount(StringView root, bool writable);

    [[nodiscard]] FileHandle Open(StringView path, FileAccess access, FileMode mode, FileFlags flags) override;
    bool GetInfo(StringView path, FileInfo &info) override;
    void Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor) override;

    [[nodiscard]] bool IsWritable() const override
    {
        return _writable;
    }

    bool Delete(StringView path) override;
    bool CreateDirectory(StringView path) override;
    bool GetPhysicalPath(StringView path, StringWriter &output) const override;
    bool OpenRegion(StringView path, FileRegion &region) override;

private:
    /// Files of a read-only mount stay open for later asynchronous reads; a writable mount opens each read anew.
    static constexpr usize MaxOpenFiles = 256;

    String _root;
    bool _writable;
    Mutex _filesLock;
    HashMap<String, SharedPtr<SharedFile>> _files;
};

using MountId = uint32;

/// Engine code addresses files by virtual path: /Engine, /Project, /User, /Cache, /Temp and any
/// other point something mounts. The longest point that contains a path owns it, as with Unix mounts. A point can
/// hold several mounts, such as a folder and its platform layers: reads take the file from the highest priority one
/// that has it, and writes go to the highest writable one.
namespace Vfs
{
/// Mounts *mount* at *point*, such as "/Engine" or "/Project"; higher priorities are searched first.
VFS_API MountId Mount(StringView point, UniquePtr<IFileMount> mount, int32 priority = 0);
VFS_API bool Unmount(MountId id);
VFS_API void UnmountAll();
[[nodiscard]] VFS_API bool IsMounted(StringView point);

/// Reading opens the file of the first mount that has it; writing uses the first writable mount and creates the
/// parent directories. Invalid on failure, including a path that is not absolute or climbs with "..".
[[nodiscard]] VFS_API FileHandle Open(StringView path, FileAccess access, FileMode mode = FileMode::OpenExisting,
                                      FileFlags flags = FileFlags::None);
[[nodiscard]] VFS_API bool Exists(StringView path);
VFS_API bool GetInfo(StringView path, FileInfo &info);
VFS_API bool ReadAll(StringView path, String &text);
VFS_API bool ReadAll(StringView path, List<uint8> &bytes);
VFS_API bool WriteAll(StringView path, StringView text);
/// Deletes a file, or a directory and everything below it; a mount point itself is never deleted.
VFS_API bool Delete(StringView path);
VFS_API bool CreateDirectory(StringView path);

/// Visits each entry of *directory* once, with the info from the mount that wins it.
VFS_API void Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor);

/// Visits the contents of *path* in every mount that has it, lowest priority first, with a description of where it
/// came from; this is how layered files such as config merge.
VFS_API void ReadEachLayer(StringView path, FunctionRef<void(StringView text, StringView source)> visitor);

/// Resolves *path* through the first mount that has it, for an asynchronous read.
VFS_API bool OpenRegion(StringView path, FileRegion &region);

/// Writes the native path the file would be read from, or written to if it does not exist.
VFS_API bool GetPhysicalPath(StringView path, StringWriter &output);

/// The layers of the point that owns *path*, highest priority first: every mount a read of it searches.
VFS_API void GetLayers(StringView path, List<MountId> &layers);
/// The layer a read of *path* takes it from; 0 if no layer has it.
[[nodiscard]] VFS_API MountId FindLayer(StringView path);

/// Like the functions above, but only in *layer*, one of GetLayers(path), so that a file can be written beside the
/// ones it belongs with instead of into the highest writable layer. Writing creates the parent directories.
[[nodiscard]] VFS_API FileHandle Open(MountId layer, StringView path, FileAccess access,
                                      FileMode mode = FileMode::OpenExisting, FileFlags flags = FileFlags::None);
VFS_API bool GetInfo(MountId layer, StringView path, FileInfo &info);
VFS_API bool ReadAll(MountId layer, StringView path, String &text);
VFS_API bool WriteAll(MountId layer, StringView path, StringView text);
VFS_API bool GetPhysicalPath(MountId layer, StringView path, StringWriter &output);

/// Writes *path* with '\' turned to '/', repeated and trailing '/' removed and "." segments dropped. False if it is
/// not absolute or has a ".." segment.
VFS_API bool Normalize(StringView path, StringWriter &output);
} // namespace Vfs
} // namespace Marlin
