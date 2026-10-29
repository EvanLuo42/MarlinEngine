#pragma once

#include <initializer_list>

#include "Application/PlatformInput.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EditorUI/Signal.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "String/String.h"
#include "Templates/Function.h"
#include "UIPainter/UIGeometry.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
class UIPainter;
class UIWindowRoot;

/// A size in painter units, a percentage of the space the parent gives, or Auto: the content's size, or the
/// parent's cross size when stretched.
struct UILength
{
    enum class Unit : uint8
    {
        Auto,
        Points,
        Percent,
    };

    Unit Kind = Unit::Auto;
    float32 Value = 0;

    constexpr UILength() = default;
    constexpr UILength(float32 points) : Kind(Unit::Points), Value(points)
    {
    }

    [[nodiscard]] static constexpr UILength Auto()
    {
        return {};
    }

    [[nodiscard]] static constexpr UILength Percent(float32 percent)
    {
        UILength length;
        length.Kind = Unit::Percent;
        length.Value = percent;
        return length;
    }

    constexpr bool operator==(const UILength &) const = default;
};

struct UIEdges
{
    float32 Left = 0;
    float32 Top = 0;
    float32 Right = 0;
    float32 Bottom = 0;

    constexpr UIEdges() = default;
    constexpr UIEdges(float32 all) : Left(all), Top(all), Right(all), Bottom(all)
    {
    }
    constexpr UIEdges(float32 horizontal, float32 vertical)
        : Left(horizontal), Top(vertical), Right(horizontal), Bottom(vertical)
    {
    }
    constexpr UIEdges(float32 left, float32 top, float32 right, float32 bottom)
        : Left(left), Top(top), Right(right), Bottom(bottom)
    {
    }

    [[nodiscard]] constexpr float32 Horizontal() const
    {
        return Left + Right;
    }

    [[nodiscard]] constexpr float32 Vertical() const
    {
        return Top + Bottom;
    }

    constexpr bool operator==(const UIEdges &) const = default;
};

enum class UIDirection : uint8
{
    Row,
    Column,
};

/// Where an item sits on the cross axis; Auto, for AlignSelf only, follows the container's AlignItems.
enum class UIAlign : uint8
{
    Auto,
    Start,
    Center,
    End,
    Stretch,
};

/// How a flex container spreads the main-axis space its items leave.
enum class UIJustify : uint8
{
    Start,
    Center,
    End,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
};

enum class UILayoutKind : uint8
{
    Flex,
    Grid,
};

/// A grid column or row: a fixed size, its content's size, or a share of what fixed and auto tracks leave.
struct UIGridTrack
{
    enum class Unit : uint8
    {
        Auto,
        Points,
        Fraction,
    };

    Unit Kind = Unit::Auto;
    float32 Value = 0;

    [[nodiscard]] static constexpr UIGridTrack Auto()
    {
        return {};
    }

    [[nodiscard]] static constexpr UIGridTrack Points(float32 points)
    {
        return {Unit::Points, points};
    }

    [[nodiscard]] static constexpr UIGridTrack Fraction(float32 fraction)
    {
        return {Unit::Fraction, fraction};
    }

    constexpr bool operator==(const UIGridTrack &) const = default;
};

