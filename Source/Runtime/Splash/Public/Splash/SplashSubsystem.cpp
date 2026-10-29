#include "Splash/SplashSubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "Application/PlatformSplash.h"
#include "Asset/Assets.h"
#include "Concurrency/ScopeLock.h"
#include "Concurrency/SpinLock.h"
#include "Diagnostics/Log.h"
#include "Engine/EngineSubsystems.h"
#include "Engine/StartupProgress.h"
#include "HAL/PlatformTime.h"
#include "Memory/MemoryOps.h"
#include "Misc/CommandLine.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

#include <initializer_list>

namespace Marlin
{
float64 SplashSubsystem::ReadyTimeout = 10;

namespace
{
/// The startup steps take this much of the bar, the critical loads the rest (09 §4.6).
constexpr float32 StartupShare = 0.3f;
/// Ticks come about every 50 ms; the log is flushed every other one until the main loop does it each frame.
constexpr uint32 TicksPerFlush = 2;

struct SplashState
{
    /// The project's name, written on the main thread and read by Tick on the window thread.
    SpinLock Lock;
    char Project[128] = {};
    /// Window thread only.
    float32 Progress = 0;
    uint32 Ticks = 0;
};

SplashState _state;

void SetProject(StringView name)
{
    const ScopeLock lock(_state.Lock);
    const usize length = name.Length() < sizeof(_state.Project) - 1 ? name.Length() : sizeof(_state.Project) - 1;
    MemCopy(_state.Project, name.Data(), length);
    _state.Project[length] = '\0';
}

/// The stem of -Project=, before the engine has resolved the project.
StringView ProjectFromCommandLine()
{
    StringView path;
    if (!CommandLine::TryGetValue("Project", path))
    {
        return {};
    }
    for (usize i = path.Length(); i-- > 0;)
    {
        if (path[i] == '/' || path[i] == '\\')
        {
            path = path.RemoveStart(i + 1);
            break;
        }
    }
    return path.EndsWith(".mproject") ? path.RemoveEnd(9) : path;
}

/// Runs on the window thread: turns the startup steps and the critical loads into what the splash says.
void Tick()
{
    if (++_state.Ticks % TicksPerFlush == 0 && LogSubsystem::IsStarted())
    {
        Log::Flush();
    }
    const StartupSnapshot startup = StartupProgress::GetSnapshot();
    StringBuilder<256> task;
    StringBuilder<64> detail;
    float32 progress = 0;
    if (!startup.MainLoopStarted)
    {
        task.Append(StringView(startup.Status));
        if (startup.StepsTotal > 0)
        {
            const uint32 step = startup.StepsDone < startup.StepsTotal ? startup.StepsDone + 1 : startup.StepsTotal;
            FormatTo(detail, "{0} / {1}", step, startup.StepsTotal);
            progress =
                StartupShare * static_cast<float32>(startup.StepsDone) / static_cast<float32>(startup.StepsTotal);
        }
    }
    else
    {
        CriticalLoadProgress loads;
        Assets::GetCriticalProgress(loads);
        if (loads.Requested > 0)
        {
            FormatTo(detail, "{0} / {1} assets", loads.Finished, loads.Requested);
            progress = StartupShare + (1 - StartupShare) * static_cast<float32>(loads.Finished) /
                                          static_cast<float32>(loads.Requested);
        }
        else
        {
            progress = StartupShare;
        }
        if (!loads.Pending)
        {
            task.Append("Preparing the editor");
        }
        else if (loads.PendingStage == AssetLoadStage::Compiling)
        {
            FormatTo(task, "Compiling {0}", loads.PendingName);
        }
        else if (loads.PendingStage == AssetLoadStage::Converting)
        {
            FormatTo(task, "Converting {0}",
                     loads.PendingName.IsEmpty() ? StringView("assets") : loads.PendingName.AsView());
        }
        else if (loads.PendingStage == AssetLoadStage::Locating)
        {
            task.Append("Waiting for the asset indexer");
        }
        else
        {
            task.Append("Loading assets");
        }
    }
    // The bar only moves forward, though the total grows as the first frame asks for more.
    _state.Progress = progress > _state.Progress ? progress : _state.Progress;
    task.Append("\xE2\x80\xA6");

    char project[sizeof(_state.Project)];
    {
        const ScopeLock lock(_state.Lock);
        MemCopy(project, _state.Project, sizeof(project));
    }
    SplashStatus status;
    status.Project = project;
    status.Task = task.CStr();
    status.Detail = detail.CStr();
    status.Stage = startup.MainLoopStarted ? "Step 2 of 2" : "Step 1 of 2";
    status.Progress = _state.Progress;
    PlatformSplash::SetStatus(status);
}

constexpr StartupSplash SplashHooks{&Splash::Show, &Splash::Hide};
} // namespace

namespace Splash
{
const StartupSplash *GetStartupSplash()
{
    return &SplashHooks;
}

void Show()
{
    if (CommandLine::HasSwitch("Headless") || CommandLine::HasSwitch("NoSplash"))
    {
        return;
    }
    const EngineEnvironment &environment = Engine::GetEnvironment();
    SetProject(ProjectFromCommandLine());

    // The engine's own UI fonts, from a source or installed engine; a packaged one falls back to system fonts.
    StringBuilder<512> fonts[3];
    const char *files[3] = {};
    uint32 fileCount = 0;
    if (!environment.EngineRoot.IsEmpty())
    {
        for (StringView file : {StringView("Inter/InterVariable.ttf"), StringView("Inter/Inter-SemiBold.ttf"),
                                StringView("JetBrainsMono/JetBrainsMono-Regular.ttf")})
        {
            FormatTo(fonts[fileCount], "{0}/Content/Fonts/{1}", environment.EngineRoot, file);
            files[fileCount] = fonts[fileCount].CStr();
            ++fileCount;
        }
    }
    StringBuilder<64> version;
    version.Append(!environment.EngineVersion.IsEmpty() ? environment.EngineVersion.AsView()
                                                        : environment.Configuration);
    StringBuilder<96> build;
    FormatTo(build, "Build {0}", environment.EngineId);

    SplashContent content;
    content.Title = "Marlin Engine";
    content.Brand = "MARLIN";
    content.Version = version.CStr();
    content.Build = build.CStr();
    content.Footer = "\xC2\xA9 2026 Aurelia Games";
    content.FontFiles = files;
    content.FontFileCount = fileCount;
    content.SansFamily = "Inter";
    content.MonoFamily = "JetBrains Mono";
    content.Tick = &Tick;
    if (PlatformSplash::Show(content))
    {
        StartupProgress::Begin();
    }
}

void Hide()
{
    PlatformSplash::Hide();
    StartupProgress::End();
}
} // namespace Splash

bool SplashSubsystem::ShouldCreate(const SubsystemContext & /*context*/)
{
    return StartupProgress::IsActive();
}

void SplashSubsystem::Initialize(SubsystemContext &context)
{
    _application = context.Collection->Get<ApplicationSubsystem>();
    const EngineEnvironment &environment = Engine::GetEnvironment();
    if (environment.HasProject())
    {
        SetProject(environment.ProjectName.AsView());
    }
}

void SplashSubsystem::Deinitialize()
{
    Close();
}

void SplashSubsystem::BeginFrame(const FrameTime &time)
{
    if (time.Frame == 0)
    {
        StartupProgress::SetMainLoopStarted();
    }
}

void SplashSubsystem::EndFrame(const FrameTime &time)
{
    if (_closed)
    {
        return;
    }
    CriticalLoadProgress loads;
    Assets::GetCriticalProgress(loads);
    if (time.Frame == 0 || loads.Finished != _lastFinished || loads.Requested != _lastRequested)
    {
        _lastProgress = time.Elapsed;
        _lastFinished = loads.Finished;
        _lastRequested = loads.Requested;
    }
    if (loads.Finished != loads.Requested)
    {
        _readyFrames = 0;
        return;
    }
    StringView failing;
    if (!StartupProgress::AreReadyChecksPassing(failing))
    {
        _readyFrames = 0;
        if (time.Elapsed - _lastProgress > ReadyTimeout)
        {
            MARLIN_LOG(LogCore, Warning, "{0} is not ready after {1} s; showing the main window anyway", failing,
                       ReadyTimeout);
            Close();
        }
        return;
    }
    // Ready at the end of two frames in a row: the second drew with everything in place.
    if (++_readyFrames >= 2)
    {
        MARLIN_LOG(LogCore, Info, "The main window shows after {0} frames and {1} startup assets", time.Frame + 1,
                   loads.Finished);
        Close();
    }
}

void SplashSubsystem::Close()
{
    if (_closed)
    {
        return;
    }
    _closed = true;
    if (_application != nullptr)
    {
        _application->ShowMainWindow();
    }
    Splash::Hide();
}
} // namespace Marlin
