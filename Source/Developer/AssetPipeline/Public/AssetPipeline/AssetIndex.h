#pragma once

#include "AssetPipeline/AssetDatabase.h"
#include "Containers/BitArray.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"
#include "AssetPipeline/AssetIndex.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(ASSETPIPELINE_API, LogAssetIndex, Info, Warning);

namespace Marlin
{
/// One object of a .masset file as the scanner saw it.
struct AssetObjectRecord
{
    Guid Uuid;
    String Name;
    String Type;
    uint32 Line = 0;
    /// AssetCanonical::Hash of the object: what conversions that read it depend on.
    Hash128 Canonical;
    /// Objects it links to, its overridden parent included, and binary files it links to.
    List<Guid> Links;
    List<Guid> BinaryLinks;
    List<String> Tags;
};

/// What scanning one file gives: a .masset with its objects, or a binary source file with its Guid from .mfiles.
struct AssetFileRecord
{
    /// The virtual path, such as /Project/Content/Rock.masset.
    String Path;
    Hash128 ContentHash;
    uint64 Size = 0;
    int64 ModifiedTime = 0;
    bool IsAsset = false;
    Guid BinaryGuid;
    List<AssetObjectRecord> Objects;
};

/// An object as queries return it.
MSTRUCT()
struct AssetObjectInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    String Name;
    MPROPERTY()
    String Type;
    /// The .masset that holds it.
    MPROPERTY()
    String Path;
    MPROPERTY()
    uint32 Line = 0;
};

/// A binary source file as queries return it.
MSTRUCT()
struct AssetBinaryInfo
{
    MGENERATED_BODY()

    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    String Path;
    /// XXH3-128 of the contents, 32 hex digits.
    MPROPERTY()
    String ContentHash;
    /// As scanned: what a reader that cannot afford the hash compares with the file.
    MPROPERTY()
    uint64 Size = 0;
    MPROPERTY()
    int64 ModifiedTime = 0;
};

/// What AssetIndex::Query matches; empty members match anything.
MSTRUCT()
struct AssetQuery
{
    MGENERATED_BODY()

    /// The type name, such as TextureSource.
    MPROPERTY()
    String Type;
    /// A pattern for the object name, with * matching any run of characters; case-insensitive.
    MPROPERTY()
    String Name;
    /// A virtual directory, such as /Project/Content/Rocks.
    MPROPERTY()
    String PathPrefix;
    MPROPERTY()
    String Tag;
    MPROPERTY()
    uint32 Limit = 1000;
};

MSTRUCT()
struct AssetIndexCounts
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Files = 0;
    MPROPERTY()
    uint64 Objects = 0;
    MPROPERTY()
    uint64 Links = 0;
    MPROPERTY()
    uint64 BinaryFiles = 0;
};

/// A read-only view of the index: a base image plus a delta image whose files replace or remove base files. The
/// indexer reads through one, and so do other processes that map the published images (AssetIndexReader).
/// Queries scan pages in parallel on the job system; lookups are hash probes.
class ASSETPIPELINE_API AssetIndexView
{
public:
    /// Both images must outlive the view.
    void Open(Span<const uint8> base, Span<const uint8> delta);

    [[nodiscard]] const AssetDatabaseView &GetBase() const
    {
        return _base;
    }

    [[nodiscard]] const AssetDatabaseView &GetDelta() const
    {
        return _delta;
    }

    bool GetFileStamp(StringView path, uint64 &size, int64 &modifiedTime, Hash128 &contentHash) const;
    bool GetRecord(StringView path, AssetFileRecord &record) const;
    void ForEachFile(FunctionRef<void(StringView path)> visitor) const;
    /// The files at *path*, a file or directory, or below it; every file if *path* is empty. A binary search over
    /// each image, which are sorted by path.
    void ForEachFileBelow(StringView path, FunctionRef<void(StringView path)> visitor) const;

    bool Find(const Guid &uuid, AssetObjectInfo &info) const;
    bool FindBinary(const Guid &uuid, AssetBinaryInfo &info) const;
    /// The binary file at the virtual *path*; false if none is indexed there or it is a .masset.
    bool FindBinaryAt(StringView path, AssetBinaryInfo &info) const;
    /// Looks up many objects at once, in parallel; *found* says which were.
    void FindMany(Span<const Guid> uuids, List<AssetObjectInfo> &infos, List<bool> &found) const;
    /// The canonical hash of an object's source, for dependency checks.
    bool GetCanonicalHash(const Guid &uuid, Hash128 &hash) const;
    /// The content hash of a binary file.
    bool GetBinaryHash(const Guid &uuid, Hash128 &hash) const;
    void Query(const AssetQuery &query, List<AssetObjectInfo> &results) const;
    void GetReferences(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const;
    void GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results) const;
    void GetDependencies(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const;
    /// Kept up to date by Open: the base is counted once per base image, and a delta adjusts that.
    [[nodiscard]] AssetIndexCounts GetCounts() const;

private:
    /// The live object of *uuid*: in the delta, or in a base file the delta keeps. *delta* says which image.
    bool Locate(const Guid &uuid, uint32 &object, bool &delta) const;
    void FillInfo(const AssetDatabaseView &image, uint32 object, AssetObjectInfo &info) const;

    AssetDatabaseView _base;
    AssetDatabaseView _delta;
    /// Base files the delta replaces or removes.
    BitArray<> _removed;
    AssetIndexCounts _counts;
    /// The counts of the base image last counted: the one at _countedBase with generation _countedGeneration.
    AssetIndexCounts _baseCounts;
    const uint8 *_countedBase = nullptr;
    uint64 _countedGeneration = 0;
};

/// The indexer's index (Documentation/Architecture/06 §2): an immutable base image, normally a mapped snapshot,
/// plus the files changed since, kept as records and rebuilt into a small delta image on Commit. Updates are
/// invisible to readers until Commit; once more than FoldThreshold files changed, Commit folds them into a new
/// base. Writers (Commit, Compact, Save, Load, Clear) take turns on a writer lock and build new images outside the
/// read-write lock, which they take exclusively only to swap them in; reads take it shared.
class ASSETPIPELINE_API AssetIndex
{
public:
    AssetIndex();
    ~AssetIndex();
    MARLIN_NONCOPYABLE(AssetIndex);