/// The layout and look of an element (09 §4.1).
// RECOVERY-DECISION D8: UIStyle, the event structs and UIElementKind were not recovered; defaults follow CSS flexbox.
struct UIStyle
{
    UILayoutKind Layout = UILayoutKind::Flex;
    UIDirection Direction = UIDirection::Column;
    UIJustify Justify = UIJustify::Start;
    UIAlign AlignItems = UIAlign::Stretch;
    UIAlign AlignSelf = UIAlign::Auto;
    /// Between flex items, and between grid columns and rows.
    float32 Gap = 0;
    UIEdges Padding;
    UIEdges Margin;
    UILength Width;
    UILength Height;
    float32 MinWidth = 0;
    float32 MinHeight = 0;
    float32 MaxWidth = 1e30f;
    float32 MaxHeight = 1e30f;
    float32 Grow = 0;
    float32 Shrink = 1;
    UILength Basis;
    List<UIGridTrack> Columns;
    List<UIGridTrack> Rows;
    /// The grid cell, 1-based; zero places the item in the next free cell.
    uint16 Column = 0;
    uint16 Row = 0;
    uint16 ColumnSpan = 1;
    uint16 RowSpan = 1;
    /// Above zero, a grid makes as many equal columns of at least this width as fit, and Columns is ignored (CSS
    /// repeat(auto-fill, minmax(width, 1fr))): tiles that wrap.
    float32 AutoFillColumnWidth = 0;
    LinearColor Background{0, 0, 0, 0};
    LinearColor BorderColor{0, 0, 0, 0};
    float32 BorderWidth = 0;
    UICornerRadii Radius;
    bool Clip = false;
    float32 Opacity = 1;
    /// False takes the element out of layout, painting and hit testing, keeping its subtree and state (CSS
    /// display: none).
    bool Visible = true;
    /// Out of the flow: sized to its content and placed at Offset (plus its margin) from its parent's content
    /// origin, over the items in the flow (CSS position: absolute). Popups and overlays.
    bool Absolute = false;
    Vector2 Offset;
    /// Moves the element and its subtree when painting and hit testing, without layout: drags and scrolling.
    Vector2 Translate;
    /// Scales the element and its subtree about its top-left corner when painting and hit testing, without layout:
    /// things that pop open.
    float32 Scale = 1;
};

struct UIPointerEvent
{
    /// In the window, in painter units.
    Vector2 Position;
    /// Relative to the element handling it.
    Vector2 LocalPosition;
    MouseButton Button = MouseButton::Left;
    ModifierKeys Modifiers = ModifierKeys::None;
    /// In notches; positive is right and away from the user.
    Vector2 Wheel;
    /// For presses, releases and clicks: 2 for the second of a double click, and so on.
    uint32 Clicks = 1;
    /// Set by a handler to stop the event bubbling further.
    bool Handled = false;
};

struct UIKeyEvent
{
    KeyCode Key = KeyCode::Unknown;
    ModifierKeys Modifiers = ModifierKeys::None;
    bool Repeat = false;
    bool Handled = false;
};

struct UITextInputEvent
{
    uint32 Character = 0;
    bool Handled = false;
};

enum class UIElementKind : uint8
{
    Box,
    Text,
    /// Holds the children of Show, For and Switch; laid out as if they were its parent's.
    Fragment,
};

/// One element of a window's tree (09 §4.1). Built once by a component function; afterwards only the
/// properties bound to signals change, marking layout dirty up to the root. Freed with the owner it was created in.
class EDITORUI_API UIElementNode
{
public:
    explicit UIElementNode(UIElementKind kind);
    ~UIElementNode();
    MARLIN_NONCOPYABLE(UIElementNode);

    [[nodiscard]] UIElementKind GetKind() const
    {
        return _kind;
    }

    [[nodiscard]] UIElementNode *GetParent() const
    {
        return _parent;
    }

    [[nodiscard]] Span<UIElementNode *const> GetChildren() const
    {
        return _children.AsSpan();
    }

    void AppendChild(UIElementNode *child);
    /// Makes *children* the children, in that order; children left out are detached, not freed.
    void SetChildren(Span<UIElementNode *const> children);
    void Detach();

    /// Its size or position may have changed: the next layout measures it and its ancestors again.
    void MarkLayoutDirty();

    /// Relative to the parent's origin; a fragment sits at its parent's origin.
    [[nodiscard]] const UIRect &GetBounds() const
    {
        return _bounds;
    }

    /// The origin in the window, in painter units.
    [[nodiscard]] Vector2 GetWindowPosition() const;

    [[nodiscard]] StringView GetText() const
    {
        return _text;
    }

    void SetText(StringView text);

    [[nodiscard]] float32 GetFontSize() const
    {
        return _fontSize;
    }

    void SetFontSize(float32 size);

