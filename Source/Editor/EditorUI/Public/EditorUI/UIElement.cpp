#include "EditorUI/UIElement.h"

#include "EditorUI/UIWindowRegistry.h"
#include "EditorUI/UIWindowRoot.h"

namespace Marlin
{
UIElementNode::UIElementNode(UIElementKind kind) : _kind(kind)
{
}

UIElementNode::~UIElementNode()
{
    for (UIElementNode *child : _children)
    {
        child->_parent = nullptr;
    }
    Detach();
    Private::NotifyNodeDestroyed(this);
}

void UIElementNode::AppendChild(UIElementNode *child)
{
    child->Detach();
    child->_parent = this;
    _children.Add(child);
    MarkLayoutDirty();
}

void UIElementNode::SetChildren(Span<UIElementNode *const> children)
{
    for (UIElementNode *child : _children)
    {
        child->_parent = nullptr;
    }
    _children.Clear();
    for (UIElementNode *child : children)
    {
        if (child->_parent != nullptr)
        {
            child->Detach();
        }
        child->_parent = this;
        _children.Add(child);
    }
    MarkLayoutDirty();
}

void UIElementNode::Detach()
{
    if (_parent == nullptr)
    {
        return;
    }
    UIElementNode *parent = _parent;
    _parent = nullptr;
    for (usize index = 0; index < parent->_children.Count(); ++index)
    {
        if (parent->_children[index] == this)
        {
            parent->_children.RemoveAt(index);
            break;
        }
    }
    parent->MarkLayoutDirty();
}

void UIElementNode::MarkLayoutDirty()
{
    for (UIElementNode *node = this; node != nullptr && (node->_measureValid || !node->_layoutDirty);
         node = node->_parent)
    {
        node->_measureValid = false;
        node->_layoutDirty = true;
    }
}

void UIElementNode::SetText(StringView text)
{
    if (_text != text)
    {
        _text = String(text);
        _shapeValid = false;
        MarkLayoutDirty();
    }
}

void UIElementNode::SetFontSize(float32 size)
{
    if (_fontSize != size)
    {
        _fontSize = size;
        _shapeValid = false;
        MarkLayoutDirty();
    }
}

void UIElementNode::SetFaces(Span<const UIFontFace> faces)
{
    _faces = faces;
    _shapeValid = false;
    MarkLayoutDirty();
}

Vector2 UIElementNode::GetWindowPosition() const
{
    Vector2 position;
    for (const UIElementNode *node = this; node != nullptr; node = node->_parent)
    {
        position += Vector2(node->_bounds.X, node->_bounds.Y) + node->Style.Translate;
    }
    return position;
}

namespace Private
{
UIElementNode *CreateNode(UIElementKind kind)
{
    auto *node = new UIElementNode(kind);
    Reactive::OnCleanup([node] { delete node; });
    return node;
}
} // namespace Private

namespace
{
template <typename T, typename TApply> UIElement &Bind(UIElement &element, UIValue<T> value, TApply apply)
{
    Private::BindValue(element.GetNode(), Move(value), [apply](UIElementNode &node, const T &next) {
        apply(node, next);
        node.MarkLayoutDirty();
    });
    return element;
}

UIElement Container(std::initializer_list<UIElement> children, UILayoutKind layout, UIDirection direction)
{
    UIElementNode *node = Private::CreateNode(UIElementKind::Box);
    node->Style.Layout = layout;
    node->Style.Direction = direction;
    for (const UIElement &child : children)
    {
        if (child.GetNode() != nullptr)
        {
            node->AppendChild(child.GetNode());
        }
    }
    return UIElement(node);
}
} // namespace

UIElement &UIElement::Direction(UIValue<UIDirection> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UIDirection next) { node.Style.Direction = next; });
}

UIElement &UIElement::Justify(UIValue<UIJustify> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UIJustify next) { node.Style.Justify = next; });
}

UIElement &UIElement::AlignItems(UIValue<UIAlign> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UIAlign next) { node.Style.AlignItems = next; });
}

UIElement &UIElement::AlignSelf(UIValue<UIAlign> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UIAlign next) { node.Style.AlignSelf = next; });
}

UIElement &UIElement::Gap(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.Gap = next; });
}

UIElement &UIElement::Padding(UIValue<UIEdges> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, const UIEdges &next) { node.Style.Padding = next; });
}

UIElement &UIElement::Margin(UIValue<UIEdges> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, const UIEdges &next) { node.Style.Margin = next; });
}

UIElement &UIElement::Width(UIValue<UILength> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UILength next) { node.Style.Width = next; });
}

UIElement &UIElement::Height(UIValue<UILength> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UILength next) { node.Style.Height = next; });
}

