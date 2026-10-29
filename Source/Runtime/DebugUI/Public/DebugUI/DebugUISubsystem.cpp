#include "DebugUI/DebugUISubsystem.h"

#include "Application/ApplicationSubsystem.h"
#include "Concurrency/SpinLock.h"
#include "Console/Console.h"
#include "DebugUI/DebugUI.h"
#include "DebugUI/DebugUIContext.h"
#include "Diagnostics/Log.h"
#include "HAL/PlatformTime.h"
#include "Hash/Hash.h"
#include "Misc/CommandLine.h"
#include "Reflection/TypeOf.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "UIPainterRenderer/UIPainterSubsystem.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
bool ContainsIgnoreCase(StringView text, StringView part)
{
    for (usize start = 0; start + part.Length() <= text.Length(); ++start)
    {
        if (text.Substring(start, part.Length()).EqualsIgnoreCase(part))
        {
            return true;
        }
    }
    return false;
}

/// The part of a variable's name before its first dot, or the whole name.
StringView GroupOf(StringView name)
{
    StringView group;
    StringView rest;
    return name.SplitFirst('.', group, rest) ? group : name;
}

/// The level a click on a category's level moves to: up through Error, then Off, then back to Trace.
LogLevel NextLevel(LogLevel level)
{
    switch (level)
    {
    case LogLevel::Trace:
        return LogLevel::Debug;
    case LogLevel::Debug:
        return LogLevel::Info;
    case LogLevel::Info:
        return LogLevel::Warning;
    case LogLevel::Warning:
        return LogLevel::Error;
    case LogLevel::Error:
        return LogLevel::Off;
    default:
        return LogLevel::Trace;
    }
}
} // namespace

bool DebugUISubsystem::Overlay = false;
bool DebugUISubsystem::ConsoleWindow = false;
float32 DebugUISubsystem::BannerSeconds = 8;

class DebugUISubsystem::BannerSink final : public ILogSink
{
public:
    struct Banner
    {
        String Text;
        LogLevel Level = LogLevel::Warning;
        uint64 Ticks = 0;
    };

    static constexpr usize MaxBanners = 6;

    void Write(const LogRecord &record) override
    {
        if (record.Level != LogLevel::Warning && record.Level != LogLevel::Error)
        {
            return;
        }
        Banner banner;
        banner.Level = record.Level;
        banner.Ticks = PlatformTime::Ticks();
        banner.Text =
            Format("{0}: {1}", record.Category != nullptr ? StringView(record.Category->GetName()) : StringView("Log"),
                   record.Message);
        ScopeLock lock(_lock);
        if (_banners.Count() == MaxBanners)
        {
            _banners.RemoveAt(0);
        }
        _banners.Add(Move(banner));
    }

    /// The banners younger than *seconds*, oldest first.
    void Collect(float64 seconds, List<Banner> &banners)
    {
        const uint64 now = PlatformTime::Ticks();
        const auto window = static_cast<uint64>(seconds * static_cast<float64>(PlatformTime::TicksPerSecond()));
        ScopeLock lock(_lock);
        for (const Banner &banner : _banners)
        {
            if (now - banner.Ticks <= window)
            {
                banners.Add(banner);
            }
        }
    }

private:
    SpinLock _lock;
    List<Banner> _banners;
};

DebugUISubsystem::DebugUISubsystem() = default;
DebugUISubsystem::~DebugUISubsystem() = default;

void DebugUISubsystem::Initialize(SubsystemContext &context)
{
    _application = context.Collection->Get<ApplicationSubsystem>();
    _painter = context.Collection->Get<UIPainterSubsystem>();
    _text = context.Collection->Get<UITextSubsystem>();
    _context = MakeUnique<DebugUIContext>();
    _banners = MakeUnique<BannerSink>();
    Log::AddSink(_banners.Get());
    _sections.Build();
    if (CommandLine::HasSwitch("DebugOverlay"))
    {
        if (ConsoleVariable *variable = Console::FindVariable("ui.DebugOverlay"))
        {
            (void)variable->Set("true", ConsoleSetBy::CommandLine);
        }
    }
}

void DebugUISubsystem::Deinitialize()
{
    if (GetDebugUIContext() == _context.Get())
    {
        SetDebugUIContext(nullptr);
    }
    Log::RemoveSink(_banners.Get());
    _banners.Reset();
    _context.Reset();
}

