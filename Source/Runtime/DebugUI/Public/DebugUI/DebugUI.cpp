#include "DebugUI/DebugUI.h"

#include "DebugUI/DebugUIContext.h"
#include "Hash/Xxh3.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/Utf.h"
#include "Templates/Utility.h"
#include "UIPainter/UIPath.h"

namespace Marlin
{
namespace
{
constinit DebugUIContext *CurrentContext = nullptr;

constexpr float32 TitleHeight = 18;
constexpr float32 TreeIndent = 12;
constexpr float32 Baseline = 12;
constexpr float32 ScrollStep = 48;
constexpr float32 ScrollBarWidth = 4;

StringView VisibleLabel(StringView label)
{
    const usize hidden = label.IndexOf("##");
    return hidden == StringView::NotFound ? label : label.Left(hidden);
}

bool Contains(const UIRect &rect, Vector2 point)
{
    return point.X >= rect.X && point.Y >= rect.Y && point.X < rect.X + rect.Width && point.Y < rect.Y + rect.Height;
}

DebugUIContext *Window()
{
    DebugUIContext *context = CurrentContext;
    return context != nullptr && context->InWindow ? context : nullptr;
}
} // namespace

DebugUIContext *GetDebugUIContext()
{
    return CurrentContext;
}

void SetDebugUIContext(DebugUIContext *context)
{
    CurrentContext = context;
}

void DebugUIContext::BeginFrame(UIPainter &painter, const UITextSubsystem &text, Span<const UIFontFace> faces,
                                Span<const ApplicationEvent> events, WindowHandle window)
{
    // Shaped text is kept while the faces and text subsystem stay and every face is loaded.
    if (Text != &text || Faces.Data() != faces.Data() || Faces.Count() != faces.Count())
    {
        ShapedTexts.Clear();
    }
    Painter = &painter;
    Text = &text;
    Faces = faces;
    ++FrameNumber;
    ShapedTexts.RemoveIf([this](const auto &entry) { return entry.Value.Frame + 1 < FrameNumber; });
    FacesReady = true;
    for (const UIFontFace face : faces)
    {
        FacesReady = FacesReady && text.IsReady(face);
    }
    ScreenSize = painter.GetSize();
    const float32 scale = painter.GetScale();
    MousePressed = false;
    MouseReleased = false;
    WheelY = 0;
    KeysPressed.Clear();
    Characters.Clear();
    const Vector2 previous = Mouse;
    for (const ApplicationEvent &event : events)
    {
        if (event.Window != window)
        {
            continue;
        }
        switch (event.Type)
        {
        case ApplicationEventType::MouseMove:
            Mouse = Vector2(static_cast<float32>(event.X), static_cast<float32>(event.Y)) / scale;
            break;
        case ApplicationEventType::MouseButtonDown:
            Mouse = Vector2(static_cast<float32>(event.X), static_cast<float32>(event.Y)) / scale;
            if (event.Button == MouseButton::Left)
            {
                MouseDown = true;
                MousePressed = true;
            }
            break;
        case ApplicationEventType::MouseButtonUp:
            if (event.Button == MouseButton::Left)
            {
                MouseDown = false;
                MouseReleased = true;
            }
            break;
        case ApplicationEventType::MouseWheel:
            WheelY += event.WheelY;
            break;
        case ApplicationEventType::KeyDown:
            KeysPressed.Add(event.Key);
            break;
        case ApplicationEventType::TextInput:
            if (event.Character >= 0x20 && event.Character != 0x7F)
            {
                Characters.Add(static_cast<char32_t>(event.Character));
            }
            break;
        default:
            break;
        }
    }
    MouseDelta = previous.X < -1e5f ? Vector2() : Mouse - previous;
    MouseOverWindowLastFrame = MouseOverWindow;
    MouseOverWindow = false;
    Hovered = 0;
    if (!MouseDown && !MouseReleased)
    {
        Active = 0;
    }
    if (MousePressed)
    {
        Focused = 0;
    }
    BannerY = 8;
    IdStack.Clear();
    IdStack.Add(0x9E3779B97F4A7C15ull);
}

void DebugUIContext::EndFrame()
{
    if (MouseReleased)
    {
        Active = 0;
    }
}

uint64 DebugUIContext::MakeId(StringView label) const
{
    return Xxh3Hash64(label.Data(), label.Length(), IdStack.Last());
}

const UIShapedText &DebugUIContext::GetShaped(StringView text)
{
    if (!FacesReady)
    {
        Text->Shape(text, {Faces, DebugUIStyle::FontSize}, UncachedShape);
        return UncachedShape;
    }
    ShapedEntry &entry = ShapedTexts.FindOrAdd(Xxh3Hash64(text.Data(), text.Length(), 0));
    if (entry.Frame == 0 || entry.Text != text)
    {
        entry.Text = String(text);
        Text->Shape(text, {Faces, DebugUIStyle::FontSize}, entry.Shaped);
    }
    entry.Frame = FrameNumber;
    return entry.Shaped;
}

float32 DebugUIContext::MeasureText(StringView text)
{
    return GetShaped(text).Width;
}

void DebugUIContext::DrawText(StringView text, Vector2 baseline, const LinearColor &color)
{
    Marlin::DrawText(*Painter, GetShaped(text), baseline, color);
}

bool DebugUIContext::TakesInput() const
{
    return (WindowFlags & DebugWindowFlags::NoInput) == DebugWindowFlags::None;
}

bool DebugUIContext::Behavior(uint64 id, const UIRect &rect, bool &hovered, bool &held)
{
    hovered = TakesInput() && Contains(rect, Mouse) && Contains(ClipRect, Mouse) && (Active == 0 || Active == id);
    if (hovered)
    {
        Hovered = id;
        if (MousePressed)
        {
            Active = id;
        }
    }
    held = Active == id && MouseDown;
    return Active == id && MouseReleased && hovered;
}

float32 DebugUIContext::ContentLeft() const
{
    return WindowPosition.X + DebugUIStyle::Padding + Indent;
}

float32 DebugUIContext::ContentWidth() const
{
    return WindowWidth - DebugUIStyle::Padding * 2 - Indent;
}

UIRect DebugUIContext::NextRow(float32 height)
{
    const UIRect row(ContentLeft(), CursorY, ContentWidth(), height);
    CursorY += height;
    return row;
}

namespace DebugUI
{
bool BeginWindow(StringView title, Vector2 position, float32 width, DebugWindowFlags flags, bool *open)
{
    DebugUIContext *context = CurrentContext;
    if (context == nullptr || context->InWindow)
    {
        return false;
    }
    context->InWindow = true;
    context->WindowFlags = flags;
    context->Window = context->MakeId(title);
    context->IdStack.Add(context->Window);
    bool fresh = false;
    DebugUIContext::WindowState &state = context->Windows.FindOrAdd(context->Window);
    if (state.Height == 0)
    {
        state.Position = position;
        fresh = true;
    }
    context->WindowWidth = width;
    context->Indent = 0;
    context->ClipRect = {-1e6f, -1e6f, 2e6f, 2e6f};
    const bool titled = (flags & DebugWindowFlags::NoTitleBar) == DebugWindowFlags::None;
    if (titled && context->TakesInput())
    {
        const UIRect bar(state.Position.X, state.Position.Y, width, TitleHeight);
        bool hovered = false;
        bool held = false;
        (void)context->Behavior(context->MakeId("##title"), bar, hovered, held);
        if (held)
        {
            state.Position += context->MouseDelta;
        }
    }
    context->WindowPosition = state.Position;
    // A window that takes input shows at most what fits below its top on the screen; the rest scrolls.
    const float32 maxHeight = Max(context->ScreenSize.Y - state.Position.Y - DebugUIStyle::Padding, 3 * TitleHeight);
    float32 height = fresh ? TitleHeight + DebugUIStyle::Padding * 2 : state.Height;
    if (context->TakesInput())
    {
        height = Min(height, maxHeight);
    }
    UIPainter &painter = *context->Painter;
    const UIRect frame(state.Position.X, state.Position.Y, width, height);
    if (context->TakesInput() && Contains(frame, context->Mouse))
    {
        context->MouseOverWindow = true;
        state.Scroll -= context->WheelY * ScrollStep;
    }
    state.Scroll = Math::Clamp(state.Scroll, 0.0f, Max(state.Height - height, 0.0f));
    painter.FillRoundedRect(frame, UICornerRadii(DebugUIStyle::CornerRadius), DebugUIStyle::Panel);
    float32 bodyTop = state.Position.Y;
    if (titled)
    {
        painter.FillRoundedRect({frame.X, frame.Y, width, TitleHeight},
                                UICornerRadii(DebugUIStyle::CornerRadius, DebugUIStyle::CornerRadius, 0, 0),
                                DebugUIStyle::Frame);
        context->DrawText(VisibleLabel(title), {frame.X + DebugUIStyle::Padding, frame.Y + 13}, DebugUIStyle::Text);
        if (open != nullptr)
        {
            const UIRect close(frame.X + width - TitleHeight, frame.Y, TitleHeight, TitleHeight);
            bool hovered = false;
            bool held = false;
            if (context->Behavior(context->MakeId("##close"), close, hovered, held))
            {
                *open = false;
            }
            const Vector2 center = close.GetCenter();
            UIStrokeStyle cross;
            cross.Width = 1.5f;
            const LinearColor color = hovered ? DebugUIStyle::Text : DebugUIStyle::Dim;
            painter.DrawLine(center - Vector2(4, 4), center + Vector2(4, 4), cross, color);
            painter.DrawLine(center + Vector2(-4, 4), center + Vector2(4, -4), cross, color);
        }
        bodyTop = frame.Y + TitleHeight;
    }
    context->CursorY = bodyTop + DebugUIStyle::Padding * 0.5f - state.Scroll;
    context->ClipRect = {frame.X, bodyTop, width, Max(frame.Y + height - bodyTop, 0.0f)};
    painter.PushClip(context->ClipRect);
    return open == nullptr || *open;
}

void EndWindow()
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return;
    }
    DebugUIContext::WindowState &state = context->Windows.FindOrAdd(context->Window);
    state.Height = context->CursorY + state.Scroll + DebugUIStyle::Padding * 0.5f - state.Position.Y;
    const UIRect body = context->ClipRect;
    const float32 overflow = state.Height - (body.Y - state.Position.Y) - body.Height;
    if (context->TakesInput() && overflow > 0.5f && body.Height > 0)
    {
        // A scroll bar along the right edge; its thumb drags the content.
        const UIRect track(body.X + body.Width - ScrollBarWidth - 2, body.Y, ScrollBarWidth + 2, body.Height);
        const float32 content = body.Height + overflow;
        const float32 thumbHeight = Max(body.Height * body.Height / content, 16.0f);
        const float32 travel = Max(body.Height - thumbHeight, 1.0f);
        bool hovered = false;
        bool held = false;
        (void)context->Behavior(context->MakeId("##scrollbar"), track, hovered, held);
        if (held)
        {
            const float32 fraction = Math::Saturate((context->Mouse.Y - body.Y - thumbHeight * 0.5f) / travel);
            state.Scroll = fraction * overflow;
        }
        state.Scroll = Math::Clamp(state.Scroll, 0.0f, overflow);
        const float32 thumbY = body.Y + travel * state.Scroll / overflow;
        context->Painter->FillRoundedRect({track.X + 1, thumbY, ScrollBarWidth, thumbHeight},
                                          UICornerRadii(ScrollBarWidth * 0.5f),
                                          held      ? DebugUIStyle::FrameActive
                                          : hovered ? DebugUIStyle::FrameHovered
                                                    : DebugUIStyle::Grid);
    }
    context->Painter->PopClip();
    context->IdStack.Pop();
    context->InWindow = false;
    context->InTable = false;
}

