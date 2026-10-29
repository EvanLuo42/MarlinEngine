#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorShell/EditingContext.h"
#include "EditorUI/Signal.h"
#include "Memory/UniquePtr.h"
#include "Reflection/ExtensionSet.h"
#include "String/String.h"
#include "Templates/Function.h"
#include "EditorShell/EditingContextSubsystem.gen.h"

namespace Marlin
{
/// The open editing contexts, in tab order, and which one is active (Documentation/Architecture/09 §5). Pinned
/// contexts, such as the Asset Manager, come from PinnedContextTypes and are always open and first; asset contexts come
/// from AssetContextTypes, tool contexts from ToolContextTypes, each of which gets an item in its menu. Main thread
/// only.
MCLASS(Dependencies = "EditorMenuSubsystem, DockingSubsystem")
class EDITORSHELL_API EditingContextSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    EditingContextSubsystem();
    ~EditingContextSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;

    /// The open contexts in tab order; follows changes.
    [[nodiscard]] const List<EditingContext *> &GetContexts() const
    {
        return _order.Get();
    }

    /// The active context; follows changes.
    [[nodiscard]] EditingContext *GetActive() const
    {
        return _active.Get();
    }

    /// Activates the context of *asset*, opening one titled *name* with the AssetContextType that edits
    /// *assetType* if it is not open yet; null if no type edits it.
    EditingContext *OpenAsset(const Guid &asset, StringView assetType, StringView name);
    /// Opens and activates the context *create* returns, which edits no open asset; *create* runs with the new
    /// context's owner current. Null if it returns null.
    EditingContext *OpenContext(FunctionRef<UniquePtr<EditingContext>()> create);
    /// Activates the context of *type*, opening it if it is not open yet.
    EditingContext *OpenTool(const ToolContextType &type);
    void Activate(EditingContext *context);
    /// Closes *context* unless it is pinned or refuses (CanClose); the tab to its right, else its left, becomes
    /// active if it was. False if it stays open.
    bool Close(EditingContext *context);
    /// Moves *context* to tab *index*, never before the pinned contexts.
    void Reorder(EditingContext *context, usize index);

    [[nodiscard]] const ExtensionSet<AssetContextType> &GetAssetTypes() const
    {
        return _assetTypes;
    }

    [[nodiscard]] const ExtensionSet<ToolContextType> &GetToolTypes() const
    {
        return _toolTypes;
    }

    /// Opens asset *uuid* titled *name*; without an asset index the asset's type is unknown.
    MFUNCTION(ConsoleCommand = "Editor.OpenAsset")
    static void OpenAssetCommand(const String &uuid, const String &name);
    /// Opens the tool whose display name is *name*.
    MFUNCTION(ConsoleCommand = "Editor.OpenTool")
    static void OpenToolCommand(const String &name);

private:
    struct Entry
    {
        UniquePtr<EditingContext> Context;
        ReactiveOwner *Owner = nullptr;
    };

    EditingContext *Add(ReactiveOwner *owner, UniquePtr<EditingContext> context);
    [[nodiscard]] usize GetPinnedCount() const;

    ReactiveOwner *_owner = nullptr;
    ExtensionSet<AssetContextType> _assetTypes;
    ExtensionSet<ToolContextType> _toolTypes;
    ExtensionSet<PinnedContextType> _pinnedTypes;
    List<Entry> _entries;
    Signal<List<EditingContext *>> _order;
    Signal<EditingContext *> _active;
};
} // namespace Marlin
