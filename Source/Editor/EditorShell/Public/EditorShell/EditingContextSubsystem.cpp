#include "EditorShell/EditingContextSubsystem.h"

#include "Diagnostics/Log.h"
#include "EditorShell/EditorMenuSubsystem.h"

namespace Marlin
{
EditingContextSubsystem::EditingContextSubsystem() = default;
EditingContextSubsystem::~EditingContextSubsystem() = default;

void EditingContextSubsystem::Initialize(SubsystemContext &context)
{
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] {
        _order = CreateSignal(List<EditingContext *>());
        _active = CreateSignal<EditingContext *>(nullptr);
    });
    _assetTypes.Build();
    _toolTypes.Build();
    _pinnedTypes.Build();

    auto *menus = context.Collection != nullptr ? context.Collection->Get<EditorMenuSubsystem>() : nullptr;
    for (const PinnedContextType *type : _pinnedTypes)
    {
        EditingContext *pinned = OpenContext([type] { return type->Create(); });
        if (pinned != nullptr && menus != nullptr)
        {
            EditorMenuItem item;
            item.Label = String(type->GetDisplayName());
            item.Execute = [this, pinned] { Activate(pinned); };
            menus->AddItem("Window", Move(item));
        }
    }
    if (!_order.Peek().IsEmpty())
    {
        Activate(_order.Peek()[0]);
    }

    if (menus != nullptr)
    {
        for (ToolContextType *type : _toolTypes)
        {
            EditorMenuItem tool;
            tool.Label = String(type->GetDisplayName());
            tool.Order = 100;
            tool.Execute = [this, type] { (void)OpenTool(*type); };
            menus->AddItem(type->GetMenu(), Move(tool));
        }
    }
}

void EditingContextSubsystem::Deinitialize()
{
    for (usize index = _entries.Count(); index-- > 0;)
    {
        _entries[index].Context.Reset();
        Reactive::DisposeOwner(_entries[index].Owner);
    }
    _entries.Clear();
    Reactive::DisposeOwner(_owner);
    _owner = nullptr;
}

EditingContext *EditingContextSubsystem::Add(ReactiveOwner *owner, UniquePtr<EditingContext> context)
{
    EditingContext *added = context.Get();
    _entries.Add({Move(context), owner});
    _order.Update([&](List<EditingContext *> &order) { order.Add(added); });
    return added;
}

usize EditingContextSubsystem::GetPinnedCount() const
{
    usize count = 0;
    for (const EditingContext *context : _order.Peek())
    {
        count += context->IsPinned() ? 1 : 0;
    }
    return count;
}

EditingContext *EditingContextSubsystem::OpenAsset(const Guid &asset, StringView assetType, StringView name)
{
    for (EditingContext *open : _order.Peek())
    {
        if (open->GetAsset() == asset)
        {
            Activate(open);
            return open;
        }
    }
    for (const AssetContextType *type : _assetTypes)
    {
        if (!type->CanEdit(assetType))
        {
            continue;
        }
        return OpenContext([&] { return type->Create(asset, name); });
    }
    MARLIN_LOG(LogCore, Warning, "No editing context edits {0} ({1})", name, assetType);
    return nullptr;
}

EditingContext *EditingContextSubsystem::OpenContext(FunctionRef<UniquePtr<EditingContext>()> create)
{
    ReactiveOwner *owner = Reactive::CreateOwner(nullptr);
    UniquePtr<EditingContext> created;
    Reactive::RunInOwner(owner, [&] { created = create(); });
    if (!created)
    {
        Reactive::DisposeOwner(owner);
        return nullptr;
    }
    EditingContext *opened = Add(owner, Move(created));
    Activate(opened);
    return opened;
}

EditingContext *EditingContextSubsystem::OpenTool(const ToolContextType &type)
{
    for (EditingContext *open : _order.Peek())
    {
        if (open->GetType() == &type)
        {
            Activate(open);
            return open;
        }
    }
    return OpenContext([&] { return type.Create(); });
}

void EditingContextSubsystem::Activate(EditingContext *context)
{
    EditingContext *previous = _active.Peek();
    if (context == previous)
    {
        return;
    }
    if (previous != nullptr)
    {
        previous->OnDeactivated();
    }
    _active.Set(context);
    if (context != nullptr)
    {
        context->OnActivated();
    }
}

bool EditingContextSubsystem::Close(EditingContext *context)
{
    const List<EditingContext *> &order = _order.Peek();
    const usize index = order.IndexOf(context);
    if (index == List<EditingContext *>::NotFound || context->IsPinned() || !context->CanClose())
    {
        return false;
    }
    if (_active.Peek() == context)
    {
        Activate(index + 1 < order.Count() ? order[index + 1] : order[index - 1]);
    }
    _order.Update([&](List<EditingContext *> &list) { list.RemoveAt(index); });
    for (usize entry = 0; entry < _entries.Count(); ++entry)
    {
        if (_entries[entry].Context.Get() == context)
        {
            // Run the effects that read the context (its tab) out before freeing it.
            Reactive::Flush();
            _entries[entry].Context.Reset();
            Reactive::DisposeOwner(_entries[entry].Owner);
            _entries.RemoveAt(entry);
            break;
        }
    }
    return true;
}

void EditingContextSubsystem::Reorder(EditingContext *context, usize index)
{
    const usize from = _order.Peek().IndexOf(context);
    if (from == List<EditingContext *>::NotFound || context->IsPinned())
    {
        return;
    }
    const usize pinned = GetPinnedCount();
    const usize last = _order.Peek().Count() - 1;
    const usize to = index < pinned ? pinned : index > last ? last : index;
    if (to == from)
    {
        return;
    }
    _order.Update([&](List<EditingContext *> &list) {
        list.RemoveAt(from);
        list.Insert(to, context);
    });
}

void EditingContextSubsystem::OpenAssetCommand(const String &uuid, const String &name)
{
    auto *contexts = Editor::GetSubsystem<EditingContextSubsystem>();
    Guid asset;
    if (contexts == nullptr || !Guid::TryParse(uuid.AsView(), asset))
    {
        MARLIN_LOG(LogCore, Warning, "Editor.OpenAsset: {0} is not an asset UUID, or the editor is not running", uuid);
        return;
    }
    (void)contexts->OpenAsset(asset, {}, name.AsView());
}

void EditingContextSubsystem::OpenToolCommand(const String &name)
{
    auto *contexts = Editor::GetSubsystem<EditingContextSubsystem>();
    if (contexts == nullptr)
    {
        return;
    }
    for (const ToolContextType *type : contexts->_toolTypes)
    {
        if (type->GetDisplayName() == name.AsView())
        {
            (void)contexts->OpenTool(*type);
            return;
        }
    }
    MARLIN_LOG(LogCore, Warning, "Editor.OpenTool: no tool is named {0}", name);
}
} // namespace Marlin
