#include "AssetPipeline/AssetIndexService.h"

#include "AssetPipeline/AssetJournal.h"
#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashSet.h"
#include "HAL/PlatformDirectoryWatcher.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformTime.h"
#include "Json/JsonSerializer.h"
#include "Json/JsonWriter.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
/// The log is folded into the snapshot once it grows past this.
constexpr uint64 MaxLogSize = uint64{64} << 20;

template <typename T> bool ReadParams(RemoteCall &call, T &params)
{
    StringBuilder<256> error;
    if (!Json::Read(call.GetParams(), params, error))
    {
        call.Fail(error.AsView(), RemoteErrors::InvalidParams);
        return false;
    }
    return true;
}

template <typename T> void SetResult(RemoteCall &call, const T &value)
{
    StringBuilder<4096> text;
    JsonWriter writer(text);
    Json::Write(writer, value);
    call.SetResultJson(text.AsView());
}

String ShaderLocatedKey(const ShaderLocateParams &params)
{
    return Format("Shader/{0}/{1}/{2}", params.EntryPoint, params.Target.Platform,
                  params.Target.Kind == TargetKind::Server ? "Server" : "Client");
}

String ScriptLocatedKey(const ScriptLocateParams &params)
{
    return Format("Script/{0}/{1}/{2}", params.Package, params.Target.Platform,
                  params.Target.Kind == TargetKind::Server ? "Server" : "Client");
}

/// One watched native folder and the virtual folder it shows.
struct Watch
{
    UniquePtr<DirectoryWatcher> Watcher;
    String Folder;
};
} // namespace

AssetIndexService::AssetIndexService(NetLoop &loop, AssetIndexServiceDesc desc)
    : _loop(loop), _desc(Move(desc)), _scanner(_index, _desc.Folders)
{
}

AssetIndexService::~AssetIndexService()
{
    Stop();
}

String AssetIndexService::LogPath() const
{
    return Format("{0}/Index.log", _desc.StoreDirectory);
}

bool AssetIndexService::Start()
{
    // The log is deleted under StoreDirectory; an unset one would put it at the root of the drive.
    if (_desc.StoreDirectory.IsEmpty() || !PlatformFile::CreateDirectory(_desc.StoreDirectory.CStr()))
    {
        MARLIN_LOG(LogAssetIndex, Error, "The asset index has no store directory");
        return false;
    }
    _index.SetSnapshotDirectory(_desc.StoreDirectory.AsView());
    _loaded = _desc.Seed.IsEmpty() ? _index.LoadLatest() : _index.Load(_desc.Seed.AsView());
    if (_loaded)
    {
        const AssetIndexCounts &counts = _index.GetCounts();
        MARLIN_LOG(LogAssetIndex, Info, "Loaded the asset index from {0}: {1} files, {2} objects", _index.GetBasePath(),
                   counts.Files, counts.Objects);
    }
    if (!_publisher.Open(_desc.PipeName.AsView()))
    {
        MARLIN_LOG(LogAssetIndex, Warning, "The asset index cannot be shared; clients fall back to the pipe");
    }
    if (_desc.Seed.IsEmpty() && _index.ReplayLog(LogPath().AsView()))
    {
        FileInfo info;
        _logSize = PlatformFile::GetInfo(LogPath().CStr(), info) ? info.Size : 0;
    }
    if (_loaded && _desc.Seed.IsEmpty() && !_desc.ProjectFile.IsEmpty())
    {
        // Before the scan thread replays the journal, so what changed while stopped invalidates them.
        LoadLocated();
    }

    RemoteServerDesc server;
    server.PipeName = _desc.PipeName;
    _server = MakeUnique<RemoteServer>(_loop, Move(server));
    _server->AddStream("AssetIndex.Changed", "Paths: the files whose rows changed, after each scan step.");
    _server->AddStream("Asset.Changed",
                       "Uuids: objects whose converted data a source change made stale; loaders reload them.");
    _server->AddStream(RemoteLogStream::Stream, "The log records of the indexer and of its converter workers; Source "
                                                "names the worker a record came from.");
    _server->AddMethod(
        "Converter.Log",
        [this](RemoteCall &call) {
            if (_server->HasSubscribers(RemoteLogStream::Stream))
            {
                StringBuilder<512> text;
                JsonWriter(text).Value(call.GetParams());
                _server->PublishJson(RemoteLogStream::Stream, text.AsView());
            }
        },
        RemoteThread::Any, "A notification of a worker: one of its log records, as Log.Stream carries it.");
    AddMethods();
    if (!_desc.ProjectFile.IsEmpty())
    {
        _converters = MakeUnique<ConverterPool>(*_server, _desc.ProjectFile.AsView(), _desc.PipeName.AsView(),
                                                _desc.MaxConverters);
        _server->AddMethod(
            "Asset.Locate", [this](RemoteCall &call) { Locate(call); }, RemoteThread::Any,
            "Where the converted data of an object is for a target, converting it on a miss.");
        _server->AddMethod(
            "Shader.Locate", [this](RemoteCall &call) { LocateShader(call); }, RemoteThread::Any,
            "Where the converted data of a global shader is for a target, by entry point, compiling it on a miss.");
        _server->AddMethod(
            "Script.Locate", [this](RemoteCall &call) { LocateScript(call); }, RemoteThread::Any,
            "Where the converted data of a script package's module is for a target, compiling it on a miss.");
    }
    if (!_server->Start())
    {
        return false;
    }
    _logStream.Server = _server.Get();
    Log::AddSink(&_logStream);
    _stopping.Store(false);
    ThreadDesc thread;
    thread.Name = "Marlin Asset Scan";
    thread.Entry = &ScanThreadMain;
    thread.Argument = this;
    _scanThread = PlatformThread::Create(thread);
    if (_converters != nullptr)
    {
        ThreadDesc shaders;
        shaders.Name = "Marlin Shader Watch";
        shaders.Entry = &ShaderWatchMain;
        shaders.Argument = this;
        _shaderThread = PlatformThread::Create(shaders);
    }
    return _scanThread.IsValid();
}

