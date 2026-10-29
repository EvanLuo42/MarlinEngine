#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetIndexShared.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConverterPool.h"
#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "Memory/UniquePtr.h"
#include "Remote/RemoteLogStream.h"
#include "Remote/RemoteServer.h"
#include "String/String.h"
#include "AssetPipeline/AssetIndexService.gen.h"

namespace Marlin
{
class AssetJournal;

MSTRUCT()
struct AssetGuidParams
{
    MGENERATED_BODY()

    MPROPERTY()
    Guid Uuid;
};

MSTRUCT()
struct AssetPathParams
{
    MGENERATED_BODY()

    MPROPERTY()
    String Path;
};

/// What AssetIndex.CanonicalHash and AssetIndex.BinaryHash answer.
MSTRUCT()
struct AssetHashResult
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Low = 0;
    MPROPERTY()
    uint64 High = 0;
};

MSTRUCT()
struct AssetLinks
{
    MGENERATED_BODY()

    MPROPERTY()
    List<Guid> Objects;
    MPROPERTY()
    List<Guid> Binaries;
};

MSTRUCT()
struct AssetIndexStatus
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetIndexCounts Counts;
    /// True until the first full scan finishes.
    MPROPERTY()
    bool Scanning = false;
    MPROPERTY()
    List<String> Folders;
    /// Global shaders the start-up warm step has compiled, and how many it compiles (MarlinLanguage.md §1.6).
    MPROPERTY()
    uint32 ShadersWarmed = 0;
    MPROPERTY()
    uint32 ShadersToWarm = 0;
    /// Changed files the scans since the indexer started have found to read, and how many of them are read.
    MPROPERTY()
    uint64 FilesToRead = 0;
    MPROPERTY()
    uint64 FilesRead = 0;
    /// Conversions waiting for a worker and on one, and those finished since the indexer started (ConversionProgress).
    MPROPERTY()
    uint32 ConversionsQueued = 0;
    MPROPERTY()
    uint32 ConversionsRunning = 0;
    MPROPERTY()
    uint64 Converted = 0;
    MPROPERTY()
    uint64 ConversionCacheHits = 0;
    MPROPERTY()
    uint64 ConversionsFailed = 0;
};

/// A located object the indexer keeps across restarts in `<StoreDirectory>/Located.json`, so a client's first
/// Asset.Locate after a restart needs no worker.
MSTRUCT()
struct AssetLocatedRecord
{
    MGENERATED_BODY()

    MPROPERTY()
    String Key;
    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    ConvertedResultLocation Location;
    MPROPERTY()
    List<String> Paths;
    MPROPERTY()
    ConversionTarget Target;
};

MSTRUCT()
struct AssetLocatedRecords
{
    MGENERATED_BODY()

    MPROPERTY()
    List<AssetLocatedRecord> Entries;
};

struct AssetIndexServiceDesc
{
    /// Virtual folders to index, such as /Engine/Content and /Project/Content.
    List<String> Folders;
    /// Where the snapshot and log live, a native directory such as <Project>/Intermediate/AssetIndex.
    String StoreDirectory;
    /// The pipe clients connect to; AssetIndexer::PipeNameFor gives the one for a project.
    String PipeName;
    /// A snapshot to start from instead of the local one, such as CI's; only differences are scanned.
    String Seed;
    /// The .mproject, for converter workers; without it Asset.Locate is not served.
    String ProjectFile;
    /// The GlobalStore the kept located objects are checked against at start (LoadLocated); port 0 for the one
    /// Engine.GlobalStore names, as the workers use.
    String GlobalStoreHost;
    uint16 GlobalStorePort = 0;
    uint32 MaxConverters = 4;
};

/// Runs an AssetIndex for one project: loads the snapshot and replays the log, scans on its own thread, follows
/// file changes, logs every update, and serves the AssetIndex.* methods and the AssetIndex.Changed stream. With a
/// project file it also runs the converter pool and serves Asset.Locate: where the converted data of an object is,
/// converting it first on a miss, and Shader.Locate, the same for global shaders by entry point.
class ASSETPIPELINE_API AssetIndexService
{
public:
    AssetIndexService(NetLoop &loop, AssetIndexServiceDesc desc);
    ~AssetIndexService();
    MARLIN_NONCOPYABLE(AssetIndexService);

    bool Start();
    /// Waits for the scan thread, then writes the snapshot and drops the log.
    void Stop();
    /// Runs the server's main-thread work; the program's loop calls it.
    void Tick();
    /// Compiles *shaders* in the background so their first Shader.Locate finds them converted (MarlinLanguage.md
    /// §1.6); progress is logged and reported by AssetIndex.Status. Needs a project file.
    void WarmShaders(Span<const ShaderLocateParams> shaders);

    [[nodiscard]] AssetIndex &GetIndex()
    {
        return _index;
    }

    [[nodiscard]] RemoteServer &GetServer()
    {
        return *_server;
    }

    [[nodiscard]] bool IsScanning() const
    {
        return _scanning.Load(MemoryOrder::Acquire);
    }

