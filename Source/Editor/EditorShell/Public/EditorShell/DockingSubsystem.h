#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorShell/Dock.h"
#include "Reflection/ExtensionSet.h"
#include "String/StringView.h"
#include "EditorShell/DockingSubsystem.gen.h"

namespace Marlin
{
class DockSpace;
class EditingContext;
class EditorUISubsystem;

/// The dock types and the DockSpaces of the open editing contexts (Documentation/Architecture/09 §5). The Window menu
/// gets an item per dock type, shown while the active context's DockSpace can hold it, and Reset Layout. Each frame
/// it lets the spaces free what left their layouts and save changed layouts. Main thread only.
MCLASS(Dependencies = "EditorUISubsystem, EditorMenuSubsystem")
class EDITORSHELL_API DockingSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    DockingSubsystem();
    ~DockingSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    [[nodiscard]] const ExtensionSet<DockType> &GetTypes() const
    {
        return _types;
    }

    /// The dock type named *name* (its reflected type's name), or null.
    [[nodiscard]] const DockType *FindType(StringView name) const;
    /// The name docks of *type* go by in layouts.
    [[nodiscard]] StringView GetName(const DockType &type) const;

    /// The editor UI, for floating windows; null without one.
    [[nodiscard]] EditorUISubsystem *GetUI() const
    {
        return _ui;
    }

    void Register(DockSpace *space);
    void Unregister(DockSpace *space);
    /// The DockSpace of *context*, or null.
    [[nodiscard]] DockSpace *Find(const EditingContext *context) const;

private:
    /// The DockSpace of the active editing context, or null.
    [[nodiscard]] DockSpace *GetActiveSpace() const;

    EditorUISubsystem *_ui = nullptr;
    ExtensionSet<DockType> _types;
    List<DockSpace *> _spaces;
    float64 _now = 0;
};
} // namespace Marlin
