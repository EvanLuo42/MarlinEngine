#pragma once

#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformChangeJournal.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// Follows the volume journals for the indexer's folders (Documentation/Architecture/06 §2): turns journal records
/// into the virtual paths that changed, and keeps cursors across restarts so the changes made while the indexer was
/// down are replayed instead of rescanning every file. Unprivileged journal reads carry no file names, so a file is
/// found by its id while it exists, and a deleted one is reported as its folder, which the scanner then rescans.
class ASSETPIPELINE_API AssetJournal
{
public:
    AssetJournal() = default;
    ~AssetJournal();
    MARLIN_NONCOPYABLE(AssetJournal);

    /// *folders* are virtual folders; false unless every one of them is on a volume with a journal, in which case
    /// the caller watches with DirectoryWatcher instead.
    bool Open(Span<const String> folders);

    /// Moves every cursor to the end of its journal.
    void MarkEnd();
    /// Restores cursors written by Save; false if there are none for these volumes.
    bool Load(StringView path);
    bool Save(StringView path) const;

    /// Appends the virtual paths changed since the cursors and advances them. False if a journal no longer reaches
    /// back to its cursor, after which the caller rescans everything and calls MarkEnd first.
    bool Poll(List<String> &paths);

private:
    struct Root
    {
        /// The folder as a native path, with '/', and the virtual folder it shows.
        String Physical;
        String Virtual;
    };

    struct Volume
    {
        String Key;
        UniquePtr<ChangeJournal> Journal;
        JournalCursor Cursor;
        List<Root> Roots;
        /// Directory ids already resolved: their native path, or empty if outside every root.
        HashMap<uint64, String> Directories;
    };

    struct PollContext
    {
        AssetJournal *Journal;
        Volume *Target;
        List<String> *Paths;
        HashSet<String> *Seen;
    };

    /// The virtual path of a record, or false if it is outside the folders.
    bool Resolve(Volume &volume, const JournalChange &change, String &path);
    /// The native path of a directory inside the roots, cached; null if it is outside them or gone.
    const String *ResolveDirectory(Volume &volume, uint64 id);
    bool ToVirtual(const Volume &volume, StringView native, String &path) const;
    static void Visit(const JournalChange &change, void *state);

    List<UniquePtr<Volume>> _volumes;
};
} // namespace Marlin