    /// Nothing to do and no one to do it for: no scan running, no conversion waiting and no client connected but
    /// converter workers, which die with the indexer. An indexer started for a client exits after a while of this (06
    /// §2).
    [[nodiscard]] bool IsIdle();

private:
    static void ScanThreadMain(void *argument);
    void ScanLoop();
    void FollowJournal(AssetJournal &journal);
    void FollowWatchers();
    /// Rescans *paths*, or everything, commits, logs and publishes.
    void Scan(StringView reason, bool all, Span<const String> paths);
    void Commit(const List<String> &changed);
    void AddMethods();
    void Locate(RemoteCall &call);
    void Resolve(const AssetLocateParams &params, const RemoteDeferred &deferred);
    void LocateShader(RemoteCall &call);
    /// Script.Locate: the module of a script package, compiling it on a miss (MarlinScript §11.1).
    void LocateScript(RemoteCall &call);
    /// Tells connection *connection*, which asked for *uuid*, that it is converted first: Asset.Converting
    /// { Uuid, Name, Shader }, for its splash screen (Documentation/Architecture/09 §4.6).
    void NotifyConverting(uint32 connection, const Guid &uuid, StringView name, bool shader);
    /// Remembers where a shader converted to, from a request made at *epoch* (Remember), until a language file
    /// changes.
    void AddLocatedShader(const ShaderLocateParams &params, const ConversionOutcome &outcome, uint64 epoch);
    /// Remembers where a script module converted to, until a script source file changes or is added.
    void AddLocatedScript(const ScriptLocateParams &params, const ConversionOutcome &outcome, uint64 epoch);
    static void ShaderWatchMain(void *argument);
    /// Follows edits to the Language and Config folders beside the indexed Content folders: language files
    /// invalidate shaders, script modules and what reflects shaders (materials); config files what read config.
    void WatchShaders();
    /// Drops the located results that *changed* may have made stale and, when someone follows Asset.Changed,
    /// converts them again in the background, announcing only those whose result key changed or that now fail.
    void Invalidate(const List<String> &changed);

    /// Locations already converted, until a file they came from changes.
    struct Located
    {
        Guid Uuid;
        ConvertedResultLocation Location;
        List<String> Paths;
        /// What it was converted for, to convert it again: the target, and the entry point of a shader or the
        /// package of a script module.
        ConversionTarget Target;
        String Shader;
        String Script;
        /// It read config, or shader sources through a converter input (a material's struct), which Paths do not
        /// list: any config file, or any language file, may change it.
        bool ReadConfig = false;
        bool ReadLanguage = false;
    };
    /// The located result for *key* from *outcome*, of a request made at *epoch*; kept only if nothing was
    /// invalidated since, as the conversion may have read a file from before the change.
    void Remember(const String &key, Located located, uint64 epoch);
    /// Located for an object's conversion *outcome*: its source file and the files of the inputs it read.
    Located MakeLocated(const Guid &uuid, const ConversionTarget &target, const ConversionOutcome &outcome);
    /// Converts *old*, which *key* located, again; Asset.Changed announces it unless its result key is the same.
    void Revalidate(const String &key, const Located &old);
    void PublishChanged(Span<const Guid> uuids);
    /// Keeps the located objects whose staleness the snapshot and journal tell, for the next start: not shaders,
    /// script modules or what read config or shader sources, whose folders are only watched while running.
    void SaveLocated();
    /// Takes back what SaveLocated kept, those results GlobalStore still has, and removes the file, so a crash
    /// later never brings back what changed since.
    void LoadLocated();
    [[nodiscard]] String LocatedPath() const;
    [[nodiscard]] String LogPath() const;
    [[nodiscard]] String JournalPath() const;

    NetLoop &_loop;
    AssetIndexServiceDesc _desc;
    AssetIndex _index;
    AssetScanner _scanner;
    AssetIndexPublisher _publisher;
    UniquePtr<RemoteServer> _server;
    /// This process's records, and those workers forward with Converter.Log, on Log.Stream for the editor and game.
    RemoteLogStream _logStream;
    ThreadHandle _scanThread;
    ThreadHandle _shaderThread;
    Atomic<bool> _stopping{false};
    Atomic<bool> _scanning{true};
    Atomic<bool> _rescan{false};
    Atomic<uint32> _shadersWarmed{0};
    Atomic<uint32> _shadersToWarm{0};
    Atomic<uint32> _shadersFailed{0};
    /// True if a snapshot was loaded, so a journal cursor saved with it can be replayed.
    bool _loaded = false;
    UniquePtr<ConverterPool> _converters;

    /// Asset.Locate calls that came before the first scan finished.
    struct WaitingLocate
    {
        AssetLocateParams Params;
        RemoteDeferred Deferred;
    };
    Mutex _locatedLock;
    HashMap<String, Located> _located;
    /// Counts Invalidate calls, under _locatedLock; see Remember.
    uint64 _epoch = 0;
    /// Counts config changes; workers read config again when a job carries a newer one.
    uint64 _configGeneration = 0;
    List<WaitingLocate> _waitingForScan;
    uint64 _logSize = 0;
};
} // namespace Marlin