void Header(StringView text)
{
    if (DebugUIContext *context = Window())
    {
        const UIRect row = context->NextRow();
        context->DrawText(text, {row.X, row.Y + Baseline}, DebugUIStyle::Dim);
    }
}

void Text(StringView text, const LinearColor &color)
{
    if (DebugUIContext *context = Window())
    {
        const UIRect row = context->NextRow();
        context->DrawText(text, {row.X, row.Y + Baseline}, color);
    }
}

void Value(StringView label, StringView value, const LinearColor &color)
{
    if (DebugUIContext *context = Window())
    {
        const UIRect row = context->NextRow();
        context->DrawText(label, {row.X, row.Y + Baseline}, DebugUIStyle::Text);
        const float32 width = context->MeasureText(value);
        context->DrawText(value, {row.X + row.Width - width, row.Y + Baseline}, color);
    }
}

void Separator()
{
    if (DebugUIContext *context = Window())
    {
        const UIRect row = context->NextRow(8);
        context->Painter->FillRect({row.X, row.Y + 4, row.Width, 1}, DebugUIStyle::Grid);
    }
}

void Spacing()
{
    if (DebugUIContext *context = Window())
    {
        (void)context->NextRow(8);
    }
}

bool Button(StringView label)
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return false;
    }
    const StringView visible = VisibleLabel(label);
    const UIRect row = context->NextRow(DebugUIStyle::RowHeight + 4);
    const UIRect button(row.X, row.Y + 1, context->MeasureText(visible) + 16, DebugUIStyle::RowHeight + 2);
    bool hovered = false;
    bool held = false;
    const bool clicked = context->Behavior(context->MakeId(label), button, hovered, held);
    context->Painter->FillRoundedRect(button, UICornerRadii(3),
                                      held      ? DebugUIStyle::FrameActive
                                      : hovered ? DebugUIStyle::FrameHovered
                                                : DebugUIStyle::Frame);
    context->DrawText(visible, {button.X + 8, button.Y + 13}, DebugUIStyle::Text);
    return clicked;
}