void AssetIndexService::Stop()
{
    if (!_scanThread.IsValid())
    {
        return;
    }
    _stopping.Store(true, MemoryOrder::Release);
    PlatformThread::Join(_scanThread);
    _scanThread = {};
    if (_shaderThread.IsValid())
    {
        PlatformThread::Join(_shaderThread);
        _shaderThread = {};
    }
    if (_converters != nullptr)
    {
        _converters->Shutdown();
    }
    Log::Flush();
    Log::RemoveSink(&_logStream);
    _logStream.Server = nullptr;
    _server->Stop();
    _index.Compact();
    if (!_index.GetBasePath().IsEmpty())
    {
        (void)PlatformFile::Delete(LogPath().CStr());
        _logSize = 0;
        if (_converters != nullptr)
        {
            // The snapshot just written is what the kept locations were made from.
            SaveLocated();
        }
    }
    _publisher.Close();
}

String AssetIndexService::LocatedPath() const
{
    return Format("{0}/Located.json", _desc.StoreDirectory);
}

void AssetIndexService::SaveLocated()
{
    AssetLocatedRecords records;
    {
        const ScopeLock lock(_locatedLock);
        for (const auto &[key, located] : _located)
        {
            if (!located.Shader.IsEmpty() || !located.Script.IsEmpty() || located.ReadConfig || located.ReadLanguage)
            {
                continue;
            }
            AssetLocatedRecord &record = records.Entries.AddDefaulted();
            record.Key = key;
            record.Uuid = located.Uuid;
            record.Location = located.Location;
            record.Paths = located.Paths;
            record.Target = located.Target;
        }
    }
    StringBuilder<4096> text;
    JsonWriter writer(text);
    Json::Write(writer, records);
    const FileHandle file = PlatformFile::Open(LocatedPath().CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (file.IsValid())
    {
        (void)PlatformFile::Write(file, text.AsView().Data(), text.AsView().Length());
        PlatformFile::Close(file);
    }
}

void AssetIndexService::LoadLocated()
{
    const String path = LocatedPath();
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return;
    }
    uint64 size = 0;
    usize read = 0;
    String text;
    const bool readable = PlatformFile::GetSize(file, size) && (text.Resize(static_cast<usize>(size)), true) &&
                          PlatformFile::Read(file, text.Data(), text.Length(), read) && read == text.Length();
    PlatformFile::Close(file);
    (void)PlatformFile::Delete(path.CStr());
    JsonValue value;
    JsonError parseError;
    AssetLocatedRecords records;
    StringBuilder<256> error;
    if (!readable || !Json::Parse(text.AsView(), value, parseError) || !Json::Read(value, records, error))
    {
        return;
    }
    // Only what GlobalStore still has: its data may have been cleared while the indexer was stopped.
    List<Hash128> keys;
    List<const AssetLocatedRecord *> parsed;
    for (const AssetLocatedRecord &record : records.Entries)
    {
        Hash128 key;
        if (ParseResultKey(record.Location.Key.AsView(), key))
        {
            keys.Add(key);
            parsed.Add(&record);
        }
    }
    ConversionStoreDesc storeDesc = ConversionStoreDesc::FromConfig();
    if (_desc.GlobalStorePort != 0)
    {
        storeDesc.Host = _desc.GlobalStoreHost;
        storeDesc.Port = _desc.GlobalStorePort;
    }
    ConversionStore store(Move(storeDesc));
    List<bool> found;
    if (keys.IsEmpty() || !store.HasResults(keys.AsSpan(), found))
    {
        return;
    }
    const ScopeLock lock(_locatedLock);
    for (usize i = 0; i < parsed.Count(); ++i)
    {
        if (!found[i])
        {
            continue;
        }
        Located &located = _located.FindOrAdd(parsed[i]->Key);
        located.Uuid = parsed[i]->Uuid;
        located.Location = parsed[i]->Location;
        located.Paths = parsed[i]->Paths;
        located.Target = parsed[i]->Target;
    }
    MARLIN_LOG(LogAssetIndex, Info, "Kept {0} located objects from the last run", _located.Count());
}