    /// Empty for the default faces.
    [[nodiscard]] Span<const UIFontFace> GetFaces() const
    {
        return _faces;
    }

    void SetFaces(Span<const UIFontFace> faces);

    /// Paint-only properties may change at any time; layout ones must be followed by MarkLayoutDirty.
    UIStyle Style;
    LinearColor TextColor = LinearColor::White;

    Function<void(UIPointerEvent &)> OnPointerDown;
    Function<void(UIPointerEvent &)> OnPointerUp;
    Function<void(UIPointerEvent &)> OnPointerMove;
    Function<void(UIPointerEvent &)> OnClick;
    Function<void(UIPointerEvent &)> OnWheel;
    /// Neither bubbles: the pointer entered or left this element or one of its children.
    Function<void(UIPointerEvent &)> OnPointerEnter;
    Function<void(UIPointerEvent &)> OnPointerLeave;
    Function<void(UIKeyEvent &)> OnKeyDown;
    Function<void(UITextInputEvent &)> OnTextInput;
    Function<void(bool)> OnFocusChanged;
    /// Takes the keyboard when pressed.
    bool Focusable = false;
    /// Custom drawing over the background and under the children, in the element's window rectangle.
    Function<void(UIPainter &, const UIRect &)> OnPaint;
    /// In a window with its own title bar, pressing the element drags the window, except where an element under it
    /// takes pointer presses (UIWindowRoot::CollectHitRegions).
    bool WindowCaption = false;

private:
    friend class UIWindowRoot;
    friend struct UILayout;

    UIElementKind _kind;
    UIElementNode *_parent = nullptr;
    List<UIElementNode *> _children;
    // RECOVERY-DECISION D3: in the outline but read by nothing that survived; kept for the window a node belongs to.
    [[maybe_unused]] UIWindowRoot *_window = nullptr;
    UIRect _bounds;
    /// A Measure result and the space it was offered.
    struct MeasureEntry
    {
        Vector2 Available;
        Vector2 Size;
    };
    static constexpr uint32 MeasureCacheSize = 8;
    /// The latest Measure results, so that a clean subtree offered alternating sizes is not measured again; dropped
    /// when _measureValid is cleared, and replaced oldest first.
    MeasureEntry _measureCache[MeasureCacheSize];
    uint8 _measureCount = 0;
    uint8 _measureNext = 0;
    bool _measureValid = false;
    bool _layoutDirty = true;
    String _text;
    float32 _fontSize = 13;
    Span<const UIFontFace> _faces;
    UIShapedText _shaped;
    bool _shapeValid = false;
};

/// A property value: fixed, or read from a signal, computed or function, in which case the element follows it.
template <typename T> class UIValue
{
public:
    template <typename TArg>
        requires ConvertibleTo<TArg, T>
    UIValue(TArg &&value) : _value(static_cast<T>(Forward<TArg>(value)))
    {
    }

    // RECOVERY-DECISION D4: signals and computeds of any type convertible to T, as the session summary records.
    template <typename TValue>
        requires ConvertibleTo<TValue, T>
    UIValue(Signal<TValue> signal) : _getter([signal] { return static_cast<T>(signal.Get()); })
    {
    }

    template <typename TValue>
        requires ConvertibleTo<TValue, T>
    UIValue(Computed<TValue> computed) : _getter([computed] { return static_cast<T>(computed.Get()); })
    {
    }

    template <typename TFunction>
        requires(!ConvertibleTo<TFunction, T>) && requires(TFunction function) {
            { function() } -> ConvertibleTo<T>;
        }
    UIValue(TFunction function) : _getter([function] { return static_cast<T>(function()); })
    {
    }

    [[nodiscard]] bool IsReactive() const
    {
        return _getter.IsBound();
    }

    [[nodiscard]] T Get() const
    {
        return _getter.IsBound() ? _getter() : *_value;
    }

private:
    Optional<T> _value;
    Function<T()> _getter;
};

