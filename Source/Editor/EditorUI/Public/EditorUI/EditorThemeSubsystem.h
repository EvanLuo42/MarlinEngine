#pragma once

#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/EditorThemeSubsystem.gen.h"

namespace Marlin
{
/// Owns the editor theme (Documentation/Architecture/09 §4.5): loads Editor/Theme.mtext from config on Initialize
/// and makes it the theme ThemeColor and ThemeMetric read.
MCLASS()
class EDITORUI_API EditorThemeSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    EditorThemeSubsystem();
    ~EditorThemeSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    [[nodiscard]] EditorTheme &GetTheme()
    {
        return _theme;
    }

    /// Reads config again and reloads the current theme; what is bound to its tokens updates in the next frame.
    MFUNCTION(ConsoleCommand = "Editor.ReloadTheme")
    static void ReloadTheme();

private:
    EditorTheme _theme;
};
} // namespace Marlin
