#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetIndexClient.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "AssetPipeline/ConverterPool.h"
#include "AssetPipeline/GlobalStoreWarming.h"
#include "Concurrency/Jobs.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "HAL/PlatformCpu.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/UniquePtr.h"
#include "Misc/CommandLine.h"
#include "Net/NetLoop.h"
#include "Remote/RemoteLogStream.h"
#include "ScriptPipeline/ScriptModuleConversion.h"
#include "ShaderPipeline/GlobalShaderConversion.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace
{
constexpr Marlin::int32 IdleTakes = 3;
/// The most jobs one Converter.Take asks for: enough that cache hits do not each cost two round trips, few enough
/// that an urgent request does not wait behind a long batch.
constexpr Marlin::uint64 TakeBatch = 8;
/// Outcomes are held back at most this long to be reported together.
constexpr Marlin::uint64 FinishDelayMilliseconds = 20;

/// Sends this worker's records to the indexer, which puts them on its Log.Stream for the editor and game.
class IndexerLogSink final : public Marlin::ILogSink
{
public:
    explicit IndexerLogSink(Marlin::RemoteClient &remote)
        : _remote(remote), _source(Marlin::Format("MarlinConverter {0}", Marlin::PlatformProcess::CurrentId()))
    {
    }

    void Write(const Marlin::LogRecord &record) override
    {
        using namespace Marlin;
        // Sending can log; those records stay here.
        thread_local bool writing = false;
        if (writing || record.Level < LogLevel::Info || !_remote.IsConnected())
        {
            return;
        }
        writing = true;
        StringBuilder<512> text;
        RemoteLogStream::FormatEntry(text, record, _source.AsView());
        _remote.NotifyJson("Converter.Log", text.AsView());
        writing = false;
    }

private:
    Marlin::RemoteClient &_remote;
    Marlin::String _source;
};

Marlin::uint64 Milliseconds()
{
    using namespace Marlin;
    return PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
}

/// Reports *outcomes* in one Converter.Finish; a single one goes out as a lone outcome, which every indexer reads.
bool Finish(Marlin::AssetIndexClient &client, Marlin::Span<const Marlin::ConversionOutcome> outcomes)
{
    using namespace Marlin;
    if (outcomes.IsEmpty())
    {
        return true;
    }
    StringBuilder<4096> text;
    JsonWriter writer(text);
    if (outcomes.Count() == 1)
    {
        Json::Write(writer, outcomes[0]);
    }
    else
    {
        writer.BeginObject().Key("Outcomes").BeginArray();
        for (const ConversionOutcome &outcome : outcomes)
        {
            Json::Write(writer, outcome);
        }
        writer.EndArray().EndObject();
    }
    JsonValue params;
    JsonError parseError;
    RemoteResponse response;
    return Json::Parse(text.AsView(), params, parseError) &&
           client.GetRemote().CallSync("Converter.Finish", params, response);
}

/// The jobs a Converter.Take answered: a batch, or one job from an indexer that does not batch. Empty if none.
void ReadJobs(const Marlin::JsonValue &result, Marlin::List<Marlin::ConversionJob> &jobs)
{
    using namespace Marlin;
    StringBuilder<256> error;
    const JsonValue *batch = result.IsObject() ? result.Find("Jobs") : nullptr;
    if (batch == nullptr || !batch->IsArray())
    {
        ConversionJob job;
        if (Json::Read(result, job, error) && job.Job != 0)
        {
            jobs.Add(Move(job));
        }
        return;
    }
    for (const JsonValue &element : batch->GetElements())
    {
        ConversionJob job;
        if (Json::Read(element, job, error) && job.Job != 0)
        {
            jobs.Add(Move(job));
        }
    }
}

/// Starts a job system on this worker's share of the cores, so converters that split work into jobs, such as the
/// texture encoders, use them; with one core or less to share, jobs run inline on the converting thread.
bool StartJobs()
{
    using namespace Marlin;
    uint32 workers = 1;
    (void)CommandLine::TryGetValue("ConverterWorkers", workers);
    workers = workers == 0 ? 1 : workers;
    const uint32 share = PlatformCpu::GetTopology().LogicalCount / workers;
    if (share < 2)
    {
        return false;
    }
    JobSystemDesc desc;
    // The converting thread helps while it waits, so it counts as one of the share.
    desc.WorkerCount = share - 1;
    return JobSystem::Start(desc);
}

/// Checks the object jobs of a batch for hits, target by target, each in one ConvertMany: two round trips for the
/// lot. *checked* holds a result for each job, Missing (or failed) for those to convert.
void CheckBatch(Marlin::AssetConversion &conversion, Marlin::Span<const Marlin::ConversionJob> jobs,
                Marlin::List<Marlin::ConversionResult> &checked)
{
    using namespace Marlin;
    checked.Clear();
    checked.Resize(jobs.Count());
    List<bool> done;
    done.Resize(jobs.Count(), false);
    for (usize first = 0; first < jobs.Count(); ++first)
    {
        if (done[first] || !jobs[first].Shader.IsEmpty() || !jobs[first].Script.IsEmpty())
        {
            continue;
        }
        const ConversionTarget &target = jobs[first].Target;
        List<Guid> uuids;
        List<usize> slots;
        for (usize i = first; i < jobs.Count(); ++i)
        {
            if (!done[i] && jobs[i].Shader.IsEmpty() && jobs[i].Script.IsEmpty() &&
                jobs[i].Target.Platform == target.Platform && jobs[i].Target.Kind == target.Kind)
            {
                uuids.Add(jobs[i].Uuid);
                slots.Add(i);
                done[i] = true;
            }
        }
        List<ConversionResult> results;
        conversion.ConvertMany(uuids.AsSpan(), target, results, false);
        for (usize k = 0; k < slots.Count(); ++k)
        {
            checked[slots[k]] = Move(results[k]);
        }
    }
}

Marlin::ConversionOutcome Convert(Marlin::AssetConversion &conversion, Marlin::GlobalShaderConversion &shaders,
                                  Marlin::ScriptModuleConversion &scripts, const Marlin::ConversionJob &job,
                                  Marlin::ConversionResult checked)
{
    using namespace Marlin;
    ConversionOutcome outcome;
    outcome.Job = job.Job;
    if (!job.Shader.IsEmpty())
    {
        // Debug information maps the code back to the source for capture tools and GPU crash dumps (08 §7); Span
        // accesses out of range are reported (MarlinLanguage.md §9.11).
        // A material pass's key carries its material and variant, which the conversion reads from it.
        ShaderCompileOptions options;
        options.DebugOutput = true;
        options.CheckSpans = true;
        options.DebugInfo = true;
        GlobalShaderConversionResult shader = shaders.Convert(job.Shader.AsView(), job.Target, options);
        outcome.Succeeded = shader.Succeeded;
        outcome.CacheHit = shader.CacheHit;
        outcome.Key = FormatResultKey(shader.Key);
        outcome.Location = shader.Location;
        outcome.Errors = Move(shader.Errors);
        outcome.Files = Move(shader.Files);
        return outcome;
    }
    if (!job.Script.IsEmpty())
    {
        ScriptModuleConversionResult script = scripts.Convert(job.Script.AsView(), job.Target, {.DebugOutput = true});
        outcome.Succeeded = script.Succeeded;
        outcome.CacheHit = script.CacheHit;
        outcome.Key = FormatResultKey(script.Key);
        outcome.Location = script.Location;
        outcome.Errors = Move(script.Errors);
        outcome.Files = Move(script.Files);
        return outcome;
    }
    ConversionResult result = checked.Succeeded ? Move(checked) : conversion.Convert(job.Uuid, job.Target);
    // RECOVERY-DECISION AP-D5: a source edited after the index scanned it fails as stale and stores nothing; the
    // indexer is following the edit, so try again a few times before reporting the failure.
    for (int32 attempt = 0; !result.Succeeded && !result.Stale.IsEmpty() && attempt < 5; ++attempt)
    {
        PlatformThread::Sleep(100);
        result = conversion.Convert(job.Uuid, job.Target);
    }
    outcome.Succeeded = result.Succeeded;
    outcome.CacheHit = result.CacheHit;
    outcome.Key = FormatResultKey(result.Key);
    outcome.Location = result.Location;
    outcome.Errors = result.Errors;
    for (const DependencyInput &input : result.Inputs)
    {
        if (input.Kind == DependencyInputKind::Config)
        {
            outcome.ReadConfig = true;
        }
        else if (input.Kind == DependencyInputKind::Converter)
        {
            // A material's reflected struct (MaterialConverter): whatever shader source the struct's compile reads.
            outcome.ReadLanguage = outcome.ReadLanguage || input.Section.AsView().StartsWith("MaterialShader:");
        }
        else
        {
            outcome.Inputs.Add(input.Uuid);
        }
    }
    return outcome;
}

/// Convert, logging what the worker actually converted; cache hits go into the batch summary, and failures the
/// indexer logs.
Marlin::ConversionOutcome ConvertAndLog(Marlin::AssetIndexClient &client, Marlin::AssetConversion &conversion,
                                        Marlin::GlobalShaderConversion &shaders,
                                        Marlin::ScriptModuleConversion &scripts, const Marlin::ConversionJob &job,
                                        Marlin::ConversionResult checked)
{
    using namespace Marlin;
    const uint64 started = Milliseconds();
    ConversionOutcome outcome = Convert(conversion, shaders, scripts, job, Move(checked));
    if (!outcome.Succeeded || outcome.CacheHit)
    {
        return outcome;
    }
    if (!job.Shader.IsEmpty())
    {
        MARLIN_LOG(LogAssetIndex, Info, "Compiled shader {0} for {1} in {2} ms", job.Shader, job.Target.Platform,
                   Milliseconds() - started);
        return outcome;
    }
    if (!job.Script.IsEmpty())
    {
        MARLIN_LOG(LogAssetIndex, Info, "Compiled the {0} scripts for {1} in {2} ms", job.Script, job.Target.Platform,
                   Milliseconds() - started);
        return outcome;
    }
    AssetObjectInfo info;
    if (client.Find(job.Uuid, info))
    {
        MARLIN_LOG(LogAssetIndex, Debug, "Converted {0} {1} ({2}) for {3} in {4} ms", info.Type, info.Name, info.Path,
                   job.Target.Platform, Milliseconds() - started);
    }
    else
    {
        MARLIN_LOG(LogAssetIndex, Debug, "Converted {0} for {1} in {2} ms", job.Uuid, job.Target.Platform,
                   Milliseconds() - started);
    }
    return outcome;
}
} // namespace