bool AssetIndexService::IsIdle()
{
    if (IsScanning() || (_converters != nullptr && !_converters->IsIdle()))
    {
        return false;
    }
    const usize workers = _converters != nullptr ? _converters->GetWorkerConnectionCount() : 0;
    return _server->GetConnectionCount() <= workers;
}

void AssetIndexService::Tick()
{
    _server->ProcessPending();
    if (_converters != nullptr)
    {
        _converters->Tick();
    }
}

void AssetIndexService::Locate(RemoteCall &call)
{
    AssetLocateParams params;
    if (!ReadParams(call, params))
    {
        return;
    }
    const RemoteDeferred deferred = call.Defer();
    {
        const ScopeLock lock(_locatedLock);
        if (IsScanning())
        {
            _waitingForScan.Add({Move(params), deferred});
            return;
        }
    }
    Resolve(params, deferred);
}

void AssetIndexService::Resolve(const AssetLocateParams &params, const RemoteDeferred &deferred)
{
    StringBuilder<96> key;
    params.Uuid.FormatTo(key, "N");
    key.Append('/').Append(params.Target.Platform).Append('/');
    key.Append(params.Target.Kind == TargetKind::Server ? "Server" : "Client");
    {
        const ScopeLock lock(_locatedLock);
        if (const Located *located = _located.Find(key.ToString()))
        {
            StringBuilder<512> text;
            JsonWriter writer(text);
            Json::Write(writer, located->Location);
            _server->Complete(deferred, text.AsView());
            return;
        }
    }
    const String cacheKey = key.ToString();
    AssetObjectInfo object;
    NotifyConverting(deferred.ConnectionId, params.Uuid, _index.Find(params.Uuid, object) ? object.Name.AsView() : "",
                     false);
    uint64 epoch = 0;
    {
        const ScopeLock lock(_locatedLock);
        epoch = _epoch;
    }
    _converters->Request(params.Uuid, params.Target,
                         params.Urgent ? ConversionPriority::Urgent : ConversionPriority::Background,
                         [this, deferred, cacheKey, epoch, params](const ConversionOutcome &outcome) {
                             if (!outcome.Succeeded)
                             {
                                 _server->Fail(deferred, outcome.Errors.AsView());
                                 return;
                             }
                             Remember(cacheKey, MakeLocated(params.Uuid, params.Target, outcome), epoch);
                             StringBuilder<512> text;
                             JsonWriter writer(text);
                             Json::Write(writer, outcome.Location);
                             _server->Complete(deferred, text.AsView());
                         });
}

AssetIndexService::Located AssetIndexService::MakeLocated(const Guid &uuid, const ConversionTarget &target,
                                                          const ConversionOutcome &outcome)
{
    Located located;
    located.Uuid = uuid;
    located.Location = outcome.Location;
    located.Target = target;
    located.ReadConfig = outcome.ReadConfig;
    located.ReadLanguage = outcome.ReadLanguage;
    AssetObjectInfo info;
    if (_index.Find(uuid, info))
    {
        located.Paths.Add(info.Path);
    }
    for (const Guid &input : outcome.Inputs)
    {
        AssetBinaryInfo binary;
        if (_index.Find(input, info))
        {
            located.Paths.Add(info.Path);
        }
        else if (_index.FindBinary(input, binary))
        {
            located.Paths.Add(binary.Path);
        }
    }
    return located;
}

void AssetIndexService::Remember(const String &key, Located located, uint64 epoch)
{
    const ScopeLock lock(_locatedLock);
    if (epoch == _epoch)
    {
        _located.FindOrAdd(key) = Move(located);
    }
}

