//! File and directory I/O: handles, metadata, paths, memory mapping and directory iteration.
//!
//! Paths are UTF-8 and may use `/` or the native separator; returned paths use `/`. Functions that fill a buffer
//! return the UTF-8 length without its terminator and write the terminated string only when the length is less than
//! the capacity.

#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"

namespace Marlin
{
/// The access rights a file is opened with.
enum class FileAccess : uint8
{
    /// Opens for reading only.
    Read,
    /// Opens for writing only.
    Write,
    /// Opens for reading and writing.
    ReadWrite,
};

/// How `PlatformFile::Open` treats an existing file.
enum class FileMode : uint8
{
    /// Fails unless the file exists.
    OpenExisting,
    /// Opens the file when it exists; creates it otherwise.
    OpenOrCreate,
    /// Creates the file, truncating an existing one.
    CreateAlways,
    /// Fails if the file exists.
    CreateNew,
};

/// Options for `PlatformFile::Open`, combined with `|`.
enum class FileFlags : uint8
{
    /// No flags.
    None = 0,
    /// Opens the file for overlapped I/O; required by `AsyncIoQueue` and `IoPort`.
    ///
    /// The synchronous read and write functions cannot use such a handle.
    Async = 1 << 0,
    /// No other handle may open the file until this one closes.
    Exclusive = 1 << 1,
    /// Deletes the file when the last handle to it closes.
    DeleteOnClose = 1 << 2,
    /// Writes append to the end of the file, whatever the position.
    Append = 1 << 3,
    /// Tunes the OS file cache for sequential scanning.
    SequentialScan = 1 << 4,
    /// Tunes the OS file cache for random access.
    RandomAccess = 1 << 5,
};
MARLIN_ENUM_FLAGS(FileFlags)

/// A native file handle; a zero `Value` is invalid.
///
/// # Thread safety
/// A handle may be used from several threads. `ReadAt` and `WriteAt` are safe to call concurrently; `Read`, `Write`
/// and `Seek` use the shared file position and must be serialized. Do not close a handle while another thread uses
/// it.
struct FileHandle
{
    /// The native handle value; zero when invalid.
    uintptr Value = 0;