/// `-Warm -Platforms=Windows,PS5 [-Threads=N]`: converts everything the project has for each platform into GlobalStore,
/// as CI does after each commit.
int Warm()
{
    using namespace Marlin;
    if (!Engine::GetEnvironment().HasProject())
    {
        MARLIN_LOG(LogAssetIndex, Error, "MarlinConverter -Warm needs -Project=<path to .mproject>");
        return 1;
    }
    GlobalStoreWarmingDesc desc;
    StringView platforms = Engine::GetEnvironment().Platform;
    (void)CommandLine::TryGetValue("Platforms", platforms);
    while (!platforms.IsEmpty())
    {
        StringView platform = platforms;
        StringView rest;
        if (!platforms.SplitFirst(',', platform, rest))
        {
            rest = {};
        }
        if (!platform.Trim().IsEmpty())
        {
            ConversionTarget &target = desc.Targets.AddDefaulted();
            target.Platform = platform.Trim();
            target.Kind = CommandLine::HasSwitch("Server") ? TargetKind::Server : TargetKind::Client;
        }
        platforms = rest;
    }
    (void)CommandLine::TryGetValue("Threads", desc.Threads);
    // Converters that split their work into jobs, such as the mesh DAG builder, share every core with the warming
    // threads, which help while they wait.
    JobSystemDesc jobDesc;
    jobDesc.WorkerCount = Max(PlatformCpu::GetTopology().LogicalCount, 2u) - 1;
    const bool jobs = JobSystem::Start(jobDesc);
    AssetIndex index;
    // From the indexer's snapshot, the scan reads only what changed since.
    (void)LoadProjectIndex(index);
    AssetScanner scanner(index, GetProjectContentFolders());
    List<String> changed;
    scanner.ScanAll(changed);
    StringBuilder<4096> failures;
    const GlobalStoreWarmingStats stats = WarmGlobalStore(index, desc, &failures);
    MARLIN_LOG(LogAssetIndex, Info, "Warmed GlobalStore for {0} targets: {1} converted, {2} cached, {3} failed",
               desc.Targets.Count(), stats.Conversions, stats.CacheHits, stats.Failures);
    if (stats.Failures != 0)
    {
        MARLIN_LOG(LogAssetIndex, Error, "Conversions failed:\n{0}", failures);
    }
    if (jobs)
    {
        JobSystem::Stop();
    }
    return stats.Failures == 0 ? 0 : 1;
}

