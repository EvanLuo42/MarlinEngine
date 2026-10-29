#include "EditorShell/DockingSubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "EditorShell/DockSpace.h"
#include "EditorShell/EditingContextSubsystem.h"
#include "EditorShell/EditorMenuSubsystem.h"
#include "EditorUI/EditorUISubsystem.h"
#include "Engine/Engine.h"

namespace Marlin
{
DockingSubsystem::DockingSubsystem() = default;
DockingSubsystem::~DockingSubsystem() = default;

void DockingSubsystem::Initialize(SubsystemContext &context)
{
    _types.Build();
    _ui = context.Collection != nullptr ? context.Collection->Get<EditorUISubsystem>() : nullptr;
    auto *menus = context.Collection != nullptr ? context.Collection->Get<EditorMenuSubsystem>() : nullptr;
    if (menus == nullptr)
    {
        return;
    }
    for (const DockType *type : _types)
    {
        EditorMenuItem item;
        item.Label = String(type->GetDisplayName());
        item.Order = 200;
        item.IsVisible = [this, type] {
            const DockSpace *space = GetActiveSpace();
            return space != nullptr && type->CanDockIn(space->GetContext());
        };
        item.Execute = [this, type] {
            if (DockSpace *space = GetActiveSpace())
            {
                (void)space->Open(*type);
            }
        };
        menus->AddItem("Window", Move(item));
    }
    EditorMenuItem reset;
    reset.Label = "Reset Layout";
    reset.Order = 300;
    reset.IsVisible = [this] { return GetActiveSpace() != nullptr; };
    reset.Execute = [this] {
        if (DockSpace *space = GetActiveSpace())
        {
            space->ResetLayout();
        }
    };
    menus->AddItem("Window", Move(reset));
}

void DockingSubsystem::Deinitialize()
{
    _spaces.Clear();
}

void DockingSubsystem::BeginFrame(const FrameTime &time)
{
    _now = time.Elapsed;
    if (const auto *application = Engine::GetSubsystem<ApplicationSubsystem>())
    {
        // Whether each window moved and resized this frame, told to the space that owns it once.
        struct WindowChange
        {
            WindowHandle Window;
            bool Moved = false;
            bool Resized = false;
        };
        List<WindowChange> changes;
        for (const ApplicationEvent &event : application->GetEvents())
        {
            if (event.Type != ApplicationEventType::WindowMoved && event.Type != ApplicationEventType::WindowResized)
            {
                continue;
            }
            WindowChange *change =
                changes.Find([&event](const WindowChange &entry) { return entry.Window == event.Window; });
            if (change == nullptr)
            {
                change = &changes.Add({event.Window});
            }
            (event.Type == ApplicationEventType::WindowMoved ? change->Moved : change->Resized) = true;
        }
        for (const WindowChange &change : changes)
        {
            for (DockSpace *space : _spaces)
            {
                if (space->OwnsWindow(change.Window))
                {
                    space->OnWindowEvents(change.Window, change.Moved, change.Resized);
                }
            }
        }
    }
    for (DockSpace *space : _spaces)
    {
        space->Tick(_now);
    }
}

const DockType *DockingSubsystem::FindType(StringView name) const
{
    for (const DockType *type : _types)
    {
        if (GetName(*type) == name)
        {
            return type;
        }
    }
    return nullptr;
}

StringView DockingSubsystem::GetName(const DockType &type) const
{
    return _types.GetType(type).GetName();
}

void DockingSubsystem::Register(DockSpace *space)
{
    _spaces.Add(space);
}

void DockingSubsystem::Unregister(DockSpace *space)
{
    (void)_spaces.Remove(space);
}

DockSpace *DockingSubsystem::Find(const EditingContext *context) const
{
    for (DockSpace *space : _spaces)
    {
        if (&space->GetContext() == context)
        {
            return space;
        }
    }
    return nullptr;
}

DockSpace *DockingSubsystem::GetActiveSpace() const
{
    const auto *contexts = Editor::GetSubsystem<EditingContextSubsystem>();
    return contexts != nullptr ? Find(contexts->GetActive()) : nullptr;
}
} // namespace Marlin