namespace Private
{
/// Applies *value* to *node* now, and again whenever it changes if it is reactive.
template <typename T, typename TApply> void BindValue(UIElementNode *node, UIValue<T> value, TApply apply)
{
    if (!value.IsReactive())
    {
        apply(*node, value.Get());
        return;
    }
    CreateEffect([node, value = Move(value), apply] { apply(*node, value.Get()); });
}

EDITORUI_API UIElementNode *CreateNode(UIElementKind kind);
} // namespace Private

/// A handle to an element being built; setters return it for chaining and take fixed or reactive values.
class EDITORUI_API UIElement
{
public:
    UIElement() = default;
    explicit UIElement(UIElementNode *node) : _node(node)
    {
    }

    [[nodiscard]] UIElementNode *GetNode() const
    {
        return _node;
    }

    UIElement &Direction(UIValue<UIDirection> value);
    UIElement &Justify(UIValue<UIJustify> value);
    UIElement &AlignItems(UIValue<UIAlign> value);
    UIElement &AlignSelf(UIValue<UIAlign> value);
    UIElement &Gap(UIValue<float32> value);
    UIElement &Padding(UIValue<UIEdges> value);
    UIElement &Margin(UIValue<UIEdges> value);
    UIElement &Width(UIValue<UILength> value);
    UIElement &Height(UIValue<UILength> value);
    UIElement &MinWidth(UIValue<float32> value);
    UIElement &MinHeight(UIValue<float32> value);
    UIElement &MaxWidth(UIValue<float32> value);
    UIElement &MaxHeight(UIValue<float32> value);
    UIElement &Grow(UIValue<float32> value);
    UIElement &Shrink(UIValue<float32> value);
    UIElement &Basis(UIValue<UILength> value);
    UIElement &Columns(std::initializer_list<UIGridTrack> tracks);
    UIElement &Rows(std::initializer_list<UIGridTrack> tracks);
    /// 1-based grid lines.
    UIElement &Cell(uint16 column, uint16 row, uint16 columnSpan = 1, uint16 rowSpan = 1);
    UIElement &AutoFillColumns(UIValue<float32> minWidth);
    UIElement &Background(UIValue<LinearColor> value);
    UIElement &Border(UIValue<float32> width, UIValue<LinearColor> color);
    UIElement &Radius(UIValue<UICornerRadii> value);
    UIElement &Clip(bool clip = true);
    UIElement &Opacity(UIValue<float32> value);
    UIElement &TextColor(UIValue<LinearColor> value);
    UIElement &FontSize(UIValue<float32> value);
    UIElement &Font(Span<const UIFontFace> faces);
    UIElement &Focusable(bool focusable = true);
    UIElement &Visible(UIValue<bool> value);
    /// Takes the element out of the flow and places it at *offset* from the parent's content origin.
    UIElement &Absolute(UIValue<Vector2> offset);
    UIElement &Translate(UIValue<Vector2> value);
    UIElement &Scale(UIValue<float32> value);
    UIElement &OnPaint(Function<void(UIPainter &, const UIRect &)> paint);
    UIElement &WindowCaption(bool caption = true);

    UIElement &OnPointerDown(Function<void(UIPointerEvent &)> handler);
    UIElement &OnPointerUp(Function<void(UIPointerEvent &)> handler);
    UIElement &OnPointerMove(Function<void(UIPointerEvent &)> handler);
    UIElement &OnPointerEnter(Function<void(UIPointerEvent &)> handler);
    UIElement &OnPointerLeave(Function<void(UIPointerEvent &)> handler);
    UIElement &OnClick(Function<void(UIPointerEvent &)> handler);
    UIElement &OnWheel(Function<void(UIPointerEvent &)> handler);
    UIElement &OnKeyDown(Function<void(UIKeyEvent &)> handler);
    UIElement &OnTextInput(Function<void(UITextInputEvent &)> handler);
    UIElement &OnFocusChanged(Function<void(bool)> handler);

private:
    UIElementNode *_node = nullptr;
};