bool Checkbox(StringView label, bool &value)
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return false;
    }
    const UIRect row = context->NextRow();
    const UIRect box(row.X, row.Y + 3, 10, 10);
    bool hovered = false;
    bool held = false;
    const bool clicked = context->Behavior(context->MakeId(label), row, hovered, held);
    if (clicked)
    {
        value = !value;
    }
    context->Painter->FillRect(box, hovered ? DebugUIStyle::FrameHovered : DebugUIStyle::Frame);
    if (value)
    {
        context->Painter->FillRect(box.Inflate(-2), DebugUIStyle::Accent);
    }
    context->DrawText(VisibleLabel(label), {row.X + 16, row.Y + Baseline}, DebugUIStyle::Text);
    return clicked;
}

namespace
{
/// The track of a slider at the right of a row, the label at the left; *fraction* moves with a drag.
bool SliderTrack(DebugUIContext &context, StringView label, float32 &fraction, StringView text)
{
    const UIRect row = context.NextRow();
    const float32 labelWidth = row.Width * 0.45f;
    const UIRect track(row.X + labelWidth, row.Y + 1, row.Width - labelWidth, DebugUIStyle::RowHeight - 2);
    bool hovered = false;
    bool held = false;
    (void)context.Behavior(context.MakeId(label), track, hovered, held);
    bool changed = false;
    if (held)
    {
        const float32 next = Math::Saturate((context.Mouse.X - track.X) / Max(track.Width, 1.0f));
        changed = next != fraction;
        fraction = next;
    }
    context.DrawText(VisibleLabel(label), {row.X, row.Y + Baseline}, DebugUIStyle::Text);
    context.Painter->FillRect(track, hovered || held ? DebugUIStyle::FrameHovered : DebugUIStyle::Frame);
    context.Painter->FillRect(
        {track.X, track.Y, track.Width * fraction, track.Height},
        LinearColor(DebugUIStyle::Accent.R, DebugUIStyle::Accent.G, DebugUIStyle::Accent.B, 0.6f));
    const float32 width = context.MeasureText(text);
    context.DrawText(text, {track.X + (track.Width - width) * 0.5f, row.Y + Baseline}, DebugUIStyle::Text);
    return changed;
}
} // namespace

