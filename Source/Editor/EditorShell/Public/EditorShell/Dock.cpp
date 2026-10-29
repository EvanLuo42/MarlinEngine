#include "EditorShell/Dock.h"

namespace Marlin
{
UIElement DockType::Build(EditingContext & /*context*/) const
{
    return Box();
}

namespace DockLayout
{
int32 AddSplit(List<DockNodeState> &nodes, int32 parent, DockAxis axis, float32 size)
{
    DockNodeState &node = nodes.AddDefaulted();
    node.Parent = parent;
    node.Size = size;
    node.Split = true;
    node.Axis = axis;
    return static_cast<int32>(nodes.Count() - 1);
}

int32 AddStack(List<DockNodeState> &nodes, int32 parent, std::initializer_list<StringView> docks, float32 size)
{
    DockNodeState &node = nodes.AddDefaulted();
    node.Parent = parent;
    node.Size = size;
    for (const StringView dock : docks)
    {
        node.Docks.Add(String(dock));
    }
    return static_cast<int32>(nodes.Count() - 1);
}
} // namespace DockLayout
} // namespace Marlin