    /// Once more files than this changed since the base, Commit folds them into a new base.
    static constexpr usize FoldThreshold = 4096;

    /// Adds or replaces *record*'s file, from the next Commit on.
    void Update(const AssetFileRecord &record);
    void Update(AssetFileRecord &&record);
    /// Removes a file from the next Commit on; false if it is not indexed.
    bool Remove(StringView path);
    /// Publishes the updates since the last Commit; returns the new generation, the same one if there were none.
    uint64 Commit();
    void Clear();

    /// Pending updates count for these three, so a scan sees its own work.
    bool GetFileStamp(StringView path, uint64 &size, int64 &modifiedTime, Hash128 &contentHash) const;
    bool GetRecord(StringView path, AssetFileRecord &record) const;
    void ForEachFile(FunctionRef<void(StringView path)> visitor) const;
    /// The files at *path*, a file or directory, or below it.
    void ForEachFileBelow(StringView path, FunctionRef<void(StringView path)> visitor) const;

    bool Find(const Guid &uuid, AssetObjectInfo &info) const;
    bool FindBinary(const Guid &uuid, AssetBinaryInfo &info) const;
    /// The binary file at the virtual *path*; false if none is indexed there or it is a .masset.
    bool FindBinaryAt(StringView path, AssetBinaryInfo &info) const;
    void FindMany(Span<const Guid> uuids, List<AssetObjectInfo> &infos, List<bool> &found) const;
    bool GetCanonicalHash(const Guid &uuid, Hash128 &hash) const;
    bool GetBinaryHash(const Guid &uuid, Hash128 &hash) const;
    void Query(const AssetQuery &query, List<AssetObjectInfo> &results) const;
    void GetReferences(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const;
    void GetReferencers(const Guid &uuid, List<AssetObjectInfo> &results) const;
    void GetDependencies(const Guid &uuid, List<Guid> &objects, List<Guid> &binaries) const;
    [[nodiscard]] AssetIndexCounts GetCounts() const;

    [[nodiscard]] uint64 GetGeneration() const;
    /// Runs *visitor* with the committed images under the read lock, for publishing them; *basePath* is the
    /// snapshot file the base is mapped from, empty while it only lives in memory.
    void WithImages(
        FunctionRef<void(Span<const uint8> base, Span<const uint8> delta, uint64 generation, StringView basePath)>
            visitor) const;

    /// Makes every fold write the new base to `<directory>/Index.<generation>.snapshot` and map it from there, so
    /// other processes can map it too; older snapshots are deleted once nothing maps them.
    void SetSnapshotDirectory(StringView directory);
    /// Loads the newest valid snapshot in the snapshot directory; false if there is none.
    bool LoadLatest();
    /// The snapshot file of the current base, empty if it is not in one.
    [[nodiscard]] String GetBasePath() const;

    /// Folds the delta into the base, unless nothing changed, and writes the base to *path*.
    bool Save(StringView path);
    /// Replaces the index with a snapshot written by Save, such as one from CI, mapping it.
    bool Load(StringView path);
    static bool AppendLog(StringView logPath, StringView path, const AssetFileRecord *record);
    /// Appends entries WriteLogEntries made, in one write.
    static bool AppendLog(StringView logPath, Span<const uint8> entries);
    /// Appends a log entry to *entries* for each of *paths*: its record as the index has it now, or its removal.
    void WriteLogEntries(Span<const String> paths, List<uint8> &entries) const;
    /// Applies a log written by AppendLog and commits; stops at a torn record at the end.
    bool ReplayLog(StringView logPath);
    /// Folds the delta into the base, and into a snapshot if there is a snapshot directory; does nothing if no file
    /// changed since a base that already is one.
    void Compact();

private:
    struct State;

    /// Moves the pending updates into the changes; false if there were none. Under the exclusive lock.
    bool MergePendingLocked();
    /// These run under the writer lock only.
    [[nodiscard]] bool NeedsFold() const;
    void PublishDelta();
    void Fold();
    [[nodiscard]] const AssetFileRecord *FindPending(StringView path, bool &removed) const;

    /// Held by a writer for a whole Commit, Compact, Save, Load or Clear, so writers never interleave.
    Mutex _writeLock;
    mutable ReadWriteLock _lock;
    State *_state;
};
} // namespace Marlin