bool Slider(StringView label, float32 &value, float32 min, float32 max)
{
    DebugUIContext *context = Window();
    if (context == nullptr || max <= min)
    {
        return false;
    }
    float32 fraction = Math::Saturate((value - min) / (max - min));
    if (!SliderTrack(*context, label, fraction, Format("{0:F3}", value).AsView()))
    {
        return false;
    }
    value = min + (max - min) * fraction;
    return true;
}

bool Slider(StringView label, int32 &value, int32 min, int32 max)
{
    DebugUIContext *context = Window();
    if (context == nullptr || max <= min)
    {
        return false;
    }
    float32 fraction = Math::Saturate(static_cast<float32>(value - min) / static_cast<float32>(max - min));
    if (!SliderTrack(*context, label, fraction, Format("{0}", value).AsView()))
    {
        return false;
    }
    const int32 next = min + static_cast<int32>(Math::Floor(fraction * static_cast<float32>(max - min) + 0.5f));
    const bool changed = next != value;
    value = next;
    return changed;
}

bool InputText(StringView label, String &text)
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return false;
    }
    const uint64 id = context->MakeId(label);
    const UIRect row = context->NextRow();
    const StringView visible = VisibleLabel(label);
    const float32 labelWidth = visible.IsEmpty() ? 0 : row.Width * 0.45f;
    const UIRect field(row.X + labelWidth, row.Y + 1, row.Width - labelWidth, DebugUIStyle::RowHeight - 2);
    bool hovered = false;
    bool held = false;
    if (context->Behavior(id, field, hovered, held) || (held && context->MousePressed))
    {
        context->Focused = id;
    }
    bool committed = false;
    const bool focused = context->Focused == id;
    if (focused)
    {
        for (const char32_t character : context->Characters)
        {
            char bytes[4];
            const usize length = Utf8::Encode(character, bytes);
            text.Append(StringView(bytes, length));
        }
        for (const KeyCode key : context->KeysPressed)
        {
            if (key == KeyCode::Backspace && !text.IsEmpty())
            {
                usize cut = text.Length() - 1;
                while (cut > 0 && (static_cast<uint8>(text[cut]) & 0xC0) == 0x80)
                {
                    --cut;
                }
                text = String(text.AsView().Left(cut));
            }
            else if (key == KeyCode::Enter || key == KeyCode::NumpadEnter)
            {
                committed = true;
            }
            else if (key == KeyCode::Escape)
            {
                context->Focused = 0;
            }
        }
    }
    if (!visible.IsEmpty())
    {
        context->DrawText(visible, {row.X, row.Y + Baseline}, DebugUIStyle::Text);
    }
    context->Painter->FillRect(field, focused   ? DebugUIStyle::FrameActive
                                      : hovered ? DebugUIStyle::FrameHovered
                                                : DebugUIStyle::Frame);
    context->Painter->PushClip(field);
    context->DrawText(text, {field.X + 4, row.Y + Baseline}, DebugUIStyle::Text);
    if (focused)
    {
        const float32 caret = field.X + 4 + context->MeasureText(text);
        context->Painter->FillRect({caret + 1, field.Y + 2, 1, field.Height - 4}, DebugUIStyle::Text);
    }
    context->Painter->PopClip();
    return committed;
}

