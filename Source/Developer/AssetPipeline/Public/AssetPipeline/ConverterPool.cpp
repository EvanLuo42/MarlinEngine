#include "AssetPipeline/ConverterPool.h"

#include "Concurrency/ScopeLock.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// A take with no job is answered empty after this, so workers never wait on a request the server forgot.
constexpr uint64 TakeTimeout = 20000;
constexpr uint32 MaxAttempts = 3;
/// The most jobs one batched take is given, whatever it asks for.
constexpr uint64 MaxTakeBatch = 256;

uint64 PoolMilliseconds()
{
    return PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
}

String EmptyTake(bool batch)
{
    if (batch)
    {
        return String("{\"Jobs\":[]}");
    }
    StringBuilder<256> text;
    JsonWriter writer(text);
    Json::Write(writer, ConversionJob());
    return text.ToString();
}
} // namespace

ConverterPool::ConverterPool(RemoteServer &server, StringView projectFile, StringView indexerPipe, uint32 maxWorkers)
    : _server(server), _projectFile(projectFile), _indexerPipe(indexerPipe), _maxWorkers(maxWorkers)
{
    _server.AddMethod(
        "Converter.Take", [this](RemoteCall &call) { Take(call); }, RemoteThread::Any,
        "For MarlinConverter workers: waits for a job to convert; with {\"Max\": N}, for up to N of them.");
    _server.AddMethod(
        "Converter.Finish", [this](RemoteCall &call) { Finish(call); }, RemoteThread::Any,
        "For MarlinConverter workers: reports a job's outcome, or with {\"Outcomes\": [...]} several.");
}

ConverterPool::~ConverterPool()
{
    Shutdown();
}

String ConverterPool::JobKey(const Guid &uuid, const ConversionTarget &target, StringView shader, StringView script)
{
    StringBuilder<128> key;
    uuid.FormatTo(key, "N");
    key.Append('/').Append(target.Platform).Append('/');
    key.Append(target.Kind == TargetKind::Server ? "Server" : "Client");
    key.Append('/').Append(shader).Append('/').Append(script);
    return key.ToString();
}

void ConverterPool::QueueLocked(Job &job, bool urgent)
{
    if (!job.Queued)
    {
        job.Queued = true;
        ++_queued;
    }
    (urgent ? _urgent : _background).Add(job.Id);
}

ConverterPool::Job *ConverterPool::PopLocked()
{
    // A job can have several entries, after it was made urgent or retried; only the first one taken counts.
    while (!_urgent.IsEmpty())
    {
        Job *job = _jobs.Find(_urgent.Pop());
        if (job != nullptr && job->Queued)
        {
            job->Queued = false;
            --_queued;
            return job;
        }
    }
    Job *found = nullptr;
    while (found == nullptr && _backgroundHead < _background.Count())
    {
        Job *job = _jobs.Find(_background[_backgroundHead++]);
        if (job != nullptr && job->Queued)
        {
            job->Queued = false;
            --_queued;
            found = job;
        }
    }
    if (_backgroundHead == _background.Count())
    {
        _background.Clear();
        _backgroundHead = 0;
    }
    else if (_backgroundHead > 1024 && _backgroundHead * 2 > _background.Count())
    {
        _background.RemoveAt(0, _backgroundHead);
        _backgroundHead = 0;
    }
    return found;
}

void ConverterPool::RemoveLocked(uint64 id)
{
    Job *job = _jobs.Find(id);
    if (job == nullptr)
    {
        return;
    }
    if (job->Queued)
    {
        --_queued;
    }
    (void)_jobsByKey.Remove(job->Key);
    (void)_running.Remove(id);
    (void)_jobs.Remove(id);
}