/// A flex container, a column unless Direction says otherwise.
EDITORUI_API UIElement Box(std::initializer_list<UIElement> children = {});
EDITORUI_API UIElement Row(std::initializer_list<UIElement> children = {});
EDITORUI_API UIElement Column(std::initializer_list<UIElement> children = {});
/// A grid container: Columns and Rows give the tracks; children fill cells in order unless Cell places them.
EDITORUI_API UIElement Grid(std::initializer_list<UIElement> children = {});
/// One line of text.
EDITORUI_API UIElement Text(UIValue<String> text);

/// *content* while *when* is true, otherwise *fallback* (if any); each is built when it starts showing and freed
/// when it stops.
EDITORUI_API UIElement Show(UIValue<bool> when, Function<UIElement()> content, Function<UIElement()> fallback = {});

/// One branch of a Switch.
struct UIMatch
{
    UIValue<bool> When;
    Function<UIElement()> Content;
};

[[nodiscard]] inline UIMatch Match(UIValue<bool> when, Function<UIElement()> content)
{
    return {Move(when), Move(content)};
}

/// The branch a Switch takes when no other matches.
[[nodiscard]] inline UIMatch Default(Function<UIElement()> content)
{
    return {true, Move(content)};
}

namespace Private
{
EDITORUI_API UIElement BuildSwitch(List<UIMatch> matches);
} // namespace Private

/// The first match whose condition holds, or nothing; rebuilt only when a different one wins.
template <typename... TMatches> UIElement Switch(TMatches &&...matches)
{
    List<UIMatch> list;
    (list.Add(Forward<TMatches>(matches)), ...);
    return Private::BuildSwitch(Move(list));
}

/// A row per item of *items*, matched by *key* across changes: a row whose key stays keeps its elements and state
/// and moves to its new place; its item signal takes the new value. Rows of keys that leave are freed.
template <typename T>
UIElement For(UIValue<List<T>> items, Function<uint64(const T &)> key, Function<UIElement(Signal<T>)> build)
{
    struct ForRow
    {
        uint64 Key = 0;
        ReactiveOwner *Owner = nullptr;
        UIElementNode *Node = nullptr;
        Signal<T> Item;
    };
    UIElementNode *fragment = Private::CreateNode(UIElementKind::Fragment);
    ReactiveOwner *rowsOwner = Reactive::CreateOwner(Reactive::GetOwner());
    auto *rows = new List<ForRow>();
    Reactive::OnCleanup([rows] { delete rows; });
    CreateEffect([fragment, rowsOwner, rows, items = Move(items), key = Move(key), build = Move(build)] {
        const List<T> current = items.Get();
        Reactive::Untrack([&] {
            HashMap<uint64, usize> previous;
            for (usize index = 0; index < rows->Count(); ++index)
            {
                previous.FindOrAdd((*rows)[index].Key) = index;
            }
            List<ForRow> next;
            List<UIElementNode *> nodes;
            next.Reserve(current.Count());
            nodes.Reserve(current.Count());
            for (const T &item : current)
            {
                const uint64 itemKey = key(item);
                const usize *found = previous.Find(itemKey);
                if (found != nullptr && (*rows)[*found].Owner != nullptr)
                {
                    ForRow &row = (*rows)[*found];
                    row.Item.Set(item);
                    next.Add(row);
                    row.Owner = nullptr;
                }
                else
                {
                    ForRow row;
                    row.Key = itemKey;
                    row.Owner = Reactive::CreateOwner(rowsOwner);
                    Reactive::RunInOwner(row.Owner, [&] {
                        row.Item = CreateSignal(item);
                        row.Node = build(row.Item).GetNode();
                    });
                    next.Add(row);
                }
                nodes.Add(next.Last().Node);
            }
            for (const ForRow &row : *rows)
            {
                if (row.Owner != nullptr)
                {
                    Reactive::DisposeOwner(row.Owner);
                }
            }
            fragment->SetChildren(nodes.AsSpan());
            *rows = Move(next);
        });
    });
    return UIElement(fragment);
}
} // namespace Marlin
