#pragma once

#include "CoreTypes.h"
#include "Engine/Engine.h"
#include "Engine/Subsystem.h"
#include "Splash/SplashSubsystem.gen.h"

namespace Marlin
{
class ApplicationSubsystem;

/// The splash screen of Documentation/Architecture/09 §4.6, for LaunchDesc::Splash: Show puts it up at step 4 unless
/// the process runs -Headless or -NoSplash, and Hide closes it.
namespace Splash
{
[[nodiscard]] SPLASH_API const StartupSplash *GetStartupSplash();
SPLASH_API void Show();
SPLASH_API void Hide();
} // namespace Splash

/// Closes the splash and shows the main window once the main loop runs, every IoPriority::Critical load has finished
/// and every StartupProgress ready check passes, for two frames in a row so that a whole frame was drawn. Exists only
/// while a splash shows.
MCLASS(Config = "Engine.Splash", Dependencies = "ApplicationSubsystem, AssetSubsystem")
class SPLASH_API SplashSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// With every load finished, how long a ready check may keep failing before the main window shows anyway.
    MPROPERTY()
    static float64 ReadyTimeout;

    MFUNCTION()
    static bool ShouldCreate(const SubsystemContext &context);

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

private:
    void Close();

    ApplicationSubsystem *_application = nullptr;
    uint32 _readyFrames = 0;
    /// When the loads last made progress, for ReadyTimeout.
    float64 _lastProgress = 0;
    uint32 _lastFinished = 0;
    uint32 _lastRequested = 0;
    bool _closed = false;
};
} // namespace Marlin