void ConverterPool::Request(const Guid &uuid, const ConversionTarget &target, ConversionPriority priority,
                            ConversionCallback callback, StringView shader, StringView script)
{
    List<Taker> answered;
    List<String> jobs;
    {
        const ScopeLock lock(_lock);
        if (_shutdown)
        {
            ConversionOutcome outcome;
            outcome.Errors = "the indexer is shutting down";
            callback(outcome);
            return;
        }
        String key = JobKey(uuid, target, shader, script);
        if (const uint64 *existing = _jobsByKey.Find(key))
        {
            Job &job = *_jobs.Find(*existing);
            job.Callbacks.Add(Move(callback));
            if (priority == ConversionPriority::Urgent && job.Priority != priority)
            {
                job.Priority = priority;
                if (job.Queued)
                {
                    QueueLocked(job, true);
                }
            }
            return;
        }
        Job &job = _jobs.FindOrAdd(_nextJob);
        job.Id = _nextJob++;
        job.Uuid = uuid;
        job.Target = target;
        job.Shader = shader;
        job.Script = script;
        job.Key = Move(key);
        job.Priority = priority;
        job.Callbacks.Add(Move(callback));
        _jobsByKey.FindOrAdd(job.Key) = job.Id;
        QueueLocked(job, priority == ConversionPriority::Urgent);
        DispatchLocked(answered, jobs);
    }
    for (usize i = 0; i < answered.Count(); ++i)
    {
        _server.Complete(answered[i].Deferred, jobs[i].AsView());
    }
}

void ConverterPool::DispatchLocked(List<Taker> &answered, List<String> &jobs)
{
    while (_queued != 0 && !_takers.IsEmpty())
    {
        const Taker taker = _takers[0];
        _takers.RemoveAt(0);
        if (!_server.IsConnected(taker.Connection))
        {
            continue;
        }
        // A fair share of what waits, so that one worker does not take every job while the others idle; a long
        // queue of cache hits still goes out in full batches.
        usize workers = _workerConnections.Count() > _maxWorkers ? _workerConnections.Count() : _maxWorkers;
        workers = workers == 0 ? 1 : workers;
        usize count = (_queued + workers - 1) / workers;
        count = count < taker.Max ? count : taker.Max;
        count = count == 0 ? 1 : count;
        StringBuilder<1024> text;
        JsonWriter writer(text);
        if (taker.Batch)
        {
            writer.BeginObject().Key("Jobs").BeginArray();
        }
        for (usize i = 0; i < count; ++i)
        {
            Job *job = PopLocked();
            if (job == nullptr)
            {
                break;
            }
            job->Worker = taker.Connection;
            ++job->Attempts;
            _running.Add(job->Id);
            ConversionJob message;
            message.Job = job->Id;
            message.Uuid = job->Uuid;
            message.Target = job->Target;
            message.Shader = job->Shader;
            message.Script = job->Script;
            message.ConfigGeneration = _configGeneration.Load(MemoryOrder::Acquire);
            Json::Write(writer, message);
        }
        if (taker.Batch)
        {
            writer.EndArray().EndObject();
        }
        answered.Add(taker);
        jobs.Add(text.ToString());
    }
}

void ConverterPool::Take(RemoteCall &call)
{
    Taker taker;
    const JsonValue &params = call.GetParams();
    if (const JsonValue *max = params.IsObject() ? params.Find("Max") : nullptr)
    {
        uint64 count = 0;
        if (max->TryGetUnsigned(count) && count != 0)
        {
            taker.Max = static_cast<uint32>(count < MaxTakeBatch ? count : MaxTakeBatch);
            taker.Batch = true;
        }
    }
    List<Taker> answered;
    List<String> jobs;
    {
        const ScopeLock lock(_lock);
        if (_shutdown)
        {
            call.Fail("the indexer is shutting down");
            return;
        }
        _workerConnections.Add(call.GetConnectionId());
        taker.Deferred = call.Defer();
        taker.Connection = call.GetConnectionId();
        taker.Since = PoolMilliseconds();
        _takers.Add(taker);
        DispatchLocked(answered, jobs);
    }
    for (usize i = 0; i < answered.Count(); ++i)
    {
        _server.Complete(answered[i].Deferred, jobs[i].AsView());
    }
}