/// `-Import=/Project/Content/Rock.png,/Project/Content/Ship.fbx`: imports each file into the .masset beside it, as
/// the editor does, for batch imports and CI. Objects the files already have are kept. `-ImportOptions=<fields>`
/// sets the importer's options, as the editor's import window would, and keeps them for later imports.
int Import()
{
    using namespace Marlin;
    StringView paths;
    if (!Engine::GetEnvironment().HasProject() || !CommandLine::TryGetValue("Import", paths))
    {
        MARLIN_LOG(LogAssetIndex, Error, "MarlinConverter -Import=<virtual paths> needs -Project=<path to .mproject>");
        return 1;
    }
    AssetIndex index;
    // From the indexer's snapshot, the scan reads only what changed since.
    (void)LoadProjectIndex(index);
    AssetScanner scanner(index, GetProjectContentFolders());
    List<String> changed;
    scanner.ScanAll(changed);
    AssetImport import;
    uint32 failures = 0;
    while (!paths.IsEmpty())
    {
        StringView path = paths;
        StringView rest;
        if (!paths.SplitFirst(',', path, rest))
        {
            rest = {};
        }
        paths = rest;
        path = path.Trim();
        AssetFileRecord record;
        if (path.IsEmpty())
        {
            continue;
        }
        if (!index.GetRecord(path, record) || !record.BinaryGuid.IsValid())
        {
            MARLIN_LOG(LogAssetIndex, Error, "{0} is not a source file of the project", path);
            ++failures;
            continue;
        }
        // -ImportOptions="Combine = false": the fields of the importer's options, which replace the kept ones.
        UniquePtr<ReflectedValue> options;
        StringView optionsFields;
        if (CommandLine::TryGetValue("ImportOptions", optionsFields))
        {
            const AssetImporter *importer = import.FindImporter(path, record.BinaryGuid);
            const TypeInfo *type = importer != nullptr ? importer->GetOptionsType() : nullptr;
            if (type == nullptr)
            {
                MARLIN_LOG(LogAssetIndex, Error, "The importer of {0} takes no options", path);
                ++failures;
                continue;
            }
            const String text = Format("{0} {{ {1} }}", type->GetName(), optionsFields);
            MarlinTextDocument document;
            MarlinTextDiagnostics diagnostics;
            options = MakeUnique<ReflectedValue>(*type);
            const MarlinTextObject *object =
                document.Parse(text.AsView(), diagnostics) ? document.GetObject(0, diagnostics) : nullptr;
            if (object == nullptr || !MarlinTextSerializer::ReadObject(*object, *type, options->Get(), diagnostics) ||
                diagnostics.HasErrors())
            {
                MARLIN_LOG(LogAssetIndex, Error, "-ImportOptions for {0}: {1}", path, diagnostics.ToString());
                ++failures;
                continue;
            }
        }
        const ImportResult result =
            import.Import(path, record.BinaryGuid, options != nullptr ? options->Get() : nullptr);
        if (!result.Succeeded)
        {
            MARLIN_LOG(LogAssetIndex, Error, "Importing {0} failed: {1}", path, result.Errors);
            ++failures;
            continue;
        }
        MARLIN_LOG(LogAssetIndex, Info, "Imported {0} into {1}: {2} added, {3} kept", path, result.AssetPath,
                   result.Added, result.Kept);
    }
    return failures == 0 ? 0 : 1;
}

