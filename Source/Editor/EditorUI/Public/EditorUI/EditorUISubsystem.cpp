#include "EditorUI/EditorUISubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "Application/PlatformWindow.h"
#include "Diagnostics/Log.h"
#include "EditorUI/EditorTheme.h"
#include "EditorUI/UIAnimation.h"
#include "Engine/StartupProgress.h"
#include "Math/MathUtility.h"
#include "Renderer/RenderSurface.h"
#include "UIPainter/UIPainter.h"
#include "UIPainterRenderer/UIPainterSubsystem.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
float32 EditorUISubsystem::Scale = 1;

EditorUISubsystem::EditorUISubsystem() = default;
EditorUISubsystem::~EditorUISubsystem() = default;

void EditorUISubsystem::Initialize(SubsystemContext &context)
{
    _application = context.Collection->Get<ApplicationSubsystem>();
    _painter = context.Collection->Get<UIPainterSubsystem>();
    _text = context.Collection->Get<UITextSubsystem>();
    _readyCheck = StartupProgress::AddReadyCheck("Editor text", [this] {
        for (const UniquePtr<EditorWindow> &window : _windows)
        {
            if (window->Root->HasPendingText())
            {
                return false;
            }
        }
        return true;
    });
    if (_painter == nullptr || _painter->GetMainCanvas() == nullptr)
    {
        return;
    }
    UniquePtr<EditorWindow> main = MakeUnique<EditorWindow>();
    main->Root = MakeUnique<UIWindowRoot>(_text);
    main->Canvas = _painter->GetMainCanvas();
    if (_application != nullptr)
    {
        main->Root->SetWindowHandle(_application->GetMainWindow());
    }
    _windows.Add(Move(main));
}

void EditorUISubsystem::Deinitialize()
{
    StartupProgress::RemoveReadyCheck(_readyCheck);
    while (_windows.Count() > 1)
    {
        const WindowHandle handle = _windows.Last()->Root->GetWindowHandle();
        Destroy(*_windows.Last());
        _windows.RemoveAt(_windows.Count() - 1);
        PlatformWindow::Destroy(handle);
    }
    _windows.Clear();
}

UIWindowRoot *EditorUISubsystem::OpenWindow(const WindowDesc &desc, Function<void()> onCloseRequested)
{
    if (_painter == nullptr)
    {
        return nullptr;
    }
    WindowDesc custom = desc;
    custom.Flags |= WindowFlags::CustomFrame;
    const WindowHandle handle = PlatformWindow::Create(custom);
    if (!handle.IsValid())
    {
        return nullptr;
    }
    UIWindowCanvas *canvas = _painter->CreateWindowCanvas(handle);
    if (canvas == nullptr)
    {
        PlatformWindow::Destroy(handle);
        return nullptr;
    }
    canvas->ClearColor =
        _painter->GetMainCanvas() != nullptr ? _painter->GetMainCanvas()->ClearColor : LinearColor::Black;
    UniquePtr<EditorWindow> window = MakeUnique<EditorWindow>();
    window->Root = MakeUnique<UIWindowRoot>(_text);
    window->Root->SetWindowHandle(handle);
    window->Canvas = canvas;
    window->Owned = true;
    window->OnCloseRequested = Move(onCloseRequested);
    UIWindowRoot *root = window->Root.Get();
    _windows.Add(Move(window));
    if ((desc.Flags & WindowFlags::Hidden) == WindowFlags::None)
    {
        PlatformWindow::Show(handle);
    }
    return root;
}

void EditorUISubsystem::CloseWindow(UIWindowRoot *window)
{
    for (UniquePtr<EditorWindow> &entry : _windows)
    {
        if (entry->Root.Get() == window && entry->Owned)
        {
            entry->Destroying = true;
            // Gone from the user's view at once; freed at the end of the frame.
            PlatformWindow::Hide(window->GetWindowHandle());
        }
    }
}