void Plot(Span<const float32> values, float32 min, float32 max, float32 height, const LinearColor &color)
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return;
    }
    const UIRect row = context->NextRow(height + 6);
    const UIRect area(row.X, row.Y + 2, row.Width, height);
    UIPainter &painter = *context->Painter;
    painter.StrokeRoundedRect(area, {}, 1, DebugUIStyle::Grid);
    for (uint32 line = 1; line < 6; ++line)
    {
        painter.FillRect({area.X + area.Width * static_cast<float32>(line) / 6, area.Y, 1, area.Height},
                         DebugUIStyle::Grid);
    }
    if (values.Count() < 2 || max <= min)
    {
        return;
    }
    UIPath path;
    for (usize index = 0; index < values.Count(); ++index)
    {
        const float32 x = area.X + area.Width * static_cast<float32>(index) / static_cast<float32>(values.Count() - 1);
        const float32 t = Math::Saturate((values[index] - min) / (max - min));
        const Vector2 point(x, area.Y + area.Height - 1 - t * (area.Height - 2));
        if (index == 0)
        {
            path.MoveTo(point);
        }
        else
        {
            path.LineTo(point);
        }
    }
    UIStrokeStyle style;
    style.Width = 1.5f;
    style.Join = UILineJoin::Round;
    painter.PushClip(area);
    painter.StrokePath(path, style, color);
    painter.PopClip();
}

