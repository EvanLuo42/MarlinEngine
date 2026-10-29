#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "Concurrency/Atomic.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSync.h"
#include "Remote/RemoteServer.h"
#include "Templates/Function.h"
#include "AssetPipeline/ConverterPool.gen.h"

namespace Marlin
{
/// A job as a worker receives it from Converter.Take; Job is zero when there was none to take.
MSTRUCT()
struct ConversionJob
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Job = 0;
    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    ConversionTarget Target;
    /// A global shader's entry point (MarlinLanguage.md §1.6), whose Guid Uuid is; empty for an object.
    MPROPERTY()
    String Shader;
    /// A script package (MarlinScript §11.1), whose module's Guid Uuid is; empty for an object.
    MPROPERTY()
    String Script;
    /// How many times the indexer has seen config change; a worker that read config before the latest change reads
    /// it again first.
    MPROPERTY()
    uint64 ConfigGeneration = 0;
};

/// What a worker sends back with Converter.Finish.
MSTRUCT()
struct ConversionOutcome
{
    MGENERATED_BODY()

    MPROPERTY()
    uint64 Job = 0;
    MPROPERTY()
    bool Succeeded = false;
    MPROPERTY()
    bool CacheHit = false;
    MPROPERTY()
    String Key;
    MPROPERTY()
    ConvertedResultLocation Location;
    MPROPERTY()
    String Errors;
    /// The objects and binary files the conversion read, so their changes can invalidate it.
    MPROPERTY()
    List<Guid> Inputs;
    /// True if it read config, which Inputs do not list.
    MPROPERTY()
    bool ReadConfig = false;
    /// True if a converter input of it reflects shader sources, such as a material's struct, which Inputs and
    /// Files do not list.
    MPROPERTY()
    bool ReadLanguage = false;
    /// The source files it read by path, such as a shader's code.
    MPROPERTY()
    List<String> Files;
};

/// What a ConverterPool is doing and has done since it started, for AssetIndex.Status and the editor's splash.
struct ConversionProgress
{
    /// Jobs waiting for a worker, and jobs a worker has.
    uint32 Queued = 0;
    uint32 Running = 0;
    /// Finished jobs: converted, found already in GlobalStore, failed.
    uint64 Converted = 0;
    uint64 CacheHits = 0;
    uint64 Failed = 0;
};

enum class ConversionPriority : uint8
{
    /// Someone waits for it, such as a load.
    Urgent,
    /// Warming the cache in the background.
    Background,
};

using ConversionCallback = Function<void(const ConversionOutcome &outcome)>;

/// Runs MarlinConverter worker processes for the indexer: workers connect back over its pipe, take jobs with
/// Converter.Take and report with Converter.Finish. Requests for the same object and target share one job; a job
/// whose worker dies is retried on another, twice, then fails. Workers start as jobs wait and leave when idle.
///
/// Both calls batch: Take with params `{"Max": N}` answers `{"Jobs": [job, ...]}` with up to N jobs, a fair share
/// of what waits, and Finish takes `{"Outcomes": [outcome, ...]}` and answers one bool per outcome. Take without
/// params answers one ConversionJob, and Finish with a ConversionOutcome answers one bool, as before.
class ASSETPIPELINE_API ConverterPool
{
public:
    /// *projectFile* and *indexerPipe* are passed to the workers; with *maxWorkers* zero none are started here and
    /// only workers that connect by themselves, such as from other machines, take jobs.
    ConverterPool(RemoteServer &server, StringView projectFile, StringView indexerPipe, uint32 maxWorkers);
    ~ConverterPool();
    MARLIN_NONCOPYABLE(ConverterPool);

    /// Converts the object *uuid*, or with *shader* the global shader of that entry point, or with *script* the
    /// script module of that package, whose Guid *uuid* is.
    void Request(const Guid &uuid, const ConversionTarget &target, ConversionPriority priority,
                 ConversionCallback callback, StringView shader = {}, StringView script = {});
    /// Starts workers, retries jobs of lost ones and expires idle takes; the indexer's loop calls it.
    void Tick();
    /// The ConversionJob::ConfigGeneration jobs handed out from now on carry.
    void SetConfigGeneration(uint64 generation)
    {
        _configGeneration.Store(generation, MemoryOrder::Release);
    }
    /// Fails what is waiting and stops the workers.
    void Shutdown();
    /// No job is waiting or running.
    [[nodiscard]] bool IsIdle();
    /// The connections of workers, which have taken jobs, still connected; they are not clients of the index.
    [[nodiscard]] usize GetWorkerConnectionCount();
    [[nodiscard]] ConversionProgress GetProgress();

private:
    Atomic<uint64> _configGeneration{0};

    struct Job
    {
        uint64 Id = 0;
        Guid Uuid;
        ConversionTarget Target;
        String Shader;
        String Script;
        /// What requests for the same work share the job by: JobKey of the four above.
        String Key;
        ConversionPriority Priority = ConversionPriority::Background;
        List<ConversionCallback> Callbacks;
        uint32 Worker = 0;
        uint32 Attempts = 0;
        /// Waiting in a queue; an entry of a job no longer waiting is skipped when it comes up.
        bool Queued = false;
    };

    struct Taker
    {
        RemoteDeferred Deferred;
        uint32 Connection = 0;
        uint64 Since = 0;
        /// The most jobs this take accepts; Batch when it asked with Max and is answered with a list.
        uint32 Max = 1;
        bool Batch = false;
    };

    /// A job a worker finished, with the callbacks it owes.
    struct Finished
    {
        ConversionOutcome Outcome;
        List<ConversionCallback> Callbacks;
    };

    void Take(RemoteCall &call);
    void Finish(RemoteCall &call);
    /// Moves out the callbacks of the job *outcome* reports, if *connection* runs it. Called with the lock held.
    bool FinishLocked(ConversionOutcome outcome, uint32 connection, List<Finished> &finished);
    /// Gives queued jobs to waiting workers. Called with the lock held; returns the takes to answer.
    void DispatchLocked(List<Taker> &answered, List<String> &jobs);
    /// Puts *job* in the urgent queue, which is taken newest first, or at the back of the background one.
    void QueueLocked(Job &job, bool urgent);
    /// The next waiting job, no longer queued, or null.
    Job *PopLocked();
    void RemoveLocked(uint64 id);
    static String JobKey(const Guid &uuid, const ConversionTarget &target, StringView shader, StringView script);
    void SpawnWorker();

    RemoteServer &_server;
    String _projectFile;
    String _indexerPipe;
    uint32 _maxWorkers;
    Mutex _lock;
    HashMap<uint64, Job> _jobs;
    HashMap<String, uint64> _jobsByKey;
    /// Urgent jobs, taken from the back; background ones, taken from _backgroundHead.
    List<uint64> _urgent;
    List<uint64> _background;
    usize _backgroundHead = 0;
    /// Jobs with Queued set.
    usize _queued = 0;
    /// Jobs a worker has, which Tick checks for lost workers.
    HashSet<uint64> _running;
    ConversionProgress _finished;
    List<Taker> _takers;
    List<ProcessHandle> _workers;
    HashSet<uint32> _workerConnections;
    uint64 _nextJob = 1;
    bool _shutdown = false;
};
} // namespace Marlin