    /// Returns true when the handle refers to an open file.
    bool IsValid() const
    {
        return Value != 0;
    }
};

/// Metadata of a file or directory, as returned by `PlatformFile::GetInfo`.
struct FileInfo
{
    /// The file size in bytes.
    uint64 Size = 0;
    /// DateTime ticks: 100 ns intervals since 0001-01-01 UTC.
    int64 ModifiedTime = 0;
    /// True when the entry is a directory.
    bool IsDirectory = false;
    /// True when the read-only attribute is set.
    bool IsReadOnly = false;
};

/// A mapped view of a file, as returned by `PlatformFile::Map`.
struct MappedRegion
{
    /// The first byte of the view.
    void *Data = nullptr;
    /// The view size in bytes.
    usize Size = 0;
    /// The platform mapping object; `PlatformFile::Unmap` closes it.
    uintptr Mapping = 0;
};

namespace PlatformFile
{
/// Opens the file at `path`, creating it as `mode` requires.
///
/// A relative path resolves against the process's current directory.
///
/// # Errors
/// Returns an invalid handle when the path is invalid or the OS cannot open or create the file.
CORE_API FileHandle Open(const char *path, FileAccess access, FileMode mode, FileFlags flags = FileFlags::None);
/// Closes `file`; does nothing for an invalid handle.
CORE_API void Close(FileHandle file);

/// Reads `size` bytes at the current position, or fewer when the end of the file comes first.
///
/// `bytesRead` receives the number read; it is zero when the position is already at the end of the file.
///
/// # Preconditions
/// `buffer` points to at least `size` writable bytes.
///
/// # Errors
/// Returns false when the OS read fails; `bytesRead` then holds the bytes read before the failure.
CORE_API bool Read(FileHandle file, void *buffer, usize size, usize &bytesRead);
/// Writes `size` bytes at the current position, continuing until all are written.
///
/// # Errors
/// Returns false when the OS write fails; bytes may already have been written.
CORE_API bool Write(FileHandle file, const void *data, usize size);
/// Reads `size` bytes at `offset`, independent of the current position.
///
/// Safe to call from several threads at once. Reading at or past the end of the file succeeds with `bytesRead` zero.
///
/// # Errors
/// Returns false when the OS read fails; reaching the end of the file is not a failure.
CORE_API bool ReadAt(FileHandle file, uint64 offset, void *buffer, usize size, usize &bytesRead);
/// Writes `size` bytes at `offset`, independent of the current position.
///
/// Safe to call from several threads at once.
///
/// # Errors
/// Returns false when the OS write fails; bytes may already have been written.
CORE_API bool WriteAt(FileHandle file, uint64 offset, const void *data, usize size);

/// Moves the current position to `position` bytes from the start of the file.
///
/// # Errors
/// Returns false when the position cannot be set.
CORE_API bool Seek(FileHandle file, uint64 position);
/// Writes the file's size in bytes to `size`.
///
/// # Errors
/// Returns false when the size cannot be read.
CORE_API bool GetSize(FileHandle file, uint64 &size);
/// Resizes the file to `size` bytes.
///
/// # Errors
/// Returns false when the size cannot be changed.
CORE_API bool SetSize(FileHandle file, uint64 size);
/// Flushes the file's buffered writes to the storage device.
///
/// # Errors
/// Returns false when the flush fails.
CORE_API bool Flush(FileHandle file);

/// Maps `size` bytes starting at `offset` into memory.
///
/// `offset` must be a multiple of the allocation granularity; a `size` of zero maps from `offset` to the end of the
/// file. A `FileAccess::Read` mapping is read-only; `FileAccess::Write` and `FileAccess::ReadWrite` map it writable.
///
/// # Errors
/// Returns false and clears `region` when no view can be created.
///
/// # Examples
///
/// ```cpp
/// MappedRegion region;
/// if (PlatformFile::Map(file, FileAccess::Read, 0, 0, region))
/// {
///     const usize size = region.Size; // -> the mapped length.
///     PlatformFile::Unmap(region);
/// }
/// ```
CORE_API bool Map(FileHandle file, FileAccess access, uint64 offset, usize size, MappedRegion &region);
/// Unmaps a region returned by `Map` and clears it.
///
/// Does nothing for a region whose `Data` is null.
CORE_API void Unmap(MappedRegion &region);

/// Writes the metadata of `path` to `info`.
///
/// # Errors
/// Returns false when nothing exists at `path` or its metadata cannot be read.
CORE_API bool GetInfo(const char *path, FileInfo &info);
/// Returns true when `path` exists and is not a directory.
CORE_API bool FileExists(const char *path);
/// Returns true when `path` exists and is a directory.
CORE_API bool DirectoryExists(const char *path);

/// Deletes the file at `path`.
///
/// # Errors
/// Returns false when the file does not exist, is a directory, or cannot be deleted.
CORE_API bool Delete(const char *path);
/// Moves or renames `from` to `to`, replacing an existing `to` when `overwrite` is true.
///
/// # Errors
/// Returns false when the move fails or `to` exists and `overwrite` is false.
CORE_API bool Move(const char *from, const char *to, bool overwrite);
/// Copies the file at `from` to `to`, replacing an existing `to` when `overwrite` is true.
///
/// # Errors
/// Returns false when the copy fails or `to` exists and `overwrite` is false.
CORE_API bool Copy(const char *from, const char *to, bool overwrite);
/// Sets or clears the read-only attribute of `path`.
///
/// # Errors
/// Returns false when the attribute cannot be changed.
CORE_API bool SetReadOnly(const char *path, bool readOnly);
/// Sets the last-write time of `path` from DateTime ticks.
///
/// `time` counts 100 ns intervals since 0001-01-01 UTC.
///
/// # Errors
/// Returns false when the time cannot be set.
CORE_API bool SetModifiedTime(const char *path, int64 time);

/// Creates `path` and any missing parents.
///
/// Returns true when the directory already exists.
///
/// # Errors
/// Returns false when a path component exists as a file or the directory cannot be created.
CORE_API bool CreateDirectory(const char *path);
/// Deletes the directory at `path`.
///
/// A non-recursive delete fails unless the directory is empty. A recursive delete removes a directory link without
/// following it, and refuses a path that `IsSafeTreeRoot` rejects.
///
/// # Preconditions
/// A recursive delete requires a full path: a drive letter, a UNC path or a `\\?\` path. A relative path or an unsafe
/// root asserts; in builds with assertions that is fatal, and without them the call returns false.
///
/// # Errors
/// Returns false when the directory is not empty (non-recursive), is missing, is a file, or is refused.
CORE_API bool DeleteDirectory(const char *path, bool recursive);
/// Returns false for a path a recursive delete must never take.
///
/// False for an empty path, a volume root or a top-level folder; the executable's directory, the current directory, a
/// KnownFolder, a user profile's root or one of its direct children; or any parent of those. Links are resolved first,
/// so a path that reaches one of them through a link is refused too. A path that does not exist is judged by its name.
CORE_API bool IsSafeTreeRoot(const char *path);

/// Writes the absolute form of `path` to `buffer` and returns its UTF-8 length without terminator.
///
/// The path uses `/` separators. The terminated string is written only when the length is less than `capacity`.
///
/// # Errors
/// Returns zero when the path cannot be resolved.
CORE_API usize GetFullPath(const char *path, char *buffer, usize capacity);
/// Writes the current directory to `buffer` and returns its UTF-8 length without terminator.
///
/// The path uses `/` separators. The terminated string is written only when the length is less than `capacity`.
///
/// # Errors
/// Returns zero when the current directory cannot be read.
CORE_API usize GetCurrentDirectory(char *buffer, usize capacity);
} // namespace PlatformFile

/// One entry from a directory listing.
struct DirectoryEntry
{
    /// The size of the `Name` buffer in bytes.
    static constexpr usize MaxNameLength = 1024;

