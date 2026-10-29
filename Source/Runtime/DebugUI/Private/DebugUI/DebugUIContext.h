#pragma once

#include "Application/PlatformApplication.h"
#include "Application/PlatformWindow.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "DebugUI/DebugUI.h"
#include "Math/Vector.h"
#include "String/String.h"
#include "UIPainter/UIPainter.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
/// The state behind the DebugUI functions for one window's frame: input, ids, window and widget state. The
/// subsystem owns one and makes it current for the frame.
struct DEBUGUI_API DebugUIContext
{
    struct WindowState
    {
        Vector2 Position;
        /// The height of all the content; the window shows less of it when it would leave the screen.
        float32 Height = 0;
        /// How far the content is scrolled up, in painter units.
        float32 Scroll = 0;
    };

    struct TableState
    {
        uint32 Columns = 0;
        float32 Weights[16] = {};
        uint32 Cell = 0;
        float32 RowTop = 0;
    };

    UIPainter *Painter = nullptr;
    const UITextSubsystem *Text = nullptr;
    Span<const UIFontFace> Faces;
    Vector2 ScreenSize;

    Vector2 Mouse{-1e6f, -1e6f};
    Vector2 MouseDelta;
    bool MouseDown = false;
    bool MousePressed = false;
    bool MouseReleased = false;
    /// The wheel notches this frame; positive scrolls up.
    float32 WheelY = 0;
    List<KeyCode> KeysPressed;
    List<char32_t> Characters;

    List<uint64> IdStack;
    uint64 Hovered = 0;
    uint64 Active = 0;
    uint64 Focused = 0;
    bool MouseOverWindow = false;
    bool MouseOverWindowLastFrame = false;

    HashMap<uint64, WindowState> Windows;
    HashMap<uint64, bool> OpenNodes;
    HashMap<uint64, String> EditBuffers;
    uint64 Window = 0;
    DebugWindowFlags WindowFlags = DebugWindowFlags::None;
    Vector2 WindowPosition;
    float32 WindowWidth = 0;
    float32 CursorY = 0;
    /// The part of the window below the title bar; widgets outside it are scrolled away and take no input.
    UIRect ClipRect;
    float32 Indent = 0;
    bool InWindow = false;
    TableState Table;
    bool InTable = false;
    float32 BannerY = 0;

    /// Text shaped in this frame or the last, by its hash: DebugUI draws the same labels every frame. Unused while a
    /// face is still loading, since shaping then leaves glyphs out.
    struct ShapedEntry
    {
        String Text;
        UIShapedText Shaped;
        uint64 Frame = 0;
    };
    HashMap<uint64, ShapedEntry> ShapedTexts;
    uint64 FrameNumber = 0;
    bool FacesReady = false;
    UIShapedText UncachedShape;

    void BeginFrame(UIPainter &painter, const UITextSubsystem &text, Span<const UIFontFace> faces,
                    Span<const ApplicationEvent> events, WindowHandle window);
    void EndFrame();

    [[nodiscard]] uint64 MakeId(StringView label) const;
    [[nodiscard]] float32 MeasureText(StringView text);
    /// *text* shaped in the DebugUI font, valid until the next call.
    [[nodiscard]] const UIShapedText &GetShaped(StringView text);
    void DrawText(StringView text, Vector2 baseline, const LinearColor &color);
    /// Hover and press handling of an interactive rectangle; true on a click (press and release inside).
    bool Behavior(uint64 id, const UIRect &rect, bool &hovered, bool &held);
    [[nodiscard]] bool TakesInput() const;
    /// Moves the cursor past a row of *height*; returns its rectangle.
    UIRect NextRow(float32 height = DebugUIStyle::RowHeight);
    [[nodiscard]] float32 ContentLeft() const;
    [[nodiscard]] float32 ContentWidth() const;
};

/// The context the DebugUI functions use; null outside a frame.
DEBUGUI_API DebugUIContext *GetDebugUIContext();
DEBUGUI_API void SetDebugUIContext(DebugUIContext *context);
} // namespace Marlin