UIElement &UIElement::MinWidth(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.MinWidth = next; });
}

UIElement &UIElement::MinHeight(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.MinHeight = next; });
}

UIElement &UIElement::MaxWidth(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.MaxWidth = next; });
}

UIElement &UIElement::MaxHeight(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.MaxHeight = next; });
}

UIElement &UIElement::Grow(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.Grow = next; });
}

UIElement &UIElement::Shrink(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.Style.Shrink = next; });
}

UIElement &UIElement::Basis(UIValue<UILength> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, UILength next) { node.Style.Basis = next; });
}

UIElement &UIElement::Columns(std::initializer_list<UIGridTrack> tracks)
{
    _node->Style.Columns.Clear();
    for (const UIGridTrack &track : tracks)
    {
        _node->Style.Columns.Add(track);
    }
    _node->MarkLayoutDirty();
    return *this;
}

UIElement &UIElement::Rows(std::initializer_list<UIGridTrack> tracks)
{
    _node->Style.Rows.Clear();
    for (const UIGridTrack &track : tracks)
    {
        _node->Style.Rows.Add(track);
    }
    _node->MarkLayoutDirty();
    return *this;
}

UIElement &UIElement::Cell(uint16 column, uint16 row, uint16 columnSpan, uint16 rowSpan)
{
    _node->Style.Column = column;
    _node->Style.Row = row;
    _node->Style.ColumnSpan = columnSpan > 0 ? columnSpan : 1;
    _node->Style.RowSpan = rowSpan > 0 ? rowSpan : 1;
    _node->MarkLayoutDirty();
    return *this;
}

UIElement &UIElement::AutoFillColumns(UIValue<float32> minWidth)
{
    return Bind(*this, Move(minWidth), [](UIElementNode &node, float32 next) {
        node.Style.AutoFillColumnWidth = next;
        node.MarkLayoutDirty();
    });
}

UIElement &UIElement::Background(UIValue<LinearColor> value)
{
    Private::BindValue(_node, Move(value),
                       [](UIElementNode &node, const LinearColor &next) { node.Style.Background = next; });
    return *this;
}

UIElement &UIElement::Border(UIValue<float32> width, UIValue<LinearColor> color)
{
    Private::BindValue(_node, Move(color),
                       [](UIElementNode &node, const LinearColor &next) { node.Style.BorderColor = next; });
    return Bind(*this, Move(width), [](UIElementNode &node, float32 next) { node.Style.BorderWidth = next; });
}

UIElement &UIElement::Radius(UIValue<UICornerRadii> value)
{
    Private::BindValue(_node, Move(value),
                       [](UIElementNode &node, const UICornerRadii &next) { node.Style.Radius = next; });
    return *this;
}

UIElement &UIElement::Clip(bool clip)
{
    _node->Style.Clip = clip;
    return *this;
}

UIElement &UIElement::Visible(UIValue<bool> value)
{
    Private::BindValue(_node, Move(value), [](UIElementNode &node, bool next) {
        node.Style.Visible = next;
        node.MarkLayoutDirty();
        // A hidden node is never laid out and so stays dirty, which stops MarkLayoutDirty at it: the parent that
        // places it must hear of the change itself.
        if (UIElementNode *parent = node.GetParent())
        {
            parent->MarkLayoutDirty();
        }
    });
    return *this;
}

UIElement &UIElement::Absolute(UIValue<Vector2> offset)
{
    _node->Style.Absolute = true;
    return Bind(*this, Move(offset), [](UIElementNode &node, const Vector2 &next) { node.Style.Offset = next; });
}

UIElement &UIElement::Translate(UIValue<Vector2> value)
{
    Private::BindValue(_node, Move(value),
                       [](UIElementNode &node, const Vector2 &next) { node.Style.Translate = next; });
    return *this;
}

UIElement &UIElement::Scale(UIValue<float32> value)
{
    Private::BindValue(_node, Move(value), [](UIElementNode &node, float32 next) { node.Style.Scale = next; });
    return *this;
}

UIElement &UIElement::OnPaint(Function<void(UIPainter &, const UIRect &)> paint)
{
    _node->OnPaint = Move(paint);
    return *this;
}

UIElement &UIElement::WindowCaption(bool caption)
{
    _node->WindowCaption = caption;
    return *this;
}

UIElement &UIElement::Opacity(UIValue<float32> value)
{
    Private::BindValue(_node, Move(value), [](UIElementNode &node, float32 next) { node.Style.Opacity = next; });
    return *this;
}

UIElement &UIElement::TextColor(UIValue<LinearColor> value)
{
    Private::BindValue(_node, Move(value), [](UIElementNode &node, const LinearColor &next) { node.TextColor = next; });
    return *this;
}