void AssetIndexService::LocateShader(RemoteCall &call)
{
    ShaderLocateParams params;
    if (!ReadParams(call, params))
    {
        return;
    }
    if (params.EntryPoint.IsEmpty() || !params.Uuid.IsValid())
    {
        call.Fail("no entry point or Guid", RemoteErrors::InvalidParams);
        return;
    }
    {
        const ScopeLock lock(_locatedLock);
        if (const Located *located = _located.Find(ShaderLocatedKey(params)))
        {
            SetResult(call, located->Location);
            return;
        }
    }
    const RemoteDeferred deferred = call.Defer();
    const String entryPoint = params.EntryPoint;
    NotifyConverting(deferred.ConnectionId, params.Uuid, entryPoint.AsView(), true);
    uint64 epoch = 0;
    {
        const ScopeLock lock(_locatedLock);
        epoch = _epoch;
    }
    _converters->Request(
        params.Uuid, params.Target, params.Urgent ? ConversionPriority::Urgent : ConversionPriority::Background,
        [this, deferred, params, epoch](const ConversionOutcome &outcome) {
            if (!outcome.Succeeded)
            {
                _server->Fail(deferred, outcome.Errors.AsView());
                return;
            }
            AddLocatedShader(params, outcome, epoch);
            StringBuilder<512> text;
            JsonWriter writer(text);
            Json::Write(writer, outcome.Location);
            _server->Complete(deferred, text.AsView());
        },
        entryPoint.AsView());
}

void AssetIndexService::LocateScript(RemoteCall &call)
{
    ScriptLocateParams params;
    if (!ReadParams(call, params))
    {
        return;
    }
    if (params.Package.IsEmpty() || !params.Uuid.IsValid())
    {
        call.Fail("no package or Guid", RemoteErrors::InvalidParams);
        return;
    }
    {
        const ScopeLock lock(_locatedLock);
        if (const Located *located = _located.Find(ScriptLocatedKey(params)))
        {
            SetResult(call, located->Location);
            return;
        }
    }
    const RemoteDeferred deferred = call.Defer();
    const String package = params.Package;
    // The splash shows it as compiling, like a shader.
    NotifyConverting(deferred.ConnectionId, params.Uuid, package.AsView(), true);
    uint64 epoch = 0;
    {
        const ScopeLock lock(_locatedLock);
        epoch = _epoch;
    }
    _converters->Request(
        params.Uuid, params.Target, params.Urgent ? ConversionPriority::Urgent : ConversionPriority::Background,
        [this, deferred, params, epoch](const ConversionOutcome &outcome) {
            if (!outcome.Succeeded)
            {
                _server->Fail(deferred, outcome.Errors.AsView());
                return;
            }
            AddLocatedScript(params, outcome, epoch);
            StringBuilder<512> text;
            JsonWriter writer(text);
            Json::Write(writer, outcome.Location);
            _server->Complete(deferred, text.AsView());
        },
        {}, package.AsView());
}

void AssetIndexService::AddLocatedScript(const ScriptLocateParams &params, const ConversionOutcome &outcome,
                                         uint64 epoch)
{
    Located located;
    located.Uuid = params.Uuid;
    located.Location = outcome.Location;
    located.Paths = outcome.Files;
    located.Target = params.Target;
    located.Script = params.Package;
    Remember(ScriptLocatedKey(params), Move(located), epoch);
}

void AssetIndexService::NotifyConverting(uint32 connection, const Guid &uuid, StringView name, bool shader)
{
    StringBuilder<256> text;
    JsonWriter writer(text);
    StringBuilder<40> formatted;
    uuid.FormatTo(formatted, {});
    writer.BeginObject().Key("Uuid").String(formatted.AsView()).Key("Name").String(name);
    writer.Key("Shader").Bool(shader).EndObject();
    _server->Notify(connection, "Asset.Converting", text.AsView());
}

void AssetIndexService::AddLocatedShader(const ShaderLocateParams &params, const ConversionOutcome &outcome,
                                         uint64 epoch)
{
    Located located;
    located.Uuid = params.Uuid;
    located.Location = outcome.Location;
    located.Paths = outcome.Files;
    located.Target = params.Target;
    located.Shader = params.EntryPoint;
    // A file the shader did not use may still change it by declaring a name it looked up (ShaderNameRecord).
    located.ReadLanguage = true;
    Remember(ShaderLocatedKey(params), Move(located), epoch);
}

void AssetIndexService::WarmShaders(Span<const ShaderLocateParams> shaders)
{
    if (_converters == nullptr || shaders.IsEmpty())
    {
        return;
    }
    const uint32 count = static_cast<uint32>(shaders.Count());
    const float64 started = PlatformTime::Seconds();
    _shadersWarmed.Store(0, MemoryOrder::Release);
    _shadersFailed.Store(0, MemoryOrder::Release);
    _shadersToWarm.Store(count, MemoryOrder::Release);
    MARLIN_LOG(LogAssetIndex, Info, "Compiling {0} global shaders for {1}", count, shaders[0].Target.Platform.AsView());
    uint64 epoch = 0;
    {
        const ScopeLock lock(_locatedLock);
        epoch = _epoch;
    }
    for (const ShaderLocateParams &shader : shaders)
    {
        _converters->Request(
            shader.Uuid, shader.Target, ConversionPriority::Background,
            [this, shader, count, epoch, started](const ConversionOutcome &outcome) {
                if (outcome.Succeeded)
                {
                    AddLocatedShader(shader, outcome, epoch);
                }
                else
                {
                    _shadersFailed.FetchAdd(1);
                    MARLIN_LOG(LogAssetIndex, Warning, "{0} does not compile:\n{1}", shader.EntryPoint, outcome.Errors);
                }
                const uint32 warmed = _shadersWarmed.FetchAdd(1) + 1;
                if (warmed == count)
                {
                    const uint32 failed = _shadersFailed.Load(MemoryOrder::Acquire);
                    MARLIN_LOG(LogAssetIndex, Info, "Compiled {0} of {1} global shaders in {2:F0} ms, {3} failed",
                               warmed - failed, count, (PlatformTime::Seconds() - started) * 1000.0, failed);
                }
            },
            shader.EntryPoint.AsView());
    }
}