void DebugUISubsystem::BeginFrame(const FrameTime &time)
{
    UIWindowCanvas *canvas = _painter != nullptr ? _painter->GetMainCanvas() : nullptr;
    if (canvas == nullptr || _text == nullptr || _application == nullptr)
    {
        SetDebugUIContext(nullptr);
        return;
    }
    if (_frameCount > 0 || time.Frame > 0)
    {
        _frameHistory[_frameCount % HistoryLength] = static_cast<float32>(time.Delta * 1000.0);
        ++_frameCount;
    }
    _context->EndFrame();
    _context->BeginFrame(canvas->GetOverlayPainter(), *_text, _text->GetMonospaceFaces(), _application->GetEvents(),
                         _application->GetMainWindow());
    SetDebugUIContext(_context.Get());
    HandleHotkeys();
    if (Overlay)
    {
        DrawOverlay(time);
    }
    if (ConsoleWindow)
    {
        DrawConsole();
    }
}

void DebugUISubsystem::HandleHotkeys()
{
    if (DebugUI::WantsKeyboard())
    {
        return;
    }
    for (const KeyCode key : _context->KeysPressed)
    {
        const StringView name = key == KeyCode::F1   ? StringView("ui.DebugOverlay")
                                : key == KeyCode::F2 ? StringView("ui.DebugConsole")
                                                     : StringView();
        if (name.IsEmpty())
        {
            continue;
        }
        if (ConsoleVariable *variable = Console::FindVariable(name))
        {
            const bool value = *static_cast<const bool *>(variable->GetValue());
            (void)variable->Set(value ? "false" : "true", ConsoleSetBy::Console);
        }
    }
}

void DebugUISubsystem::DrawOverlay(const FrameTime &time)
{
    const uint32 count = _frameCount < HistoryLength ? _frameCount : HistoryLength;
    const uint32 first = _frameCount - count;
    for (uint32 index = 0; index < count; ++index)
    {
        _frameOrdered[index] = _frameHistory[(first + index) % HistoryLength];
    }
    const DebugOverlayFrame frame{time, Span<const float32>(_frameOrdered, count)};
    if (DebugUI::BeginWindow("##DebugOverlay", {8, 8}, 280, DebugWindowFlags::NoTitleBar | DebugWindowFlags::NoInput))
    {
        bool first = true;
        for (DebugOverlaySection *section : _sections)
        {
            if (!first)
            {
                DebugUI::Separator();
            }
            first = false;
            DebugUI::PushId(section->GetTitle());
            DebugUI::Header(section->GetTitle());
            section->Draw(frame);
            DebugUI::PopId();
        }
    }
    DebugUI::EndWindow();
    List<BannerSink::Banner> banners;
    _banners->Collect(BannerSeconds, banners);
    for (const BannerSink::Banner &banner : banners)
    {
        DebugUI::Banner(banner.Text, banner.Level == LogLevel::Error ? DebugUIStyle::Bad : DebugUIStyle::Warning);
    }
}

void DebugUISubsystem::DrawLogLevels()
{
    if (!DebugUI::TreeNode("Log Levels", !_filter.IsEmpty()))
    {
        return;
    }
    List<LogCategory *> categories;
    Log::ForEachCategory([&](LogCategory &category) {
        if (_filter.IsEmpty() || ContainsIgnoreCase(StringView(category.GetName()), _filter.AsView()))
        {
            categories.Add(&category);
        }
    });
    categories.Sort([](const LogCategory *a, const LogCategory *b) {
        return StringView(a->GetName()) < StringView(b->GetName());
    });
    constexpr usize MaxRows = 40;
    DebugUI::Text("Click a level to raise it; Off silences the category.", DebugUIStyle::Dim);
    DebugUI::BeginTable(2, {});
    for (usize index = 0; index < categories.Count() && index < MaxRows; ++index)
    {
        LogCategory &category = *categories[index];
        const LogLevel level = category.GetLevel();
        const StringView name(category.GetName());
        DebugUI::TableCell(name);
        const String label = Format("{0}##{1}", Log::GetLevelName(level), name);
        if (DebugUI::TableButton(label.AsView()))
        {
            category.SetLevel(NextLevel(level));
        }
    }
    DebugUI::EndTable();
    if (categories.Count() > MaxRows)
    {
        DebugUI::Text(Format("{0} more; filter to narrow", categories.Count() - MaxRows), DebugUIStyle::Dim);
    }
    DebugUI::TreePop();
}