void BeginTable(uint32 columns, Span<const float32> weights)
{
    DebugUIContext *context = Window();
    if (context == nullptr || columns == 0)
    {
        return;
    }
    DebugUIContext::TableState &table = context->Table;
    table.Columns = Min(columns, 16u);
    float32 total = 0;
    for (uint32 index = 0; index < table.Columns; ++index)
    {
        table.Weights[index] = index < weights.Count() ? weights[index] : 1;
        total += table.Weights[index];
    }
    for (uint32 index = 0; index < table.Columns; ++index)
    {
        table.Weights[index] /= Max(total, 1e-6f);
    }
    table.Cell = 0;
    table.RowTop = context->CursorY;
    context->InTable = true;
}

namespace
{
UIRect NextCell(DebugUIContext &context)
{
    DebugUIContext::TableState &table = context.Table;
    const uint32 column = table.Cell % table.Columns;
    if (column == 0)
    {
        table.RowTop = context.CursorY;
        context.CursorY += DebugUIStyle::RowHeight;
    }
    float32 left = context.ContentLeft();
    for (uint32 index = 0; index < column; ++index)
    {
        left += context.ContentWidth() * table.Weights[index];
    }
    ++table.Cell;
    return {left, table.RowTop, context.ContentWidth() * table.Weights[column], DebugUIStyle::RowHeight};
}
} // namespace

void TableCell(StringView text, const LinearColor &color)
{
    DebugUIContext *context = Window();
    if (context == nullptr || !context->InTable)
    {
        return;
    }
    const UIRect cell = NextCell(*context);
    context->Painter->PushClip(cell);
    context->DrawText(text, {cell.X, cell.Y + Baseline}, color);
    context->Painter->PopClip();
}

bool TableButton(StringView text)
{
    DebugUIContext *context = Window();
    if (context == nullptr || !context->InTable)
    {
        return false;
    }
    const UIRect cell = NextCell(*context);
    bool hovered = false;
    bool held = false;
    const bool clicked = context->Behavior(context->MakeId(text), cell, hovered, held);
    if (hovered || held)
    {
        context->Painter->FillRect(cell, held ? DebugUIStyle::FrameActive : DebugUIStyle::FrameHovered);
    }
    context->Painter->PushClip(cell);
    context->DrawText(VisibleLabel(text), {cell.X, cell.Y + Baseline}, DebugUIStyle::Text);
    context->Painter->PopClip();
    return clicked;
}

void EndTable()
{
    if (DebugUIContext *context = Window())
    {
        context->InTable = false;
    }
}

