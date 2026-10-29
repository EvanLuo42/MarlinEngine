#pragma once

#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "SceneEditor/SceneEditorSubsystem.gen.h"

namespace Marlin
{
class EditingContextSubsystem;

/// Adds File > New Scene, which opens a scene context that is not saved yet; until the Asset Manager opens Levels,
/// it is how a scene context opens.
MCLASS(Dependencies = "EditingContextSubsystem, EditorMenuSubsystem")
class SCENEEDITOR_API SceneEditorSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    SceneEditorSubsystem();
    ~SceneEditorSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// Opens a new, unsaved scene.
    void NewScene();

private:
    EditingContextSubsystem *_contexts = nullptr;
    uint32 _untitled = 0;
};
} // namespace Marlin
