#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "EnumFlags.h"
#include "Math/Color.h"
#include "Math/Vector.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// The look of DebugUI: the Metal Performance HUD's dense translucent panels, monospace rows, thin graphs.
struct DebugUIStyle
{
    static constexpr LinearColor Panel{0, 0, 0, 0.85f};
    static constexpr LinearColor Text{0.86f, 0.86f, 0.86f};
    static constexpr LinearColor Dim{0.55f, 0.56f, 0.6f};
    static constexpr LinearColor Good{0.35f, 0.85f, 0.4f};
    static constexpr LinearColor Warning{1.0f, 0.78f, 0.15f};
    static constexpr LinearColor Bad{1.0f, 0.35f, 0.3f};
    static constexpr LinearColor Accent{0.3f, 0.55f, 1.0f};
    static constexpr LinearColor Grid{1, 1, 1, 0.16f};
    static constexpr LinearColor Frame{1, 1, 1, 0.08f};
    static constexpr LinearColor FrameHovered{1, 1, 1, 0.15f};
    static constexpr LinearColor FrameActive{1, 1, 1, 0.24f};
    static constexpr float32 FontSize = 12;
    static constexpr float32 RowHeight = 16;
    static constexpr float32 Padding = 8;
    static constexpr float32 CornerRadius = 6;
};

enum class DebugWindowFlags : uint8
{
    None = 0,
    /// No title bar to read or drag: an overlay panel.
    NoTitleBar = 1 << 0,
    /// Widgets in it ignore the mouse and the window never takes it from the game.
    NoInput = 1 << 1,
};
MARLIN_ENUM_FLAGS(DebugWindowFlags)

/// Immediate-mode debug widgets (Documentation/Architecture/09 §3), drawn with UIPainter over the game's UI on the
/// main window. Call them from the main thread anywhere in the frame between BeginWindow and EndWindow; they draw at
/// once and return what the user did this frame. Widgets are told apart by their label within the window (append
/// "##suffix" to a label to make two alike labels differ without showing it) and by PushId. Without a
/// DebugUISubsystem, or before it starts the frame, the calls do nothing and return false. Not linked in Shipping.
namespace DebugUI
{
/// Starts a window at *position* (painter units, the first time; dragging its title moves it) that is *width*
/// wide and as tall as its content was last frame. *open*, if given, gets a close button. False if the window is
/// closed; EndWindow must still be called.
DEBUGUI_API bool BeginWindow(StringView title, Vector2 position, float32 width,
                             DebugWindowFlags flags = DebugWindowFlags::None, bool *open = nullptr);
DEBUGUI_API void EndWindow();

/// A dim section header.
DEBUGUI_API void Header(StringView text);
DEBUGUI_API void Text(StringView text, const LinearColor &color = DebugUIStyle::Text);
/// A row with *label* on the left and *value* right-aligned.
DEBUGUI_API void Value(StringView label, StringView value, const LinearColor &color = DebugUIStyle::Text);
DEBUGUI_API void Separator();
DEBUGUI_API void Spacing();

/// True on the frame the button is clicked.
DEBUGUI_API bool Button(StringView label);
/// True on the frame *value* changes.
DEBUGUI_API bool Checkbox(StringView label, bool &value);
DEBUGUI_API bool Slider(StringView label, float32 &value, float32 min, float32 max);
DEBUGUI_API bool Slider(StringView label, int32 &value, int32 min, int32 max);
/// A one-line text field; true on the frame Enter commits it.
DEBUGUI_API bool InputText(StringView label, String &text);

/// A line graph of *values* between *min* and *max* across the window, with a faint grid.
DEBUGUI_API void Plot(Span<const float32> values, float32 min, float32 max, float32 height = 40,
                      const LinearColor &color = DebugUIStyle::Accent);

/// Starts a table of *columns*, widths in proportion to *weights* (equal if empty); cells fill rows left to right.
DEBUGUI_API void BeginTable(uint32 columns, Span<const float32> weights = {});
DEBUGUI_API void TableCell(StringView text, const LinearColor &color = DebugUIStyle::Text);
/// True on the frame the cell is clicked, for selectable rows.
DEBUGUI_API bool TableButton(StringView text);
DEBUGUI_API void EndTable();

/// A collapsible node; true while open, and then TreePop must follow its children.
DEBUGUI_API bool TreeNode(StringView label, bool defaultOpen = false);
DEBUGUI_API void TreePop();

/// A one-line warning banner across the top of the screen, right of the overlay, with a warning sign.
DEBUGUI_API void Banner(StringView text, const LinearColor &color = DebugUIStyle::Warning);

DEBUGUI_API void PushId(StringView id);
DEBUGUI_API void PushId(uint64 id);
DEBUGUI_API void PopId();

/// The width the content of the current window has, in painter units.
[[nodiscard]] DEBUGUI_API float32 GetContentWidth();
/// True if the mouse is over a DebugUI window that takes input, or a widget holds it: the game should not use it.
[[nodiscard]] DEBUGUI_API bool WantsMouse();
/// True while a text field has the keyboard.
[[nodiscard]] DEBUGUI_API bool WantsKeyboard();
} // namespace DebugUI
} // namespace Marlin