UIWindowRoot *EditorUISubsystem::FindWindowAt(int32 x, int32 y, const UIWindowRoot *ignore) const
{
    // The windows opened last are tested first: they were most likely on top.
    for (usize index = _windows.Count(); index-- > 0;)
    {
        const EditorWindow &window = *_windows[index];
        const WindowHandle handle = window.Root->GetWindowHandle();
        if (window.Destroying || window.Root.Get() == ignore || !handle.IsValid() ||
            !PlatformWindow::IsVisible(handle) || PlatformWindow::IsMinimized(handle))
        {
            continue;
        }
        const PlatformRect rect = PlatformWindow::GetClientRect(handle);
        if (x >= rect.X && x < rect.X + rect.Width && y >= rect.Y && y < rect.Y + rect.Height)
        {
            return window.Root.Get();
        }
    }
    return nullptr;
}

float32 EditorUISubsystem::GetPixelsPerUnit(WindowHandle window)
{
    const float32 dpi = window.IsValid() ? PlatformWindow::GetDpiScale(window) : 1.0f;
    return dpi * Math::Clamp(Scale, 0.5f, 4.0f);
}

void EditorUISubsystem::Destroy(EditorWindow &window)
{
    // The tree first: its cleanups may still name the window.
    window.Root.Reset();
    if (window.Owned)
    {
        if (_painter != nullptr && window.Canvas != nullptr)
        {
            _painter->DestroyCanvas(window.Canvas);
        }
        window.Canvas = nullptr;
    }
}

void EditorUISubsystem::Update(const FrameTime &time)
{
    if (_windows.IsEmpty())
    {
        return;
    }
    const float32 scale = Math::Clamp(Scale, 0.5f, 4.0f);
    if (_application != nullptr)
    {
        for (const ApplicationEvent &event : _application->GetEvents())
        {
            // By index: a handler may open a window.
            for (usize index = 0; index < _windows.Count(); ++index)
            {
                EditorWindow &window = *_windows[index];
                if (window.Destroying || window.Root->GetWindowHandle() != event.Window)
                {
                    continue;
                }
                if (event.Type == ApplicationEventType::WindowCloseRequested && window.OnCloseRequested.IsBound())
                {
                    window.OnCloseRequested();
                }
                window.Root->DispatchEvent(event, window.Canvas->GetSurface().GetDpiScale() * scale);
                break;
            }
        }
    }
    UIAnimation::SetTime(time.Elapsed);
    Reactive::Flush();
    for (UniquePtr<EditorWindow> &window : _windows)
    {
        if (window->Destroying || window->Canvas == nullptr || !window->Canvas->GetPainter().IsRecording())
        {
            continue;
        }
        UIPainter &painter = window->Canvas->GetPainter();
        // What shows where the tree does not reach, such as a window grown past the last layout.
        window->Canvas->ClearColor = ThemeColor("Window.Background")();
        window->Root->Layout(painter.GetSize() / scale);
        painter.PushTransform(UITransform::Scale(scale, scale));
        window->Root->Paint(painter);
        painter.PopTransform();
        UpdateHitRegions(*window, painter.GetScale() * scale);
    }
    for (usize index = _windows.Count(); index-- > 1;)
    {
        if (_windows[index]->Destroying)
        {
            const WindowHandle handle = _windows[index]->Root->GetWindowHandle();
            Destroy(*_windows[index]);
            _windows.RemoveAt(index);
            PlatformWindow::Destroy(handle);
        }
    }
}

void EditorUISubsystem::SetScale(float32 scale)
{
    Scale = scale;
    MARLIN_LOG(LogCore, Info, "Editor UI scale {0}", Math::Clamp(Scale, 0.5f, 4.0f));
}

void EditorUISubsystem::UpdateHitRegions(EditorWindow &window, float32 scale)
{
    const WindowHandle handle = window.Root->GetWindowHandle();
    if (!handle.IsValid())
    {
        return;
    }
    window.Root->CollectHitRegions(scale, _scratchRegions);
    bool same = _scratchRegions.Count() == window.HitRegions.Count();
    for (usize index = 0; same && index < window.HitRegions.Count(); ++index)
    {
        const WindowHitRegion &a = _scratchRegions[index];
        const WindowHitRegion &b = window.HitRegions[index];
        same = a.Area == b.Area && a.Rect.X == b.Rect.X && a.Rect.Y == b.Rect.Y && a.Rect.Width == b.Rect.Width &&
               a.Rect.Height == b.Rect.Height;
    }
    if (!same)
    {
        Swap(window.HitRegions, _scratchRegions);
        PlatformWindow::SetHitTestRegions(handle, window.HitRegions.AsSpan());
    }
}
} // namespace Marlin
