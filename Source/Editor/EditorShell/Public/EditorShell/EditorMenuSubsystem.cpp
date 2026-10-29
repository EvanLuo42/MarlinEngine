#include "EditorShell/EditorMenuSubsystem.h"

namespace Marlin
{
List<String> EditorMenuSubsystem::MenuBar;

EditorMenuSubsystem::EditorMenuSubsystem() = default;
EditorMenuSubsystem::~EditorMenuSubsystem() = default;

void EditorMenuSubsystem::Initialize(SubsystemContext & /*context*/)
{
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] { _version = CreateSignal<uint32>(0); });
}

void EditorMenuSubsystem::Deinitialize()
{
    _menus.Clear();
    Reactive::DisposeOwner(_owner);
    _owner = nullptr;
}

void EditorMenuSubsystem::AddItem(StringView menu, EditorMenuItem item)
{
    Menu *target = nullptr;
    for (Menu &existing : _menus)
    {
        if (existing.Name == menu)
        {
            target = &existing;
        }
    }
    if (target == nullptr)
    {
        target = &_menus.Add({String(menu), {}});
    }
    usize index = target->Items.Count();
    while (index > 0 && target->Items[index - 1].Order > item.Order)
    {
        --index;
    }
    target->Items.Insert(index, Move(item));
    _version.Set(_version.Peek() + 1);
}

Span<const EditorMenuItem> EditorMenuSubsystem::GetItems(StringView menu) const
{
    (void)_version.Get();
    for (const Menu &existing : _menus)
    {
        if (existing.Name == menu)
        {
            return existing.Items.AsSpan();
        }
    }
    return {};
}
} // namespace Marlin
