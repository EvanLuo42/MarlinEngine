#pragma once

#include "Application/PlatformApplication.h"
#include "Application/PlatformWindow.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Application/ApplicationSubsystem.gen.h"

namespace Marlin
{
/// Pumps the OS messages each frame and owns the main window; closing it ends the main loop. `-Headless` leaves it
/// out, as do targets that do not link Application, such as servers.
MCLASS(Config = "Engine.Window", UserConfigurable)
class APPLICATION_API ApplicationSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// The client size of the main window.
    MPROPERTY()
    static int32 Width;
    MPROPERTY()
    static int32 Height;
    MPROPERTY()
    static bool Fullscreen;
    /// Opens maximized, keeping its frame (unlike Fullscreen); restoring returns to Width and Height.
    MPROPERTY()
    static bool Maximized;
    /// The smallest client size the user can resize it to, at 96 DPI; 0 leaves it to the OS.
    MPROPERTY()
    static int32 MinWidth;
    MPROPERTY()
    static int32 MinHeight;
    /// Whether the main window draws its own title bar (WindowFlags::CustomFrame); the editor does.
    MPROPERTY()
    static bool CustomFrame;

    MFUNCTION()
    static bool ShouldCreate(const SubsystemContext &context);

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// The events PumpMessages took this frame, in the order the OS sent them.
    [[nodiscard]] Span<const ApplicationEvent> GetEvents() const
    {
        return _events;
    }

    [[nodiscard]] WindowHandle GetMainWindow() const
    {
        return _window;
    }

    /// Shows the main window, which starts hidden while a splash screen is up; does nothing once it shows.
    void ShowMainWindow();

private:
    WindowHandle _window;
    List<ApplicationEvent> _events;
};
} // namespace Marlin