void AssetIndexService::ShaderWatchMain(void *argument)
{
    static_cast<AssetIndexService *>(argument)->WatchShaders();
}

void AssetIndexService::WatchShaders()
{
    // RECOVERY-DECISION AP-D2: the shader source sets watched are `<root>/Language` beside every indexed content
    // folder (MarlinLanguage.md §1.3, §1.6), each layer of them; a folder no layer has is not created, only skipped.
    // `<root>/Config` is watched the same way, for the conversions that read config (06 §3.2).
    List<Watch> watches;
    for (const String &folder : _desc.Folders)
    {
        StringView root;
        StringView name;
        if (!folder.AsView().SplitLast('/', root, name))
        {
            continue;
        }
        for (const StringView sub : {StringView("Language"), StringView("Config")})
        {
            const String watched = Format("{0}/{1}", root, sub);
            bool seen = false;
            for (const Watch &existing : watches)
            {
                seen = seen || existing.Folder == watched;
            }
            FileInfo info;
            if (seen || !Vfs::GetInfo(watched.AsView(), info) || !info.IsDirectory)
            {
                continue;
            }
            for (const String &physical : GetPhysicalFolders(watched.AsView()))
            {
                Watch &watch = watches.AddDefaulted();
                watch.Watcher = MakeUnique<DirectoryWatcher>();
                watch.Folder = watched;
                if (!watch.Watcher->Open(physical.CStr(), true))
                {
                    watches.Pop();
                }
            }
        }
    }
    while (!_stopping.Load(MemoryOrder::Acquire))
    {
        bool any = false;
        for (Watch &watch : watches)
        {
            if (!watch.Watcher->Wait(10))
            {
                continue;
            }
            any = true;
            List<String> changed;
            HashSet<String> seen;
            FileChange change;
            while (watch.Watcher->Next(change))
            {
                if (change.Kind == FileChangeKind::Overflow)
                {
                    // Which files changed is lost; anything read below the folder may be stale. A file of the
                    // folder's kind stands for the ones not read by path (Invalidate).
                    const ScopeLock lock(_locatedLock);
                    for (const auto &entry : _located)
                    {
                        for (const String &path : entry.Value.Paths)
                        {
                            if (path.AsView().StartsWith(watch.Folder.AsView()) && seen.Add(path))
                            {
                                changed.Add(path);
                            }
                        }
                    }
                    String any = Format("{0}/{1}", watch.Folder,
                                        watch.Folder.AsView().EndsWith("/Config") ? "Any.mtext" : "Any.mcommon");
                    if (seen.Add(any))
                    {
                        changed.Add(Move(any));
                    }
                    continue;
                }
                String path = Format("{0}/{1}", watch.Folder, StringView(change.Path));
                if (seen.Add(path))
                {
                    changed.Add(Move(path));
                }
            }
            if (!changed.IsEmpty())
            {
                Invalidate(changed);
            }
        }
        if (!any)
        {
            PlatformThread::Sleep(watches.IsEmpty() ? 50 : 10);
        }
    }
}