UIElement &UIElement::FontSize(UIValue<float32> value)
{
    return Bind(*this, Move(value), [](UIElementNode &node, float32 next) { node.SetFontSize(next); });
}

UIElement &UIElement::Font(Span<const UIFontFace> faces)
{
    _node->SetFaces(faces);
    return *this;
}

UIElement &UIElement::Focusable(bool focusable)
{
    _node->Focusable = focusable;
    return *this;
}

UIElement &UIElement::OnPointerDown(Function<void(UIPointerEvent &)> handler)
{
    _node->OnPointerDown = Move(handler);
    return *this;
}

UIElement &UIElement::OnPointerUp(Function<void(UIPointerEvent &)> handler)
{
    _node->OnPointerUp = Move(handler);
    return *this;
}

UIElement &UIElement::OnPointerMove(Function<void(UIPointerEvent &)> handler)
{
    _node->OnPointerMove = Move(handler);
    return *this;
}

UIElement &UIElement::OnPointerEnter(Function<void(UIPointerEvent &)> handler)
{
    _node->OnPointerEnter = Move(handler);
    return *this;
}

UIElement &UIElement::OnPointerLeave(Function<void(UIPointerEvent &)> handler)
{
    _node->OnPointerLeave = Move(handler);
    return *this;
}

UIElement &UIElement::OnClick(Function<void(UIPointerEvent &)> handler)
{
    _node->OnClick = Move(handler);
    return *this;
}

UIElement &UIElement::OnWheel(Function<void(UIPointerEvent &)> handler)
{
    _node->OnWheel = Move(handler);
    return *this;
}

UIElement &UIElement::OnKeyDown(Function<void(UIKeyEvent &)> handler)
{
    _node->OnKeyDown = Move(handler);
    return *this;
}

UIElement &UIElement::OnTextInput(Function<void(UITextInputEvent &)> handler)
{
    _node->OnTextInput = Move(handler);
    return *this;
}

UIElement &UIElement::OnFocusChanged(Function<void(bool)> handler)
{
    _node->OnFocusChanged = Move(handler);
    return *this;
}

UIElement Box(std::initializer_list<UIElement> children)
{
    return Container(children, UILayoutKind::Flex, UIDirection::Column);
}

UIElement Row(std::initializer_list<UIElement> children)
{
    return Container(children, UILayoutKind::Flex, UIDirection::Row);
}

UIElement Column(std::initializer_list<UIElement> children)
{
    return Container(children, UILayoutKind::Flex, UIDirection::Column);
}

UIElement Grid(std::initializer_list<UIElement> children)
{
    return Container(children, UILayoutKind::Grid, UIDirection::Row);
}

UIElement Text(UIValue<String> text)
{
    UIElement element(Private::CreateNode(UIElementKind::Text));
    return Bind(element, Move(text), [](UIElementNode &node, const String &next) { node.SetText(next); });
}

UIElement Show(UIValue<bool> when, Function<UIElement()> content, Function<UIElement()> fallback)
{
    UIElementNode *fragment = Private::CreateNode(UIElementKind::Fragment);
    const Computed<bool> shown = CreateComputed<bool>([when = Move(when)] { return when.Get(); });
    CreateEffect([fragment, shown, content = Move(content), fallback = Move(fallback)] {
        const bool on = shown.Get();
        Reactive::Untrack([&] {
            const Function<UIElement()> &build = on ? content : fallback;
            UIElementNode *child = build.IsBound() ? build().GetNode() : nullptr;
            fragment->SetChildren(child != nullptr ? Span<UIElementNode *const>(&child, 1)
                                                   : Span<UIElementNode *const>());
        });
    });
    return UIElement(fragment);
}

namespace Private
{
UIElement BuildSwitch(List<UIMatch> matches)
{
    UIElementNode *fragment = CreateNode(UIElementKind::Fragment);
    auto *branches = new List<UIMatch>(Move(matches));
    Reactive::OnCleanup([branches] { delete branches; });
    const Computed<int32> selected = CreateComputed<int32>([branches] {
        for (usize index = 0; index < branches->Count(); ++index)
        {
            if ((*branches)[index].When.Get())
            {
                return static_cast<int32>(index);
            }
        }
        return -1;
    });
    CreateEffect([fragment, branches, selected] {
        const int32 index = selected.Get();
        Reactive::Untrack([&] {
            UIElementNode *child = index >= 0 ? (*branches)[static_cast<usize>(index)].Content().GetNode() : nullptr;
            fragment->SetChildren(child != nullptr ? Span<UIElementNode *const>(&child, 1)
                                                   : Span<UIElementNode *const>());
        });
    });
    return UIElement(fragment);
}
} // namespace Private
} // namespace Marlin