bool ConverterPool::FinishLocked(ConversionOutcome outcome, uint32 connection, List<Finished> &finished)
{
    Job *job = _jobs.Find(outcome.Job);
    if (job == nullptr || job->Worker != connection)
    {
        return false;
    }
    Finished &done = finished.AddDefaulted();
    done.Callbacks = Move(job->Callbacks);
    RemoveLocked(outcome.Job);
    ++(!outcome.Succeeded ? _finished.Failed : outcome.CacheHit ? _finished.CacheHits : _finished.Converted);
    done.Outcome = Move(outcome);
    return true;
}

void ConverterPool::Finish(RemoteCall &call)
{
    const JsonValue &params = call.GetParams();
    const JsonValue *batch = params.IsObject() ? params.Find("Outcomes") : nullptr;
    List<ConversionOutcome> outcomes;
    StringBuilder<256> error;
    if (batch != nullptr && batch->IsArray())
    {
        for (const JsonValue &element : batch->GetElements())
        {
            if (!Json::Read(element, outcomes.AddDefaulted(), error))
            {
                call.Fail(error.AsView(), RemoteErrors::InvalidParams);
                return;
            }
        }
    }
    else if (!Json::Read(params, outcomes.AddDefaulted(), error))
    {
        call.Fail(error.AsView(), RemoteErrors::InvalidParams);
        return;
    }
    List<Finished> finished;
    List<bool> accepted;
    {
        const ScopeLock lock(_lock);
        for (ConversionOutcome &outcome : outcomes)
        {
            accepted.Add(FinishLocked(Move(outcome), call.GetConnectionId(), finished));
        }
    }
    for (const Finished &done : finished)
    {
        if (!done.Outcome.Succeeded)
        {
            MARLIN_LOG(LogAssetIndex, Warning, "Converting {0} failed:\n{1}", done.Outcome.Job, done.Outcome.Errors);
        }
        for (const ConversionCallback &callback : done.Callbacks)
        {
            callback(done.Outcome);
        }
    }
    if (batch == nullptr)
    {
        call.SetResult(JsonValue::MakeBool(accepted[0]));
        return;
    }
    JsonValue result = JsonValue::MakeArray();
    for (const bool ok : accepted)
    {
        result.Add(JsonValue::MakeBool(ok));
    }
    call.SetResult(result);
}

void ConverterPool::SpawnWorker()
{
    const String executable = Format("{0}/MarlinConverter{1}", Engine::GetEnvironment().ExecutableDirectory,
                                     MARLIN_PLATFORM_WINDOWS ? ".exe" : "");
    // The worker count lets each worker size its job system to its share of the cores.
    const String arguments =
        Format("-Project=\"{0}\" -Indexer={1} -ConverterWorkers={2}", _projectFile, _indexerPipe, _maxWorkers);
    ProcessDesc desc;
    desc.Executable = executable.CStr();
    desc.Arguments = arguments.CStr();
    desc.Flags = ProcessFlags::Hidden | ProcessFlags::KillOnParentExit;
    ProcessHandle process;
    if (PlatformProcess::Create(desc, process))
    {
        _workers.Add(process);
    }
    else
    {
        MARLIN_LOG(LogAssetIndex, Error, "Cannot start {0}", executable);
    }
}