void AssetIndexService::Invalidate(const List<String> &changed)
{
    // A set, so the cost is one lookup per path a located result read, not one per changed path as well.
    HashSet<StringView> changedPaths;
    // A script module depends on every script source file, so a new one invalidates it too. A shader, or a
    // material that reflects one, may be changed by any language file (ShaderNameRecord); what read config by
    // any config file.
    bool scriptChanged = false;
    bool languageChanged = false;
    bool configChanged = false;
    for (const String &path : changed)
    {
        const StringView view = path.AsView();
        changedPaths.Add(view);
        scriptChanged = scriptChanged || view.EndsWith(".mscript") || view.EndsWith(".mcommon");
        languageChanged = languageChanged || view.EndsWith(".mshader") || view.EndsWith(".mcommon");
        configChanged = configChanged || (view.Contains("/Config/") && view.EndsWith(".mtext"));
    }
    List<String> keys;
    List<Located> stale;
    {
        const ScopeLock lock(_locatedLock);
        ++_epoch;
        if (configChanged)
        {
            ++_configGeneration;
            if (_converters != nullptr)
            {
                _converters->SetConfigGeneration(_configGeneration);
            }
        }
        for (const auto &entry : _located)
        {
            const Located &located = entry.Value;
            bool isStale = (scriptChanged && !located.Script.IsEmpty()) || (languageChanged && located.ReadLanguage) ||
                           (configChanged && located.ReadConfig);
            for (usize i = 0; !isStale && i < located.Paths.Count(); ++i)
            {
                isStale = changedPaths.Contains(located.Paths[i].AsView());
            }
            if (isStale)
            {
                keys.Add(entry.Key);
                stale.Add(located);
            }
        }
        for (const String &key : keys)
        {
            _located.Remove(key);
        }
    }
    // Without anyone to tell, dropping them is enough: the next Locate converts again.
    if (stale.IsEmpty() || _converters == nullptr || !_server->HasSubscribers("Asset.Changed"))
    {
        return;
    }
    for (usize i = 0; i < stale.Count(); ++i)
    {
        Revalidate(keys[i], stale[i]);
    }
}

void AssetIndexService::Revalidate(const String &key, const Located &old)
{
    uint64 epoch = 0;
    {
        const ScopeLock lock(_locatedLock);
        epoch = _epoch;
    }
    _converters->Request(
        old.Uuid, old.Target, ConversionPriority::Background,
        [this, key, old, epoch](const ConversionOutcome &outcome) {
            if (outcome.Succeeded)
            {
                Located located;
                if (!old.Shader.IsEmpty() || !old.Script.IsEmpty())
                {
                    located = old;
                    located.Location = outcome.Location;
                    located.Paths = outcome.Files;
                }
                else
                {
                    located = MakeLocated(old.Uuid, old.Target, outcome);
                }
                Remember(key, Move(located), epoch);
                // The same result, such as after an edit to another object of the same file, a touch, or a file a
                // shader could have used but does not: nothing to reload.
                if (outcome.Location.Key == old.Location.Key)
                {
                    return;
                }
            }
            PublishChanged(Span<const Guid>(&old.Uuid, 1));
        },
        old.Shader.AsView(), old.Script.AsView());
}

void AssetIndexService::PublishChanged(Span<const Guid> uuids)
{
    if (uuids.IsEmpty() || !_server->HasSubscribers("Asset.Changed"))
    {
        return;
    }
    StringBuilder<1024> text;
    JsonWriter writer(text);
    writer.BeginObject().Key("Uuids").BeginArray();
    for (const Guid &uuid : uuids)
    {
        StringBuilder<40> formatted;
        uuid.FormatTo(formatted, {});
        writer.String(formatted.AsView());
    }
    writer.EndArray().EndObject();
    _server->PublishJson("Asset.Changed", text.AsView());
}

void AssetIndexService::ScanThreadMain(void *argument)
{
    static_cast<AssetIndexService *>(argument)->ScanLoop();
}

void AssetIndexService::Commit(const List<String> &changed)
{
    if (changed.IsEmpty())
    {
        return;
    }
    // The changes go to the log in one write, unless this commit folds them into a snapshot anyway.
    List<uint8> entries;
    _index.WriteLogEntries(changed.AsSpan(), entries);
    const String logPath = LogPath();
    const bool fold = _index.GetBasePath().IsEmpty() || _logSize + entries.Count() > MaxLogSize;
    if (fold)
    {
        _index.Compact();
    }
    if (fold && !_index.GetBasePath().IsEmpty())
    {
        // The new snapshot has every change the log held.
        (void)PlatformFile::Delete(logPath.CStr());
        _logSize = 0;
    }
    else if (AssetIndex::AppendLog(logPath.AsView(), entries.AsSpan()))
    {
        _logSize += entries.Count();
    }
    (void)_publisher.Publish(_index);
    // After publishing: Invalidate converts again what changed, and the workers must see the new index to.
    Invalidate(changed);
    if (_server->HasSubscribers("AssetIndex.Changed"))
    {
        StringBuilder<4096> text;
        JsonWriter writer(text);
        writer.BeginObject().Key("Paths").BeginArray();
        for (const String &path : changed)
        {
            writer.String(path.AsView());
        }
        writer.EndArray().EndObject();
        _server->PublishJson("AssetIndex.Changed", text.AsView());
    }
}

String AssetIndexService::JournalPath() const
{
    return Format("{0}/Index.journal", _desc.StoreDirectory);
}

