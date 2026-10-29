#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorUI/UIWindowRoot.h"
#include "Memory/UniquePtr.h"
#include "Templates/Function.h"
#include "EditorUI/EditorUISubsystem.gen.h"

namespace Marlin
{
class ApplicationSubsystem;
class UIPainterSubsystem;
class UITextSubsystem;
class UIWindowCanvas;

/// The editor UI of every editor window (Documentation/Architecture/09 §4.1, §4.5): the main window and the windows
/// OpenWindow opens, such as floating docks. Each frame it dispatches each window's events to its element tree,
/// flushes signal propagation once, then lays out what is dirty, paints each tree into its window's canvas and hands
/// the OS the tree's title bar regions.
MCLASS(Config = "Editor.UI",
       Dependencies = "ApplicationSubsystem, EditorThemeSubsystem, UIPainterSubsystem, UITextSubsystem")
class EDITORUI_API EditorUISubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    /// The editor UI's size on top of the display's DPI scale: 1.25 draws everything a quarter larger. Clamped to
    /// 0.5–4.
    MPROPERTY()
    static float32 Scale;

    EditorUISubsystem();
    ~EditorUISubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void Update(const FrameTime &time) override;

    /// The main window's element tree; null without a main window.
    [[nodiscard]] UIWindowRoot *GetMainWindow() const
    {
        return _windows.IsEmpty() ? nullptr : _windows[0]->Root.Get();
    }

    /// Opens an OS window of its own (WindowFlags::CustomFrame is added) with an empty element tree, painted from the
    /// next frame; *onCloseRequested* runs when the OS asks to close it. Null if the window or its surface cannot be
    /// made. Positions and sizes in *desc* are physical pixels.
    UIWindowRoot *OpenWindow(const WindowDesc &desc, Function<void()> onCloseRequested);
    /// Closes a window OpenWindow opened, at the end of this frame's Update; its tree is freed then.
    void CloseWindow(UIWindowRoot *window);

    /// The window whose client area holds *x*, *y* (physical pixels on the desktop), preferring the windows
    /// OpenWindow opened, which stay above the main window; null if none. *ignore*, such as a window being dragged
    /// under the cursor, is skipped.
    [[nodiscard]] UIWindowRoot *FindWindowAt(int32 x, int32 y, const UIWindowRoot *ignore = nullptr) const;

    /// The physical pixels one painter unit of *window* covers: its display's DPI scale times Scale.
    [[nodiscard]] static float32 GetPixelsPerUnit(WindowHandle window);

    /// Sets Scale; the UI lays out again at the new size next frame.
    MFUNCTION(ConsoleCommand = "Editor.UIScale")
    static void SetScale(float32 scale);

private:
    struct EditorWindow
    {
        UniquePtr<UIWindowRoot> Root;
        UIWindowCanvas *Canvas = nullptr;
        /// Opened by OpenWindow, so destroyed here; the main window belongs to ApplicationSubsystem.
        bool Owned = false;
        bool Destroying = false;
        Function<void()> OnCloseRequested;
        List<WindowHitRegion> HitRegions;
    };

    /// Tells the OS which parts of *window* are its title bar when they changed (WindowFlags::CustomFrame).
    void UpdateHitRegions(EditorWindow &window, float32 scale);
    void Destroy(EditorWindow &window);

    ApplicationSubsystem *_application = nullptr;
    UIPainterSubsystem *_painter = nullptr;
    const UITextSubsystem *_text = nullptr;
    /// The main window first.
    List<UniquePtr<EditorWindow>> _windows;
    List<WindowHitRegion> _scratchRegions;
    /// Holds a splash screen until no text waits for its font (Documentation/Architecture/09 §4.6).
    uint32 _readyCheck = 0;
};
} // namespace Marlin
