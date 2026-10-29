#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorUI/Signal.h"
#include "EditorUI/UIElement.h"
#include "EditorShell/EditorShellSubsystem.gen.h"

namespace Marlin
{
class ApplicationSubsystem;
class EditingContextSubsystem;
class EditingContext;
class EditorMenuSubsystem;
class EditorUISubsystem;
class UITextSubsystem;

/// The editor's main window (Documentation/Architecture/09 §5): a title bar of its own holding a tab per editing
/// context and the minimize, maximize and close buttons; the menu bar; the active context's content; the status bar.
MCLASS(Dependencies = "EditorUISubsystem, EditingContextSubsystem, EditorMenuSubsystem")
class EDITORSHELL_API EditorShellSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    EditorShellSubsystem();
    ~EditorShellSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    struct TabStrip;
    struct MenuState;

private:
    [[nodiscard]] UIElement BuildWindow();
    [[nodiscard]] UIElement BuildTitleBar();
    [[nodiscard]] UIElement BuildTabStrip();
    [[nodiscard]] UIElement BuildTab(EditingContext *context, TabStrip *strip);
    [[nodiscard]] UIElement BuildWindowButtons();
    [[nodiscard]] UIElement BuildMenuBar();
    [[nodiscard]] UIElement BuildMenuOverlay();
    [[nodiscard]] UIElement BuildMenuPopup(usize menu);
    [[nodiscard]] UIElement BuildContent();
    [[nodiscard]] UIElement BuildStatusBar();

    ApplicationSubsystem *_application = nullptr;
    const UITextSubsystem *_text = nullptr;
    EditorUISubsystem *_ui = nullptr;
    EditingContextSubsystem *_contexts = nullptr;
    EditorMenuSubsystem *_menus = nullptr;
    ReactiveOwner *_owner = nullptr;
    Signal<bool> _maximized;
    MenuState *_menu = nullptr;
};
} // namespace Marlin