void AssetIndexService::Scan(StringView reason, bool all, Span<const String> paths)
{
    List<String> changed;
    if (all)
    {
        _scanner.ScanAll(changed);
    }
    if (!paths.IsEmpty())
    {
        // One batch: the .mfiles are written and the index commits once, however many paths changed.
        _scanner.ScanPaths(paths, changed);
    }
    Commit(changed);
    if (!changed.IsEmpty())
    {
        MARLIN_LOG(LogAssetIndex, Debug, "{0}: {1} files changed", reason, changed.Count());
    }
}

void AssetIndexService::ScanLoop()
{
    const float64 scanStart = PlatformTime::Seconds();
    AssetJournal journal;
    const bool useJournal = journal.Open(_desc.Folders.AsSpan());
    bool replayed = false;
    if (useJournal && _loaded && journal.Load(JournalPath().AsView()))
    {
        List<String> paths;
        if (journal.Poll(paths))
        {
            Scan("Replayed the journal", false, paths.AsSpan());
            replayed = true;
        }
    }
    if (!replayed)
    {
        journal.MarkEnd();
        Scan("Scanned every folder", true, {});
    }
    if (_index.GetBasePath().IsEmpty())
    {
        _index.Compact();
    }
    (void)_publisher.Publish(_index);
    if (useJournal)
    {
        (void)journal.Save(JournalPath().AsView());
    }
    MARLIN_LOG(LogAssetIndex, Info, "Asset index ready in {0:F0} ms: {1} files, {2} objects ({3})",
               (PlatformTime::Seconds() - scanStart) * 1000.0, _index.GetCounts().Files, _index.GetCounts().Objects,
               replayed ? "journal replayed" : "full scan");
    List<WaitingLocate> waiting;
    {
        const ScopeLock lock(_locatedLock);
        _scanning.Store(false, MemoryOrder::Release);
        waiting = Move(_waitingForScan);
        _waitingForScan.Clear();
    }
    for (const WaitingLocate &entry : waiting)
    {
        Resolve(entry.Params, entry.Deferred);
    }
    if (useJournal)
    {
        FollowJournal(journal);
    }
    else
    {
        FollowWatchers();
    }
}

void AssetIndexService::FollowJournal(AssetJournal &journal)
{
    while (!_stopping.Load(MemoryOrder::Acquire))
    {
        List<String> paths;
        const bool rescan = _rescan.Exchange(false);
        if (rescan || !journal.Poll(paths))
        {
            journal.MarkEnd();
            Scan("Rescanned every folder", true, {});
        }
        else if (!paths.IsEmpty())
        {
            Scan("Followed the journal", false, paths.AsSpan());
        }
        else
        {
            PlatformThread::Sleep(20);
            continue;
        }
        (void)journal.Save(JournalPath().AsView());
    }
}

void AssetIndexService::FollowWatchers()
{
    // Every layer of a folder, such as its platform layers, is watched; changes are reported by virtual path.
    List<Watch> watches;
    for (const String &folder : _desc.Folders)
    {
        for (const String &physical : GetPhysicalFolders(folder.AsView()))
        {
            Watch &watch = watches.AddDefaulted();
            watch.Watcher = MakeUnique<DirectoryWatcher>();
            watch.Folder = folder;
            if (!watch.Watcher->Open(physical.CStr(), true))
            {
                MARLIN_LOG(LogAssetIndex, Warning, "{0} is not watched; changes need AssetIndex.Rescan", folder);
                watches.Pop();
            }
        }
    }
    while (!_stopping.Load(MemoryOrder::Acquire))
    {
        if (_rescan.Exchange(false))
        {
            Scan("Rescanned every folder", true, {});
        }
        bool any = false;
        for (Watch &watch : watches)
        {
            if (!watch.Watcher->Wait(watches.Count() > 1 ? 10 : 50))
            {
                continue;
            }
            any = true;
            List<String> paths;
            HashSet<String> seen;
            FileChange change;
            bool overflow = false;
            while (watch.Watcher->Next(change))
            {
                if (change.Kind == FileChangeKind::Overflow)
                {
                    overflow = true;
                    continue;
                }
                String path = Format("{0}/{1}", watch.Folder, StringView(change.Path));
                if (seen.Add(path))
                {
                    paths.Add(Move(path));
                }
            }
            if (overflow)
            {
                paths.Clear();
                paths.Add(watch.Folder);
            }
            Scan("Followed file changes", false, paths.AsSpan());
        }
        if (!any && watches.IsEmpty())
        {
            PlatformThread::Sleep(50);
        }
    }
}

