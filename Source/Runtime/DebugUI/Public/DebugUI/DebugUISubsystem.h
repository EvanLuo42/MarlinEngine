#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "DebugUI/DebugOverlay.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "DebugUI/DebugUISubsystem.gen.h"

namespace Marlin
{
class ApplicationSubsystem;
class ConsoleVariable;
class UIPainterSubsystem;
class UITextSubsystem;
struct DebugUIContext;

/// Runs DebugUI and the debug overlay (09 §3): starts each frame's context on the main
/// canvas's overlay painter with the window's input, draws the debug overlay's sections and recent warnings, and the
/// console window that edits console variables and runs commands. -DebugOverlay on the command line or F1 turns the
/// overlay on, F2 the console window.
MCLASS(Config = "Engine.DebugUI", Dependencies = "ApplicationSubsystem, UIPainterSubsystem, UITextSubsystem")
class DEBUGUI_API DebugUISubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    /// Shows the debug overlay: frame, GPU and UI statistics with recent warnings.
    MPROPERTY(ConsoleVariable = "ui.DebugOverlay")
    static bool Overlay;
    /// Shows the console window.
    MPROPERTY(ConsoleVariable = "ui.DebugConsole")
    static bool ConsoleWindow;
    /// How long a warning or error stays as a banner, in seconds.
    MPROPERTY()
    static float32 BannerSeconds;

    DebugUISubsystem();
    ~DebugUISubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

private:
    class BannerSink;
    static constexpr uint32 HistoryLength = 120;

    void HandleHotkeys();
    void DrawOverlay(const FrameTime &time);
    void DrawConsole();
    void DrawLogLevels();

    ApplicationSubsystem *_application = nullptr;
    UIPainterSubsystem *_painter = nullptr;
    UITextSubsystem *_text = nullptr;
    UniquePtr<DebugUIContext> _context;
    UniquePtr<BannerSink> _banners;
    ExtensionSet<DebugOverlaySection> _sections;
    float32 _frameHistory[HistoryLength] = {};
    float32 _frameOrdered[HistoryLength] = {};
    uint32 _frameCount = 0;
    String _command;
    String _filter;
    List<String> _consoleOutput;
    /// The variables the filter shows, sorted, rebuilt when the filter or the set of variables changes.
    List<ConsoleVariable *> _consoleVariables;
    String _consoleFilter;
    uint64 _consoleVariablesHash = 0;
    bool _consoleVariablesValid = false;
};
} // namespace Marlin
// RECOVERY-DECISION D1: the head of the class comment (line 19, up to "09 §3)") was lost; reworded.
