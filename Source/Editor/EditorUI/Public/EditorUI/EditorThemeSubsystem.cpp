#include "EditorUI/EditorThemeSubsystem.h"

#include "Config/Config.h"

namespace Marlin
{
EditorThemeSubsystem::EditorThemeSubsystem() = default;
EditorThemeSubsystem::~EditorThemeSubsystem() = default;

void EditorThemeSubsystem::Initialize(SubsystemContext & /*context*/)
{
    _theme.LoadFromConfig();
    EditorTheme::SetCurrent(&_theme);
}

void EditorThemeSubsystem::Deinitialize()
{
    if (EditorTheme::GetCurrent() == &_theme)
    {
        EditorTheme::SetCurrent(nullptr);
    }
}

void EditorThemeSubsystem::ReloadTheme()
{
    EditorTheme *theme = EditorTheme::GetCurrent();
    if (theme == nullptr)
    {
        MARLIN_LOG(LogEditorTheme, Warning, "Editor.ReloadTheme: no editor theme is loaded");
        return;
    }
    Config::Reload();
    theme->LoadFromConfig();
    MARLIN_LOG(LogEditorTheme, Info, "Reloaded the editor theme");
}
} // namespace Marlin
