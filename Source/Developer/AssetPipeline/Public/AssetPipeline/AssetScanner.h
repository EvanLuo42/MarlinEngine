#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "Concurrency/Atomic.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Vfs/Vfs.h"
#include "AssetPipeline/AssetScanner.gen.h"

namespace Marlin
{
class StringWriter;

/// A binary source file's entry in the .mfiles of its content folder and layer.
MSTRUCT()
struct BinaryFileEntry
{
    MGENERATED_BODY()

    /// Relative to the folder that holds .mfiles, with '/'.
    MPROPERTY()
    String Path;
    MPROPERTY()
    String Hash;
    MPROPERTY()
    uint64 Size = 0;
};

/// Fills an AssetIndex from the files under content folders, such as /Project/Content, through every VFS layer of
/// them (the folder and its platform layers). A .masset gives its objects and links; any other file is a binary source
/// file whose Guid lives in the `.mfiles` of the folder in the layer that holds the file, which the scanner keeps: an
/// entry follows its file through a rename by content hash, so files moved outside the editor keep their Guid.
/// Unchanged files, by size and time, are not read again; the others are hashed and parsed in parallel on the job
/// system. Each scan is one batch: the .mfiles it changed are written once and the index commits once, at its end.
class ASSETPIPELINE_API AssetScanner
{
public:
    /// *folders* are virtual folders, each with its own .mfiles in each layer.
    AssetScanner(AssetIndex &index, List<String> folders);

    /// Scans every folder and commits; appends the paths whose rows changed.
    void ScanAll(List<String> &changed);
    /// Rescans *path*, a file or folder below one of the folders, after a change on disk, and commits.
    void ScanPath(StringView path, List<String> &changed);
    /// Rescans each of *paths* as ScanPath does, as one batch with one commit.
    void ScanPaths(Span<const String> paths, List<String> &changed);

    [[nodiscard]] Span<const String> GetFolders() const
    {
        return _folders.AsSpan();
    }

    /// Changed files the scans so far have found to read, and how many of them are read; from any thread.
    void GetProgress(uint64 &read, uint64 &found) const
    {
        found = _filesFound.Load(MemoryOrder::Relaxed);
        read = _filesRead.Load(MemoryOrder::Relaxed);
    }

    /// Reads the objects and links of a .masset; false with *errors* if it does not parse.
    static bool ParseAsset(StringView path, StringView text, AssetFileRecord &record, StringWriter &errors);
    /// XXH3-128 of a file's contents.
    static bool HashFile(StringView path, Hash128 &hash);

private:
    /// What a file looked like, to tell whether it changed: .mfiles is read again only when its stamp changes, so
    /// the scanner's own writes, which it records, are not read back.
    struct FileStamp
    {
        bool Exists = false;
        uint64 Size = 0;
        int64 ModifiedTime = 0;

        bool operator==(const FileStamp &) const = default;
    };

    /// The .mfiles of a folder in one VFS layer.
    struct FolderLayer
    {
        MountId Layer = 0;
        HashMap<String, Guid> ByPath;
        HashMap<Guid, BinaryFileEntry> Entries;
        /// Entries by content hash, for following renames. It may still name entries that changed or went away
        /// since; lookups check against Entries.
        HashMap<String, List<Guid>> ByHash;
        /// Entries whose file the current scan found in this layer.
        HashSet<Guid> Seen;
        /// The .mfiles as last read or written.
        FileStamp Stamp;
        bool Dirty = false;
    };

    struct Folder
    {
        String Path;
        List<FolderLayer> Layers;
        /// Vfs::GetLayers of the folder at the last load.
        List<MountId> LayerIds;
        bool Loaded = false;
    };

    /// A file to hash, and parse if it is a .masset, in parallel with the others of its batch.
    struct ScanItem
    {
        Folder *Owner = nullptr;
        String Path;
        FileInfo Info;
        MountId Layer = 0;
        bool Asset = false;
        bool Listed = false;
        /// Set by NeedsScan: the file changed since it was indexed, by size or time; and whether it was indexed, with
        /// the content hash it had.
        bool Changed = false;
        bool Known = false;
        Hash128 IndexedHash;
        /// Filled in by Process.
        bool Read = false;
        bool Parsed = false;
        Hash128 Hash;
        AssetFileRecord Record;
        String Errors;
    };

    [[nodiscard]] static FileStamp StampOf(MountId layer, StringView path);
    Folder *FolderOf(StringView path, StringView &relative);
    static FolderLayer *FindLayer(Folder &folder, MountId layer);
    /// Reads the .mfiles of each layer whose stamp changed since it was last read or written.
    void LoadFolder(Folder &folder);
    void SaveFolder(Folder &folder);
    void BeginBatch();
    /// Writes every .mfiles the batch changed and commits.
    void EndBatch();
    void ScanFolder(Folder &folder, List<String> &changed);
    /// Rescans a file or folder below *folder*; files to read go to *items*.
    void ScanBelow(Folder &folder, StringView path, List<ScanItem> &items, List<String> &changed);
    void Walk(StringView directory, List<ScanItem> &files) const;
    /// Sets item.Changed if its file changed since it was indexed.
    void NeedsScan(ScanItem &item);
    /// Hashes and parses the changed *items* in parallel, then indexes them, assets and listed binaries first, so a
    /// renamed file can take the Guid of the entry its old path left; *present* is every file of a whole-folder scan.
    void ProcessItems(List<ScanItem> &items, List<String> &changed, const HashSet<StringView> *present);
    static void Process(ScanItem &item);
    void Apply(ScanItem &item, List<String> &changed, const HashSet<StringView> *present);
    Guid AssignBinary(Folder &folder, MountId layer, StringView relative, const Hash128 &hash, uint64 size,
                      const HashSet<StringView> *present);
    /// Removes the indexed files at *path* or below it that *present* does not have (all of them without it).
    void RemoveGone(StringView path, Folder *folder, const HashSet<StringView> *present, List<String> &changed);

    AssetIndex &_index;
    List<String> _folders;
    List<Folder> _state;
    Atomic<uint64> _filesFound{0};
    Atomic<uint64> _filesRead{0};
};

/// The content folders of the running project: each subfolder Engine.AssetIndex.Folders lists (default Content) under
/// /Engine and /Project.
[[nodiscard]] ASSETPIPELINE_API List<String> GetProjectContentFolders();

/// Starts *index* from the project's asset indexer state (<Project>/Intermediate/AssetIndex): its newest snapshot
/// and its log, read only. A ScanAll after it reads only the files that changed since, instead of every file, as a
/// tool such as the packager would on an empty index. False if there is no snapshot.
ASSETPIPELINE_API bool LoadProjectIndex(AssetIndex &index);

/// The native folders that show the virtual *folder*, one per VFS layer that has it, for watching them. If no layer
/// has it, creates it in the lowest layer, the folder that is not a platform layer, and returns that.
[[nodiscard]] ASSETPIPELINE_API List<String> GetPhysicalFolders(StringView folder);
} // namespace Marlin