    /// The entry's name without its directory, UTF-8 and terminated.
    char Name[MaxNameLength] = {};
    /// The entry's metadata.
    FileInfo Info;
};

/// Lists a directory without `.` and `..`, in no particular order.
///
/// `Open` starts a listing, `Next` returns one entry at a time, and `Close` releases it. The iterator is not copyable
/// or movable, so keep it in place while it is open.
///
/// # Thread safety
/// An iterator is used by one thread at a time; it has no internal synchronization.
///
/// # Examples
///
/// ```cpp
/// DirectoryIterator iterator;
/// if (iterator.Open("Saved/Logs"))
/// {
///     DirectoryEntry entry;
///     while (iterator.Next(entry))
///     {
///         const char *name = entry.Name; // -> the entry's name, without its directory.
///     }
/// }
/// ```
class CORE_API DirectoryIterator
{
public:
    /// Creates an iterator with no listing open.
    DirectoryIterator() = default;
    /// Closes the listing.
    ~DirectoryIterator();
    MARLIN_NONCOPYABLE(DirectoryIterator);

    /// Starts listing `path`, closing any listing already open.
    ///
    /// # Errors
    /// Returns false when the directory cannot be listed. An empty directory opens successfully with no entries.
    bool Open(const char *path);
    /// Writes the next entry to `entry` and returns true, or returns false when the listing is exhausted.
    ///
    /// `entry` is overwritten only when an entry is returned.
    bool Next(DirectoryEntry &entry);
    /// Ends the listing and releases the OS handle.
    void Close();

private:
    bool Advance();

    void *_handle = nullptr;
    bool _hasEntry = false;
    DirectoryEntry _entry;
};
} // namespace Marlin
