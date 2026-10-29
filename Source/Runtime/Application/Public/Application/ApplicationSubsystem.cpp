#include "Application/ApplicationSubsystem.h"

#include "Application/PlatformApplication.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "Engine/StartupProgress.h"
#include "Misc/CommandLine.h"
#include "String/String.h"

namespace Marlin
{
int32 ApplicationSubsystem::Width = 1280;
int32 ApplicationSubsystem::Height = 720;
bool ApplicationSubsystem::Fullscreen = false;
bool ApplicationSubsystem::CustomFrame = false;
bool ApplicationSubsystem::Maximized = false;
int32 ApplicationSubsystem::MinWidth = 0;
int32 ApplicationSubsystem::MinHeight = 0;

bool ApplicationSubsystem::ShouldCreate(const SubsystemContext & /*context*/)
{
    return !CommandLine::HasSwitch("Headless");
}

void ApplicationSubsystem::Initialize(SubsystemContext & /*context*/)
{
    if (!PlatformApplication::Initialize())
    {
        MARLIN_LOG(LogCore, Error, "The platform application layer did not start");
        return;
    }
    const EngineEnvironment &environment = Engine::GetEnvironment();
    const String &title =
        !environment.Product.DisplayName.IsEmpty() ? environment.Product.DisplayName : environment.TargetName;
    WindowDesc desc;
    desc.Title = title.CStr();
    desc.Width = Width;
    desc.Height = Height;
    desc.MinWidth = MinWidth;
    desc.MinHeight = MinHeight;
    // Hidden until it can show a whole frame: at once without a splash screen, else when the splash closes (09 §4.6).
    desc.Flags |= WindowFlags::Hidden;
    if (CustomFrame)
    {
        desc.Flags |= WindowFlags::CustomFrame;
    }
    if (Maximized && !Fullscreen)
    {
        desc.Flags |= WindowFlags::Maximized;
    }
    _window = PlatformWindow::Create(desc);
    if (!_window.IsValid())
    {
        MARLIN_LOG(LogCore, Error, "Cannot create the main window \"{0}\" at {1}x{2}", title, Width, Height);
        return;
    }
    PlatformWindow::SetFullscreen(_window, Fullscreen);
    const PlatformRect rect = PlatformWindow::GetClientRect(_window);
    MARLIN_LOG(LogCore, Info, "Main window \"{0}\": {1}x{2} at {3:F2} DPI ({4})", title, rect.Width, rect.Height,
               PlatformWindow::GetDpiScale(_window), Fullscreen ? "fullscreen" : "windowed");
    if (!StartupProgress::IsActive())
    {
        ShowMainWindow();
    }
}

void ApplicationSubsystem::ShowMainWindow()
{
    if (_window.IsValid() && !PlatformWindow::IsVisible(_window))
    {
        PlatformWindow::Show(_window);
        PlatformWindow::Focus(_window);
    }
}

void ApplicationSubsystem::Deinitialize()
{
    if (_window.IsValid())
    {
        PlatformWindow::Destroy(_window);
        _window = {};
    }
    PlatformApplication::Shutdown();
}

void ApplicationSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    PlatformApplication::PumpMessages();
    _events.Clear();
    ApplicationEvent event;
    while (PlatformApplication::PollEvent(event))
    {
        _events.Add(event);
        if (event.Type == ApplicationEventType::WindowCloseRequested && event.Window == _window)
        {
            MARLIN_LOG(LogCore, Info, "The main window was closed; exiting");
            Engine::RequestExit(0);
        }
    }
}
} // namespace Marlin
