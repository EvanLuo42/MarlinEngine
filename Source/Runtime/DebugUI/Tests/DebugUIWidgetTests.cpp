#include "Testing/Testing.h"

#include "DebugUI/DebugUI.h"
// The widget context is a private header of DebugUI, exported for these tests.
#include "../Private/DebugUI/DebugUIContext.h"

#include "UIPainter/UICommandList.h"

// RECOVERY-DECISION D5: the outline indexes no DebugUI tests; these were added during recovery to check the widgets
// by recording into a painter, since the UI shaders do not load in Game yet.
namespace Marlin::Tests
{
namespace
{
/// Runs one DebugUI frame on *context* with *events*, calling *body* between BeginFrame and EndFrame.
template <typename TBody>
usize RunFrame(DebugUIContext &context, const UITextSubsystem &text, Span<const ApplicationEvent> events, TBody &&body)
{
    UICommandList list;
    UIPainter painter;
    painter.Begin(list, 800, 600, 1);
    context.EndFrame();
    context.BeginFrame(painter, text, {}, events, WindowHandle());
    SetDebugUIContext(&context);
    body();
    SetDebugUIContext(nullptr);
    painter.End();
    return list.GetCommandCount();
}

ApplicationEvent MouseEvent(ApplicationEventType type, Vector2 point)
{
    ApplicationEvent event;
    event.Type = type;
    event.X = static_cast<int32>(point.X);
    event.Y = static_cast<int32>(point.Y);
    return event;
}
} // namespace

MARLIN_TEST(DebugUIWidgets, NoContextDoesNothing)
{
    bool value = false;
    MARLIN_EXPECT(!DebugUI::BeginWindow("Test", {0, 0}, 100));
    DebugUI::EndWindow();
    MARLIN_EXPECT(!DebugUI::Button("Go") && !DebugUI::Checkbox("Flag", value) && !value);
    MARLIN_EXPECT(!DebugUI::WantsMouse() && !DebugUI::WantsKeyboard() && DebugUI::GetContentWidth() == 0);
}

MARLIN_TEST(DebugUIWidgets, ButtonAndCheckboxClicks)
{
    UITextSubsystem text;
    DebugUIContext context;
    Vector2 button;
    Vector2 checkbox;
    bool value = false;
    bool clicked = false;
    auto window = [&] {
        MARLIN_REQUIRE(DebugUI::BeginWindow("Test", {20, 20}, 240));
        DebugUIContext *current = GetDebugUIContext();
        button = {current->ContentLeft() + 4, current->CursorY + 4};
        clicked = DebugUI::Button("Go");
        checkbox = {current->ContentLeft() + 4, current->CursorY + 4};
        (void)DebugUI::Checkbox("Flag", value);
        DebugUI::Value("Frame", "16.6 ms");
        DebugUI::EndWindow();
    };

    const usize commands = RunFrame(context, text, {}, window);
    // The first frame has no content height yet: the panel and the title bar.
    MARLIN_EXPECT(commands >= 2);
    MARLIN_EXPECT(!clicked && !value);

    // A click is a press and a release inside the widget.
    const ApplicationEvent press[] = {MouseEvent(ApplicationEventType::MouseButtonDown, button)};
    (void)RunFrame(context, text, press, window);
    MARLIN_EXPECT(!clicked);
    const ApplicationEvent release[] = {MouseEvent(ApplicationEventType::MouseButtonUp, button)};
    (void)RunFrame(context, text, release, window);
    MARLIN_EXPECT(clicked);
    (void)RunFrame(context, text, {}, window);
    MARLIN_EXPECT(!clicked && DebugUI::WantsMouse() == false);

    const ApplicationEvent toggle[] = {MouseEvent(ApplicationEventType::MouseButtonDown, checkbox),
                                       MouseEvent(ApplicationEventType::MouseButtonUp, checkbox)};
    (void)RunFrame(context, text, toggle, window);
    MARLIN_EXPECT(value && !clicked);

    // The mouse over the window belongs to DebugUI from the next frame on.
    SetDebugUIContext(&context);
    MARLIN_EXPECT(context.MouseOverWindow);
    SetDebugUIContext(nullptr);
}

MARLIN_TEST(DebugUIWidgets, NoInputWindowIgnoresMouse)
{
    UITextSubsystem text;
    DebugUIContext context;
    Vector2 button;
    bool clicked = false;
    auto window = [&] {
        MARLIN_REQUIRE(
            DebugUI::BeginWindow("##Overlay", {0, 0}, 200, DebugWindowFlags::NoTitleBar | DebugWindowFlags::NoInput));
        button = {GetDebugUIContext()->ContentLeft() + 4, GetDebugUIContext()->CursorY + 4};
        clicked = DebugUI::Button("Go");
        DebugUI::EndWindow();
    };
    (void)RunFrame(context, text, {}, window);
    const ApplicationEvent click[] = {MouseEvent(ApplicationEventType::MouseButtonDown, button),
                                      MouseEvent(ApplicationEventType::MouseButtonUp, button)};
    (void)RunFrame(context, text, click, window);
    MARLIN_EXPECT(!clicked && !context.MouseOverWindow);
}

MARLIN_TEST(DebugUIWidgets, TallWindowScrollsWithTheWheel)
{
    UITextSubsystem text;
    DebugUIContext context;
    float32 firstY = 0;
    float32 contentEnd = 0;
    bool firstClicked = false;
    auto window = [&] {
        MARLIN_REQUIRE(DebugUI::BeginWindow("Tall", {20, 20}, 240));
        firstY = GetDebugUIContext()->CursorY;
        firstClicked = DebugUI::Button("First");
        for (uint32 row = 0; row < 80; ++row)
        {
            DebugUI::Text("row");
        }
        contentEnd = GetDebugUIContext()->CursorY;
        DebugUI::EndWindow();
    };
    (void)RunFrame(context, text, {}, window);
    (void)RunFrame(context, text, {}, window);
    // The content is taller than the 600-unit screen, which the window stops at.
    MARLIN_EXPECT(contentEnd > 600);
    const float32 top = firstY;

    ApplicationEvent wheel = MouseEvent(ApplicationEventType::MouseMove, {100, 200});
    (void)RunFrame(context, text, Span<const ApplicationEvent>(&wheel, 1), window);
    wheel.Type = ApplicationEventType::MouseWheel;
    wheel.WheelY = -2;
    (void)RunFrame(context, text, Span<const ApplicationEvent>(&wheel, 1), window);
    MARLIN_EXPECT(firstY < top);

    // Scrolled far enough, the first button has left the window, so a click at the mouse does nothing.
    wheel.WheelY = -100;
    (void)RunFrame(context, text, Span<const ApplicationEvent>(&wheel, 1), window);
    const ApplicationEvent click[] = {MouseEvent(ApplicationEventType::MouseButtonDown, {100, 200}),
                                      MouseEvent(ApplicationEventType::MouseButtonUp, {100, 200})};
    (void)RunFrame(context, text, click, window);
    MARLIN_EXPECT(!firstClicked);

    // Scrolling back up returns the content to its place.
    wheel.WheelY = 1000;
    (void)RunFrame(context, text, Span<const ApplicationEvent>(&wheel, 1), window);
    MARLIN_EXPECT(firstY == top);
}

MARLIN_TEST(DebugUIWidgets, InputTextTypesAndCommits)
{
    UITextSubsystem text;
    DebugUIContext context;
    String command;
    Vector2 field;
    bool committed = false;
    auto window = [&] {
        MARLIN_REQUIRE(DebugUI::BeginWindow("Console", {300, 80}, 520));
        field = {GetDebugUIContext()->ContentLeft() + 4, GetDebugUIContext()->CursorY + 4};
        committed = DebugUI::InputText("##Command", command);
        DebugUI::EndWindow();
    };
    (void)RunFrame(context, text, {}, window);
    const ApplicationEvent click[] = {MouseEvent(ApplicationEventType::MouseButtonDown, field),
                                      MouseEvent(ApplicationEventType::MouseButtonUp, field)};
    (void)RunFrame(context, text, click, window);
    MARLIN_EXPECT(DebugUI::WantsKeyboard() == false && context.Focused != 0);

    ApplicationEvent typed[4];
    const char characters[] = {'h', 'e', 'y'};
    for (usize index = 0; index < 3; ++index)
    {
        typed[index].Type = ApplicationEventType::TextInput;
        typed[index].Character = static_cast<uint32>(characters[index]);
    }
    typed[3].Type = ApplicationEventType::KeyDown;
    typed[3].Key = KeyCode::Backspace;
    (void)RunFrame(context, text, typed, window);
    MARLIN_EXPECT(command == "he" && !committed);

    ApplicationEvent enter;
    enter.Type = ApplicationEventType::KeyDown;
    enter.Key = KeyCode::Enter;
    (void)RunFrame(context, text, Span<const ApplicationEvent>(&enter, 1), window);
    MARLIN_EXPECT(committed && command == "he");
}
} // namespace Marlin::Tests
