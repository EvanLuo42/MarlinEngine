#include "Engine/EngineSubsystems.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/Jobs.h"
#include "Engine/Engine.h"
#include "Misc/CommandLine.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
uint32 JobSubsystem::WorkerCount = 0;
uint32 JobSubsystem::ReservedCores = 0;
bool LogSubsystem::WriteFile = true;

namespace
{
Atomic<bool> _logStarted{false};
} // namespace

bool LogSubsystem::IsStarted()
{
    return _logStarted.Load(MemoryOrder::Acquire);
}

void JobSubsystem::Initialize(SubsystemContext & /*context*/)
{
    JobSystemDesc desc;
    desc.WorkerCount = WorkerCount;
    desc.ReservedCores = ReservedCores;
    if (!JobSystem::Start(desc))
    {
        MARLIN_LOG(LogCore, Error, "The job system did not start");
        return;
    }
    MARLIN_LOG(LogCore, Info, "Job system started with {0} workers", JobSystem::GetWorkerCount());
}

void JobSubsystem::Deinitialize()
{
    JobSystem::Stop();
}

String LogSubsystem::SetCategoryLevel(const String &category, const String &level)
{
    LogCategory *found = Log::FindCategory(category.AsView());
    LogLevel parsed = LogLevel::Info;
    if (found == nullptr)
    {
        return Format("No log category is named '{0}'; Log.List shows them", category);
    }
    if (!Log::ParseLevel(level.AsView(), parsed))
    {
        return Format("'{0}' is not a level; use Trace, Debug, Info, Warning, Error, Fatal or Off", level);
    }
    found->SetLevel(parsed);
    return Format("{0} = {1}", StringView(found->GetName()), Log::GetLevelName(parsed));
}

String LogSubsystem::ListCategories()
{
    StringBuilder<2048> text;
    Log::ForEachCategory([&text](LogCategory &category) {
        text.Append(text.IsEmpty() ? "" : "\n")
            .Append(category.GetName())
            .Append(" = ")
            .Append(Log::GetLevelName(category.GetLevel()));
    });
    return text.ToString();
}

void LogSubsystem::Initialize(SubsystemContext & /*context*/)
{
    // -LogLevel=LogCore:Debug,LogAssetIndex:Trace, before the first message below, so startup is covered.
    StringView levels;
    if (CommandLine::TryGetValue("LogLevel", levels))
    {
        StringBuilder<512> errors;
        if (!Log::SetLevels(levels, errors))
        {
            MARLIN_LOG(LogCore, Warning, "-LogLevel is not applied in full:\n{0}", errors.AsView());
        }
    }
    StringBuilder<256> path("/User/Logs/");
    path.Append(Engine::GetEnvironment().TargetName.AsView()).Append(".log");
    if (WriteFile)
    {
        _sink.File = Vfs::Open(path.AsView(), FileAccess::Write, FileMode::CreateAlways);
        if (!_sink.File.IsValid())
        {
            MARLIN_LOG(LogCore, Warning, "Cannot write {0}; the log goes to the debugger only", path.AsView());
        }
    }
    Log::AddSink(&_sink);
    if (_sink.File.IsValid())
    {
        MARLIN_LOG(LogCore, Info, "Writing the log to {0}", path.AsView());
    }
    _logStarted.Store(true, MemoryOrder::Release);
}

void LogSubsystem::Deinitialize()
{
    _logStarted.Store(false, MemoryOrder::Release);
    Log::Flush();
    Log::RemoveSink(&_sink);
    _sink.Finish();
    if (_sink.File.IsValid())
    {
        PlatformFile::Close(_sink.File);
        _sink.File = {};
    }
}

void LogSubsystem::EndFrame(const FrameTime & /*time*/)
{
    Log::Flush();
}

void LogSubsystem::FileSink::Write(const LogRecord &record)
{
    if (!File.IsValid())
    {
        return;
    }
    Log::FormatRecord(_pending, record);
    _pending.Append('\n');
    _urgent = _urgent || record.Level >= LogLevel::Error;
}

void LogSubsystem::FileSink::Flush()
{
    if (!File.IsValid() || _pending.IsEmpty())
    {
        return;
    }
    // One write at a time keeps the file in order, and frees _writing.
    Finish();
    _writing.Clear();
    _writing.AddRange(Span<const char>(_pending.Data(), _pending.Length()));
    _pending.Clear();
    if (_urgent || !JobSystem::IsRunning())
    {
        _urgent = false;
        (void)PlatformFile::Write(File, _writing.Data(), _writing.Count());
        return;
    }
    Jobs::Run(JobDecl{&WriteJob, this}, &_written);
}

void LogSubsystem::FileSink::Finish()
{
    if (!_written.IsDone())
    {
        Jobs::Wait(_written);
    }
}

void LogSubsystem::FileSink::WriteJob(void *argument)
{
    auto &sink = *static_cast<FileSink *>(argument);
    (void)PlatformFile::Write(sink.File, sink._writing.Data(), sink._writing.Count());
}
} // namespace Marlin
