#pragma once

#include "CoreTypes.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// What the startup is doing, for a splash screen to show (Documentation/Architecture/09 §4.6).
struct StartupSnapshot
{
    /// Subsystems initialized, and those planned so far; the editor's scope adds its own when it starts.
    uint32 StepsDone = 0;
    uint32 StepsTotal = 0;
    /// The step or the status a subsystem set during it, UTF-8 and null-terminated.
    char Status[128] = {};
    /// The main loop has run a frame.
    bool MainLoopStarted = false;
};

using StartupReadyCheckId = uint32;

/// Startup reporting for a splash screen. It records nothing until a splash calls Begin, so processes without one pay
/// a relaxed load per call. Reporting is open: the engine reports each subsystem it initializes, and a subsystem
/// names a long synchronous step itself with SetStatus. Thread-safe; ready checks are main-thread only.
namespace StartupProgress
{
/// A splash screen is showing: steps are recorded from now on.
ENGINE_API void Begin();
/// The splash screen closed.
ENGINE_API void End();
[[nodiscard]] ENGINE_API bool IsActive();

/// A collection plans *count* more subsystems.
ENGINE_API void AddSteps(uint32 count);
/// The engine starts the step *name*, such as initializing a subsystem; the status reads "<verb> <name>".
ENGINE_API void BeginStep(StringView verb, StringView name);
ENGINE_API void EndStep();
/// Replaces the status until the next step, for a step that takes long, such as "Starting asset indexer".
ENGINE_API void SetStatus(StringView status);
ENGINE_API void SetMainLoopStarted();
[[nodiscard]] ENGINE_API StartupSnapshot GetSnapshot();

/// Adds what must be true before the main window shows, such as a UI's shaders being ready.
ENGINE_API StartupReadyCheckId AddReadyCheck(StringView name, Function<bool()> check);
ENGINE_API void RemoveReadyCheck(StartupReadyCheckId id);
/// True when every ready check passes; otherwise *failing* names the first one that does not.
[[nodiscard]] ENGINE_API bool AreReadyChecksPassing(StringView &failing);
} // namespace StartupProgress
} // namespace Marlin