bool TreeNode(StringView label, bool defaultOpen)
{
    DebugUIContext *context = Window();
    if (context == nullptr)
    {
        return false;
    }
    const uint64 id = context->MakeId(label);
    bool *open = context->OpenNodes.Find(id);
    if (open == nullptr)
    {
        open = &context->OpenNodes.FindOrAdd(id);
        *open = defaultOpen;
    }
    const UIRect row = context->NextRow();
    bool hovered = false;
    bool held = false;
    if (context->Behavior(id, row, hovered, held))
    {
        *open = !*open;
    }
    UIPath arrow;
    const Vector2 center(row.X + 4, row.Y + 8);
    if (*open)
    {
        arrow.AddPolyline(
            Span<const Vector2>({center + Vector2(-4, -2), center + Vector2(4, -2), center + Vector2(0, 3)}), true);
    }
    else
    {
        arrow.AddPolyline(
            Span<const Vector2>({center + Vector2(-2, -4), center + Vector2(3, 0), center + Vector2(-2, 4)}), true);
    }
    context->Painter->FillPath(arrow, hovered ? DebugUIStyle::Text : DebugUIStyle::Dim);
    context->DrawText(VisibleLabel(label), {row.X + 12, row.Y + Baseline}, DebugUIStyle::Text);
    if (*open)
    {
        context->IdStack.Add(id);
        context->Indent += TreeIndent;
    }
    return *open;
}

void TreePop()
{
    if (DebugUIContext *context = Window())
    {
        context->Indent = Max(context->Indent - TreeIndent, 0.0f);
        if (context->IdStack.Count() > 2)
        {
            context->IdStack.Pop();
        }
    }
}

void Banner(StringView text, const LinearColor &color)
{
    DebugUIContext *context = CurrentContext;
    if (context == nullptr)
    {
        return;
    }
    const float32 left = 300;
    const float32 width = Min(context->ScreenSize.X - left - 8, context->MeasureText(text) + 32);
    const UIRect strip(left, context->BannerY, Max(width, 40.0f), DebugUIStyle::RowHeight + 2);
    context->BannerY += strip.Height + 2;
    UIPainter &painter = *context->Painter;
    painter.FillRect(strip, DebugUIStyle::Panel);
    UIPath sign;
    const Vector2 corner(strip.X + 6, strip.Y + strip.Height - 3);
    sign.AddPolyline(Span<const Vector2>({corner, corner + Vector2(12, 0), corner + Vector2(6, -11)}), true);
    painter.FillPath(sign, color);
    painter.FillRect({corner.X + 5.4f, corner.Y - 8, 1.2f, 4.5f}, LinearColor(0, 0, 0, 0.85f));
    painter.FillRect({corner.X + 5.4f, corner.Y - 2.5f, 1.2f, 1.2f}, LinearColor(0, 0, 0, 0.85f));
    painter.PushClip(strip);
    context->DrawText(text, {strip.X + 24, strip.Y + 13}, DebugUIStyle::Text);
    painter.PopClip();
}

void PushId(StringView id)
{
    if (DebugUIContext *context = CurrentContext)
    {
        context->IdStack.Add(context->MakeId(id));
    }
}

void PushId(uint64 id)
{
    if (DebugUIContext *context = CurrentContext)
    {
        context->IdStack.Add(HashCombine(context->IdStack.Last(), id));
    }
}

void PopId()
{
    if (DebugUIContext *context = CurrentContext; context != nullptr && context->IdStack.Count() > 1)
    {
        context->IdStack.Pop();
    }
}

float32 GetContentWidth()
{
    DebugUIContext *context = Window();
    return context != nullptr ? context->ContentWidth() : 0;
}

bool WantsMouse()
{
    DebugUIContext *context = CurrentContext;
    return context != nullptr && (context->MouseOverWindowLastFrame || context->Active != 0);
}

bool WantsKeyboard()
{
    DebugUIContext *context = CurrentContext;
    return context != nullptr && context->Focused != 0;
}
} // namespace DebugUI
} // namespace Marlin
