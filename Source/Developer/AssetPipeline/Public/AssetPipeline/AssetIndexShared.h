#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformIpc.h"
#include "HAL/PlatformSync.h"
#include "String/String.h"
#include "Templates/Function.h"

namespace Marlin
{
/// Publishes the indexer's committed images to other processes on the machine (Documentation/Architecture/06 §2):
/// the base stays in its snapshot file, the delta is copied into a new named shared memory segment per commit, and
/// a small control block says which ones are current. Readers map them; nothing goes through a pipe.
class ASSETPIPELINE_API AssetIndexPublisher
{
public:
    AssetIndexPublisher() = default;
    ~AssetIndexPublisher();
    MARLIN_NONCOPYABLE(AssetIndexPublisher);

    /// *name* is shared by the indexer and its readers, such as the indexer's pipe name.
    bool Open(StringView name);
    void Close();
    /// Publishes what *index* has committed; its base must be in a snapshot file (SetSnapshotDirectory).
    bool Publish(const AssetIndex &index);

private:
    struct Segment
    {
        SharedMemory Memory;
        uint64 Generation = 0;
    };

    String _name;
    SharedMemory _control;
    List<Segment> _segments;
    uint64 _published = ~uint64{0};
};

/// Maps what an AssetIndexPublisher publishes and answers queries in this process: a lookup costs what it costs in
/// the indexer, with no kernel call once mapped. Checks for a newer generation with one atomic load per Read and
/// remaps when there is one.
class ASSETPIPELINE_API AssetIndexReader
{
public:
    AssetIndexReader() = default;
    ~AssetIndexReader();
    MARLIN_NONCOPYABLE(AssetIndexReader);

    /// False if no indexer publishes *name*.
    bool Open(StringView name);
    void Close();
    [[nodiscard]] bool IsOpen() const
    {
        return _control.Data != nullptr;
    }

    /// Runs *visitor* with the newest published view; other threads may read at the same time.
    bool Read(FunctionRef<void(const AssetIndexView &view)> visitor);
    [[nodiscard]] uint64 GetGeneration() const;

private:
    /// Maps the published generation if it is newer; false if the indexer stopped publishing.
    bool Refresh();

    String _name;
    SharedMemory _control;
    ReadWriteLock _lock;
    FileHandle _baseFile;
    MappedRegion _base;
    String _basePath;
    SharedMemory _delta;
    AssetIndexView _view;
    uint64 _generation = 0;
};
} // namespace Marlin