int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinConverter",
                                 ProgramFeatures::Vfs | ProgramFeatures::Project | ProgramFeatures::Config))
    {
        return 1;
    }
    if (StringView imports; CommandLine::TryGetValue("Import", imports))
    {
        const int exitCode = Import();
        ProgramHost::Shutdown();
        return exitCode;
    }
    if (CommandLine::HasSwitch("Warm"))
    {
        const int exitCode = Warm();
        ProgramHost::Shutdown();
        return exitCode;
    }
    StringView pipe;
    NetLoop loop;
    (void)loop.Start(1);
    AssetIndexClient client(loop);
    if (!CommandLine::TryGetValue("Indexer", pipe) || !client.ConnectPipe(pipe, 2000))
    {
        MARLIN_LOG(LogAssetIndex, Error, "MarlinConverter needs -Indexer=<pipe> of a running MarlinAssetIndexer");
        ProgramHost::Shutdown();
        return 1;
    }
    IndexerLogSink logSink(client.GetRemote());
    Log::AddSink(&logSink);
    const bool jobs = StartJobs();
    int32 idle = 0;
    {
        ClientSourceResolver resolver(client);
        ConversionStore store;
        AssetConversion conversion(resolver, store);
        GlobalShaderConversion shaders(store);
        ScriptModuleConversion scripts(store);
        JsonValue take = JsonValue::MakeObject();
        take.Set("Max", JsonValue::MakeUnsigned(TakeBatch));
        bool connected = true;
        // The indexer's ConfigGeneration this worker's config is known to be as new as.
        uint64 configGeneration = 0;
        while (connected && idle < IdleTakes && client.IsConnected())
        {
            RemoteResponse response;
            if (!client.GetRemote().CallSync("Converter.Take", take, response, 60000))
            {
                break;
            }
            List<ConversionJob> taken;
            ReadJobs(response.Result, taken);
            if (taken.IsEmpty())
            {
                ++idle;
                continue;
            }
            idle = 0;
            uint64 newest = 0;
            for (const ConversionJob &job : taken)
            {
                newest = Max(newest, job.ConfigGeneration);
            }
            if (newest > configGeneration)
            {
                // Config may have changed since this worker read it (a worker started after the change reads it
                // once more for nothing); the indexer converts what read it again.
                Config::Reload();
                configGeneration = newest;
            }
            List<ConversionResult> checked;
            CheckBatch(conversion, taken.AsSpan(), checked);
            List<ConversionOutcome> outcomes;
            const uint64 batchStart = Milliseconds();
            uint64 heldSince = 0;
            usize hits = 0;
            usize failed = 0;
            for (usize i = 0; i < taken.Count(); ++i)
            {
                ConversionOutcome outcome =
                    ConvertAndLog(client, conversion, shaders, scripts, taken[i], Move(checked[i]));
                hits += outcome.Succeeded && outcome.CacheHit ? 1 : 0;
                failed += outcome.Succeeded ? 0 : 1;
                outcomes.Add(Move(outcome));
                Log::Flush();
                const uint64 now = Milliseconds();
                heldSince = heldSince == 0 ? now : heldSince;
                if (now - heldSince >= FinishDelayMilliseconds)
                {
                    connected = Finish(client, outcomes.AsSpan());
                    outcomes.Clear();
                    heldSince = 0;
                    if (!connected)
                    {
                        break;
                    }
                }
            }
            const usize converted = taken.Count() - hits - failed;
            if (converted != 0)
            {
                MARLIN_LOG(LogAssetIndex, Info, "Converted {0} of {1} taken in {2} ms ({3} cached, {4} failed)",
                           converted, taken.Count(), Milliseconds() - batchStart, hits, failed);
            }
            else if (hits != 0)
            {
                MARLIN_LOG(LogAssetIndex, Debug, "Cache: {0} of {1} taken were already in GlobalStore", hits,
                           taken.Count());
            }
            connected = connected && Finish(client, outcomes.AsSpan());
        }
    }
    Log::Flush();
    Log::RemoveSink(&logSink);
    client.Disconnect();
    if (jobs)
    {
        JobSystem::Stop();
    }
    ProgramHost::Shutdown();
    return 0;
}