void ConverterPool::Tick()
{
    List<Taker> expired;
    List<ConversionCallback> failed;
    List<Taker> answered;
    List<String> jobs;
    {
        const ScopeLock lock(_lock);
        const uint64 now = PoolMilliseconds();
        for (usize i = _takers.Count(); i-- > 0;)
        {
            if (now - _takers[i].Since > TakeTimeout || !_server.IsConnected(_takers[i].Connection))
            {
                expired.Add(_takers[i]);
                _takers.RemoveAt(i);
            }
        }
        // Only the jobs workers hold can lose their worker; waiting ones are not looked at.
        List<uint64> lost;
        for (const uint64 id : _running)
        {
            const Job *job = _jobs.Find(id);
            if (job == nullptr || !_server.IsConnected(job->Worker))
            {
                lost.Add(id);
            }
        }
        for (const uint64 id : lost)
        {
            (void)_running.Remove(id);
            Job *job = _jobs.Find(id);
            if (job == nullptr)
            {
                continue;
            }
            job->Worker = 0;
            if (job->Attempts >= MaxAttempts)
            {
                for (ConversionCallback &callback : job->Callbacks)
                {
                    failed.Add(Move(callback));
                }
                RemoveLocked(id);
                ++_finished.Failed;
                continue;
            }
            QueueLocked(*job, true);
        }
        for (usize i = _workers.Count(); i-- > 0;)
        {
            if (!PlatformProcess::IsRunning(_workers[i]))
            {
                PlatformProcess::Close(_workers[i]);
                _workers.RemoveAt(i);
            }
        }
        const usize wanted = _queued < _maxWorkers ? _queued : _maxWorkers;
        if (!_shutdown && _takers.IsEmpty() && _workers.Count() < wanted)
        {
            SpawnWorker();
        }
        DispatchLocked(answered, jobs);
    }
    for (const Taker &taker : expired)
    {
        _server.Complete(taker.Deferred, EmptyTake(taker.Batch).AsView());
    }
    for (usize i = 0; i < answered.Count(); ++i)
    {
        _server.Complete(answered[i].Deferred, jobs[i].AsView());
    }
    ConversionOutcome crashed;
    crashed.Errors = "the converter crashed on it every time it was tried";
    for (const ConversionCallback &callback : failed)
    {
        callback(crashed);
    }
}

bool ConverterPool::IsIdle()
{
    const ScopeLock lock(_lock);
    return _jobs.IsEmpty();
}

ConversionProgress ConverterPool::GetProgress()
{
    const ScopeLock lock(_lock);
    ConversionProgress progress = _finished;
    progress.Queued = static_cast<uint32>(_queued);
    progress.Running = static_cast<uint32>(_running.Count());
    return progress;
}

usize ConverterPool::GetWorkerConnectionCount()
{
    const ScopeLock lock(_lock);
    List<uint32> gone;
    for (const uint32 connection : _workerConnections)
    {
        if (!_server.IsConnected(connection))
        {
            gone.Add(connection);
        }
    }
    for (const uint32 connection : gone)
    {
        (void)_workerConnections.Remove(connection);
    }
    return _workerConnections.Count();
}

void ConverterPool::Shutdown()
{
    List<ConversionCallback> callbacks;
    List<Taker> takers;
    {
        const ScopeLock lock(_lock);
        if (_shutdown)
        {
            return;
        }
        _shutdown = true;
        for (auto &entry : _jobs)
        {
            for (ConversionCallback &callback : entry.Value.Callbacks)
            {
                callbacks.Add(Move(callback));
            }
        }
        _jobs.Clear();
        _jobsByKey.Clear();
        _urgent.Clear();
        _background.Clear();
        _backgroundHead = 0;
        _queued = 0;
        _running.Clear();
        takers = Move(_takers);
        _takers.Clear();
    }
    for (const Taker &taker : takers)
    {
        _server.Fail(taker.Deferred, "the indexer is shutting down");
    }
    ConversionOutcome outcome;
    outcome.Errors = "the indexer is shutting down";
    for (const ConversionCallback &callback : callbacks)
    {
        callback(outcome);
    }
    for (const ProcessHandle &worker : _workers)
    {
        (void)PlatformProcess::Wait(worker, 2000);
        PlatformProcess::Close(worker);
    }
    _workers.Clear();
}
} // namespace Marlin