void AssetIndexService::AddMethods()
{
    RemoteServer &server = *_server;
    server.AddMethod(
        "AssetIndex.Find",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            AssetObjectInfo info;
            if (!ReadParams(call, params))
            {
                return;
            }
            if (!_index.Find(params.Uuid, info))
            {
                call.Fail("no object has that UUID");
                return;
            }
            SetResult(call, info);
        },
        RemoteThread::Any, "The object of a UUID: name, type, file and line.");
    server.AddMethod(
        "AssetIndex.FindBinary",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            AssetBinaryInfo info;
            if (!ReadParams(call, params))
            {
                return;
            }
            if (!_index.FindBinary(params.Uuid, info))
            {
                call.Fail("no binary file has that GUID");
                return;
            }
            SetResult(call, info);
        },
        RemoteThread::Any, "The binary source file of a GUID: path and content hash.");
    server.AddMethod(
        "AssetIndex.FindBinaryAt",
        [this](RemoteCall &call) {
            AssetPathParams params;
            AssetBinaryInfo info;
            if (!ReadParams(call, params))
            {
                return;
            }
            if (!_index.FindBinaryAt(params.Path.AsView(), info))
            {
                call.Fail("no binary file is indexed at that path");
                return;
            }
            SetResult(call, info);
        },
        RemoteThread::Any, "The binary source file at a virtual path: GUID and content hash.");
    server.AddMethod(
        "AssetIndex.CanonicalHash",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            Hash128 hash;
            if (!ReadParams(call, params))
            {
                return;
            }
            if (!_index.GetCanonicalHash(params.Uuid, hash))
            {
                call.Fail("no object has that UUID");
                return;
            }
            AssetHashResult result;
            result.Low = hash.Low;
            result.High = hash.High;
            SetResult(call, result);
        },
        RemoteThread::Any, "The canonical hash of an object, for clients that cannot read the shared index.");
    server.AddMethod(
        "AssetIndex.BinaryHash",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            Hash128 hash;
            if (!ReadParams(call, params))
            {
                return;
            }
            if (!_index.GetBinaryHash(params.Uuid, hash))
            {
                call.Fail("the index has no hash for that GUID");
                return;
            }
            AssetHashResult result;
            result.Low = hash.Low;
            result.High = hash.High;
            SetResult(call, result);
        },
        RemoteThread::Any, "The content hash of a binary file, for clients that cannot read the shared index.");
    server.AddMethod(
        "AssetIndex.Query",
        [this](RemoteCall &call) {
            AssetQuery query;
            if (!ReadParams(call, query))
            {
                return;
            }
            List<AssetObjectInfo> results;
            _index.Query(query, results);
            SetResult(call, results);
        },
        RemoteThread::Any, "Objects by type, name pattern, folder and tag.");
    server.AddMethod(
        "AssetIndex.References",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            if (!ReadParams(call, params))
            {
                return;
            }
            AssetLinks links;
            _index.GetReferences(params.Uuid, links.Objects, links.Binaries);
            SetResult(call, links);
        },
        RemoteThread::Any, "The objects and binary files an object links to.");
    server.AddMethod(
        "AssetIndex.Referencers",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            if (!ReadParams(call, params))
            {
                return;
            }
            List<AssetObjectInfo> results;
            _index.GetReferencers(params.Uuid, results);
            SetResult(call, results);
        },
        RemoteThread::Any, "The objects that link to an object or a binary file.");
    server.AddMethod(
        "AssetIndex.Dependencies",
        [this](RemoteCall &call) {
            AssetGuidParams params;
            if (!ReadParams(call, params))
            {
                return;
            }
            AssetLinks links;
            _index.GetDependencies(params.Uuid, links.Objects, links.Binaries);
            SetResult(call, links);
        },
        RemoteThread::Any, "Everything an object needs, transitively.");
    server.AddMethod(
        "AssetIndex.Status",
        [this](RemoteCall &call) {
            AssetIndexStatus status;
            status.Counts = _index.GetCounts();
            status.Scanning = IsScanning();
            status.Folders = _desc.Folders;
            status.ShadersWarmed = _shadersWarmed.Load(MemoryOrder::Acquire);
            status.ShadersToWarm = _shadersToWarm.Load(MemoryOrder::Acquire);
            _scanner.GetProgress(status.FilesRead, status.FilesToRead);
            if (_converters != nullptr)
            {
                const ConversionProgress progress = _converters->GetProgress();
                status.ConversionsQueued = progress.Queued;
                status.ConversionsRunning = progress.Running;
                status.Converted = progress.Converted;
                status.ConversionCacheHits = progress.CacheHits;
                status.ConversionsFailed = progress.Failed;
            }
            SetResult(call, status);
        },
        RemoteThread::Any,
        "Counts, whether the first scan is still running, the folders indexed, and the progress of scans, shader "
        "warming and conversions.");
    server.AddMethod(
        "AssetIndex.Rescan",
        [this](RemoteCall &call) {
            _rescan.Store(true, MemoryOrder::Release);
            call.SetResult(JsonValue::MakeBool(true));
        },
        RemoteThread::Any, "Scans every folder again, for changes the file watchers missed.");
}
} // namespace Marlin