void DebugUISubsystem::DrawConsole()
{
    bool open = true;
    if (DebugUI::BeginWindow("Console", {300, 80}, 520, DebugWindowFlags::None, &open))
    {
        for (const String &line : _consoleOutput)
        {
            DebugUI::Text(line, DebugUIStyle::Dim);
        }
        if (DebugUI::InputText("##Command", _command) && !_command.IsEmpty())
        {
            StringBuilder<256> output;
            const bool succeeded = Console::Execute(_command.AsView().Trim(), output);
            _consoleOutput.Add(Format("> {0}", _command));
            if (!output.AsView().IsEmpty())
            {
                _consoleOutput.Add(String(output.AsView()));
            }
            else if (!succeeded)
            {
                _consoleOutput.Add(String("Unknown command"));
            }
            while (_consoleOutput.Count() > 8)
            {
                _consoleOutput.RemoveAt(0);
            }
            _command = String();
        }
        DebugUI::Separator();
        (void)DebugUI::InputText("Filter", _filter);
        DrawLogLevels();
        // Filtering and sorting every variable is redone only when the filter or the variables change, which a sum of
        // their hashed addresses notices in any order.
        uint64 variablesHash = 0;
        Console::ForEachVariable([&](ConsoleVariable &variable) {
            variablesHash += HashInteger(static_cast<uint64>(reinterpret_cast<uintptr>(&variable)));
        });
        if (!_consoleVariablesValid || variablesHash != _consoleVariablesHash || _filter != _consoleFilter)
        {
            _consoleVariablesValid = true;
            _consoleVariablesHash = variablesHash;
            _consoleFilter = _filter;
            _consoleVariables.Clear();
            Console::ForEachVariable([&](ConsoleVariable &variable) {
                if (_filter.IsEmpty() || ContainsIgnoreCase(variable.GetName(), _filter.AsView()))
                {
                    _consoleVariables.Add(&variable);
                }
            });
            _consoleVariables.Sort(
                [](const ConsoleVariable *a, const ConsoleVariable *b) { return a->GetName() < b->GetName(); });
        }
        const List<ConsoleVariable *> &variables = _consoleVariables;
        constexpr usize MaxRows = 40;
        usize rows = 0;
        usize groupStart = 0;
        while (groupStart < variables.Count() && rows < MaxRows)
        {
            const StringView group = GroupOf(variables[groupStart]->GetName());
            usize groupEnd = groupStart;
            while (groupEnd < variables.Count() && GroupOf(variables[groupEnd]->GetName()) == group)
            {
                ++groupEnd;
            }
            if (DebugUI::TreeNode(group, !_filter.IsEmpty()))
            {
                for (usize index = groupStart; index < groupEnd && rows < MaxRows; ++index, ++rows)
                {
                    ConsoleVariable &variable = *variables[index];
                    if (&variable.GetType() == &TypeOf<bool>())
                    {
                        bool value = *static_cast<const bool *>(variable.GetValue());
                        if (DebugUI::Checkbox(variable.GetName(), value) && !variable.HasFlag("ReadOnly"))
                        {
                            (void)variable.Set(value ? "true" : "false", ConsoleSetBy::Console);
                        }
                        continue;
                    }
                    const uint64 id = _context->MakeId(variable.GetName());
                    String &buffer = _context->EditBuffers.FindOrAdd(id);
                    if (_context->Focused != id)
                    {
                        buffer = variable.ToString();
                    }
                    if (DebugUI::InputText(variable.GetName(), buffer))
                    {
                        (void)variable.Set(buffer.AsView().Trim(), ConsoleSetBy::Console);
                        _context->Focused = 0;
                    }
                }
                DebugUI::TreePop();
            }
            ++rows;
            groupStart = groupEnd;
        }
        if (groupStart < variables.Count())
        {
            DebugUI::Text(Format("{0} more; filter to narrow", variables.Count() - groupStart), DebugUIStyle::Dim);
        }
    }
    DebugUI::EndWindow();
    if (!open)
    {
        if (ConsoleVariable *variable = Console::FindVariable("ui.DebugConsole"))
        {
            (void)variable->Set("false", ConsoleSetBy::Console);
        }
    }
}
} // namespace Marlin
