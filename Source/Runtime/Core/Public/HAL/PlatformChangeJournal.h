//! A volume's change journal, which records file changes even while no process is watching.
//!
//! On Windows this is the NTFS USN journal. The journal can be recreated or wrap, so a cursor is valid only for the
//! journal it came from; `Read` says when it can no longer continue. File systems without a journal make `Open` fail,
//! in which case callers fall back to directory watching and a rescan.

#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"

namespace Marlin
{
/// What happened to a file, as the volume journal records it; several at once are common.
enum class JournalReason : uint32
{
    /// No reason this enum maps; a record can still be visited, for example a close-only record.
    None = 0,
    /// The file or directory was created.
    Created = 1 << 0,
    /// The file or directory was deleted.
    Deleted = 1 << 1,
    /// The contents or size changed.
    Modified = 1 << 2,
    /// The record carries the old name of a rename.
    RenamedFrom = 1 << 3,
    /// The record carries the new name of a rename.
    RenamedTo = 1 << 4,
    /// Attributes, times or security changed without a content change.
    Metadata = 1 << 5,
};
MARLIN_ENUM_FLAGS(JournalReason)

/// A point in a volume's journal, valid only for the journal it came from: a recreated journal has a new `JournalId`.
struct JournalCursor
{
    /// The journal's identity.
    uint64 JournalId = 0;
    /// The position in the journal.
    int64 Position = 0;
};

/// One record from a volume's journal.
struct JournalChange
{
    /// The size of the `Name` buffer in bytes.
    static constexpr usize MaxNameLength = 512;

    /// The file or directory's journal id, usable with `GetPath`.
    uint64 FileId = 0;
    /// The id of the directory that contained the entry.
    uint64 ParentId = 0;
    /// What happened; several reasons can be set at once.
    JournalReason Reasons = JournalReason::None;
    /// True when the entry is a directory.
    bool IsDirectory = false;
    /// UTF-8, without the directory; empty when the journal does not report names.
    char Name[MaxNameLength] = {};
};

/// A volume's change journal (the NTFS USN journal on Windows): every change to every file on the volume, kept by the
/// file system even while no process watches, so an indexer can catch up on what happened while it was not running.
/// Unavailable on file systems without one; callers fall back to `DirectoryWatcher` and a rescan.
///
/// The Windows implementation reads the journal unprivileged, which reports no file names: `JournalChange::Name` is
/// empty, so resolve entries through `JournalChange::FileId` and `GetPath`. A journal can be recreated or wrap;
/// `Read` then fails and the caller must rescan.
///
/// # Thread safety
/// A journal is used by one thread at a time; it has no internal synchronization. `Read` stages records in a
/// thread-local buffer, so separate journals on separate threads do not share it.
///
/// # Examples
///
/// ```cpp
/// ChangeJournal journal;
/// if (journal.Open("C:/"))
/// {
///     JournalCursor cursor;
///     if (journal.GetEnd(cursor))
///     {
///         const bool current = journal.Read(
///             cursor, [](const JournalChange &change, void *) { /* record change */ }, nullptr);
///         // -> true while the journal still reaches back to the cursor.
///     }
/// }
/// ```
class CORE_API ChangeJournal
{
public:
    /// Creates a journal that is not open.
    ChangeJournal() = default;
    /// Closes the journal.
    ~ChangeJournal();
    MARLIN_NONCOPYABLE(ChangeJournal);

    /// Opens the journal of the volume that holds `directory`, closing any journal already open.
    ///
    /// # Errors
    /// Returns false when the directory cannot be opened or the volume keeps no journal.
    bool Open(const char *directory);
    /// Closes the journal, if one is open.
    void Close();
    /// Returns true when a journal is open.
    [[nodiscard]] bool IsOpen() const
    {
        return _handle != 0;
    }

    /// Writes the journal's current end, the cursor to start following it from.
    ///
    /// # Errors
    /// Returns false when no journal is open or the query fails.
    [[nodiscard]] bool GetEnd(JournalCursor &cursor) const;

    /// Visits the changes after `cursor` and advances it, one batch at a time.
    ///
    /// Changes are visited in journal order on the calling thread. A batch is always visited in full, so `visitor` can
    /// be called more than `maxChanges` times in one call; the limit is checked between batches, and the cursor
    /// advances to the end of the last batch read. The default covers one batch.
    ///
    /// # Preconditions
    /// `visitor` is not null; this is not checked.
    ///
    /// # Errors
    /// Returns false when no journal is open, the query fails, or the journal no longer reaches back to `cursor`
    /// (it was recreated or wrapped). The caller must then rescan and start a new cursor.
    ///
    /// # Performance
    /// Reads records into a 256 KiB thread-local buffer; it does not allocate.
    bool Read(JournalCursor &cursor, void (*visitor)(const JournalChange &change, void *context), void *context,
              uint32 maxChanges = 65536);

    /// Writes the full path of a file or directory by its id, with '/' separators.
    ///
    /// Returns the UTF-8 length without terminator and writes the terminated path only when the length is less than
    /// `capacity`; zero when the journal is closed or the id no longer exists.
    usize GetPath(uint64 fileId, char *buffer, usize capacity) const;

private:
    uintptr _handle = 0;
};
} // namespace Marlin
