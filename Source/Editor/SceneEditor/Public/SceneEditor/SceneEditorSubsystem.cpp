#include "SceneEditor/SceneEditorSubsystem.h"

#include "EditorShell/EditingContextSubsystem.h"
#include "EditorShell/EditorMenuSubsystem.h"
#include "SceneEditor/SceneContext.h"
#include "String/Format.h"

namespace Marlin
{
SceneEditorSubsystem::SceneEditorSubsystem() = default;
SceneEditorSubsystem::~SceneEditorSubsystem() = default;

void SceneEditorSubsystem::Initialize(SubsystemContext &context)
{
    _contexts = context.Collection != nullptr ? context.Collection->Get<EditingContextSubsystem>() : nullptr;
    auto *menus = context.Collection != nullptr ? context.Collection->Get<EditorMenuSubsystem>() : nullptr;
    if (_contexts == nullptr || menus == nullptr)
    {
        return;
    }
    EditorMenuItem item;
    item.Label = "New Scene";
    item.Execute = [this] { NewScene(); };
    menus->AddItem("File", Move(item));
}

void SceneEditorSubsystem::Deinitialize()
{
    _contexts = nullptr;
}

void SceneEditorSubsystem::NewScene()
{
    if (_contexts == nullptr)
    {
        return;
    }
    const ExtensionSet<AssetContextType> &types = _contexts->GetAssetTypes();
    const SceneContextType *sceneType = nullptr;
    for (const AssetContextType *type : types)
    {
        if (types.GetType(*type).IsA(TypeOf<SceneContextType>()))
        {
            sceneType = static_cast<const SceneContextType *>(type);
        }
    }
    if (sceneType == nullptr)
    {
        return;
    }
    ++_untitled;
    const String name = _untitled == 1 ? String("Untitled Scene") : Format("Untitled Scene {0}", _untitled);
    (void)_contexts->OpenContext([&] { return MakeUnique<SceneContext>(sceneType, Guid(), name.AsView()); });
}
} // namespace Marlin
