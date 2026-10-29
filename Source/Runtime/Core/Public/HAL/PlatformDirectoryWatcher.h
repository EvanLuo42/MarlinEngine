//! Directory change notifications from the OS, with an explicit signal when events are lost.
//!
//! A watcher reports adds, removes, modifications and renames below one directory, recursively when asked. The OS can
//! overflow its buffer in a burst of changes; that arrives as `FileChangeKind::Overflow` and the caller must rescan.

#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// What happened to a path in a watched directory.
enum class FileChangeKind : uint8
{
    /// The path was created.
    Added,
    /// The path was removed.
    Removed,
    /// The path was modified.
    Modified,
    /// The path is the old name of a rename.
    RenamedFrom,
    /// The path is the new name of a rename.
    RenamedTo,
    /// Changes were lost; rescan the directory.
    Overflow,
};

/// One observed change.
struct FileChange
{
    /// The size of the `Path` buffer in bytes.
    static constexpr usize MaxPathLength = 1024;

    /// The kind of change.
    FileChangeKind Kind = FileChangeKind::Modified;
    /// Relative to the watched directory, with '/' separators; empty for `Overflow`.
    char Path[MaxPathLength] = {};
};

/// Watches a directory and reports changes below it.
///
/// `Wait` collects a batch of notifications and `Next` returns its entries one at a time. Changes to names, sizes,
/// last-write times and creation are reported; other metadata changes are not. A rename arrives as a `RenamedFrom`
/// entry and a `RenamedTo` entry.
///
/// # Thread safety
/// A watcher is used by one thread at a time; it has no internal synchronization.
///
/// # Examples
///
/// ```cpp
/// DirectoryWatcher watcher;
/// if (watcher.Open("Saved/Logs", true))
/// {
///     FileChange change;
///     while (watcher.Wait(1000)) // -> true when a batch is ready.
///     {
///         while (watcher.Next(change)) // -> false once the batch is exhausted.
///         {
///             if (change.Kind == FileChangeKind::Overflow)
///             {
///                 // Changes were lost; rescan the directory.
///             }
///         }
///     }
/// }
/// ```
class CORE_API DirectoryWatcher
{
public:
    /// Creates a watcher with no directory open.
    DirectoryWatcher() = default;
    /// Stops watching and releases the handles.
    ~DirectoryWatcher();
    MARLIN_NONCOPYABLE(DirectoryWatcher);

    /// Starts watching `path`, including its subdirectories when `recursive` is true.
    ///
    /// Closes any directory already open.
    ///
    /// # Errors
    /// Returns false when the directory cannot be opened or watched.
    bool Open(const char *path, bool recursive);
    /// Stops watching and releases the handles.
    void Close();

    /// Waits up to `timeout` milliseconds and returns true once changes are ready for `Next`.
    ///
    /// Returns true at once while entries of the current batch remain, and false on timeout or when no directory is
    /// open.
    bool Wait(uint32 timeout);
    /// Writes the next change to `change` and returns true, or returns false when no batch is ready.
    ///
    /// `Overflow` means the OS lost changes; rescan the directory. After the last entry of a batch, the next call
    /// returns false until `Wait` collects a new batch.
    bool Next(FileChange &change);

private:
    void *_state = nullptr;
};
} // namespace Marlin
