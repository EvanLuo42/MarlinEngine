#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// What a splash screen shows that does not change (Documentation/Architecture/09 §4.6). Strings are UTF-8 and copied
/// by Show; empty or null ones are left out.
struct SplashContent
{
    /// The large title, such as "Marlin Engine".
    const char *Title = "";
    /// The small word beside the logo.
    const char *Brand = "";
    /// The two lines at the top right, such as the version and the build.
    const char *Version = "";
    const char *Build = "";
    /// The bottom right line, such as the copyright.
    const char *Footer = "";
    /// Font files to load for the splash alone, and the families to draw with; the platform falls back to a system
    /// font when a family is missing.
    const char *const *FontFiles = nullptr;
    uint32 FontFileCount = 0;
    const char *SansFamily = "";
    const char *MonoFamily = "";
    /// Called on the window thread about every 50 ms while the splash shows, to call SetStatus.
    void (*Tick)() = nullptr;
};

/// What a splash screen shows that changes; strings are UTF-8 and copied.
struct SplashStatus
{
    /// The project being opened; the subtitle is left out while it is empty.
    const char *Project = "";
    /// What is happening, such as "Compiling Marlin.Passes.Ui.Count".
    const char *Task = "";
    /// Counts beside it, such as "12 / 40".
    const char *Detail = "";
    /// The bottom left line, such as "Step 1 of 2".
    const char *Stage = "";
    /// 0 to 1.
    float32 Progress = 0;
};

/// A small native window shown while the engine starts, drawn by the platform without the renderer (09 §4.6). It
/// lives on the platform's window thread, which Show starts if it is not running yet (12 §1.5), so it keeps drawing
/// while the main thread is busy. Callable from any thread.
namespace PlatformSplash
{
/// False if it could not be shown; showing it twice does nothing.
APPLICATION_API bool Show(const SplashContent &content);
APPLICATION_API void SetStatus(const SplashStatus &status);
/// Closes it; does nothing if it is not shown.
APPLICATION_API void Hide();
[[nodiscard]] APPLICATION_API bool IsShown();
} // namespace PlatformSplash
} // namespace Marlin
