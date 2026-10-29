#include "DebugUI/DebugOverlay.h"

#include "DebugUI/DebugUI.h"
#include "Engine/Engine.h"
#include "HAL/PlatformTime.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiGpuProfiler.h"
#include "RenderGraph/RenderGraphSubsystem.h"
#include "Renderer/RenderSurface.h"
#include "String/Format.h"
#include "UIPainterRenderer/UIPainterSubsystem.h"

namespace Marlin
{
namespace
{
LinearColor FrameColor(float32 milliseconds)
{
    return milliseconds <= 17.0f   ? DebugUIStyle::Good
           : milliseconds <= 34.0f ? DebugUIStyle::Warning
                                   : DebugUIStyle::Bad;
}

float32 PlotCeiling(Span<const float32> values)
{
    float32 peak = 0;
    for (const float32 value : values)
    {
        peak = value > peak ? value : peak;
    }
    return peak <= 16.7f ? 16.7f : peak <= 33.4f ? 33.4f : peak * 1.1f;
}

String Bytes(uint64 bytes)
{
    return bytes >= (1ull << 30) ? Format("{0:F2} GB", static_cast<float64>(bytes) / static_cast<float64>(1ull << 30))
                                 : Format("{0:F0} MB", static_cast<float64>(bytes) / static_cast<float64>(1ull << 20));
}
} // namespace

StringView FrameOverlaySection::GetTitle() const
{
    return "FRAME";
}

void FrameOverlaySection::Draw(const DebugOverlayFrame &frame)
{
    float32 average = 0;
    float32 worst = 0;
    for (const float32 value : frame.FrameMilliseconds)
    {
        average += value;
        worst = value > worst ? value : worst;
    }
    average /= frame.FrameMilliseconds.IsEmpty() ? 1.0f : static_cast<float32>(frame.FrameMilliseconds.Count());
    const float32 fps = average > 0 ? 1000.0f / average : 0;
    DebugUI::Value("FPS", Format("{0:F0}", fps), FrameColor(average));
    DebugUI::Value("Frame", Format("{0:F2} ms", average), FrameColor(average));
    DebugUI::Value("Worst", Format("{0:F2} ms", worst), FrameColor(worst));
    DebugUI::Value("Frame index", Format("{0}", frame.Time.Frame));
    DebugUI::Plot(frame.FrameMilliseconds, 0, PlotCeiling(frame.FrameMilliseconds), 40, FrameColor(average));
}

StringView GpuOverlaySection::GetTitle() const
{
    return "GPU";
}

void GpuOverlaySection::Draw(const DebugOverlayFrame &frame)
{
    (void)frame;
    RenderGraphSubsystem *graphs = Engine::GetSubsystem<RenderGraphSubsystem>();
    RhiDevice *device = graphs != nullptr ? graphs->GetDevice() : nullptr;
    if (device == nullptr)
    {
        DebugUI::Text("No device", DebugUIStyle::Dim);
        return;
    }
    DebugUI::Text(device->GetAdapterInfo().Name);
    DebugUI::Value("Backend", Format("{0}", device->GetBackendName()));
    if (UIPainterSubsystem *painter = Engine::GetSubsystem<UIPainterSubsystem>())
    {
        if (const UIWindowCanvas *canvas = painter->GetMainCanvas())
        {
            const RenderSurface &surface = canvas->GetSurface();
            DebugUI::Value("Surface", Format("{0}x{1} {2}", surface.GetWidth(), surface.GetHeight(),
                                             surface.IsHdr() ? StringView("HDR10") : StringView("SDR")));
            if (surface.IsHdr())
            {
                DebugUI::Value("UI white", Format("{0:F0} nits", surface.GetUiWhiteNits()));
            }
        }
    }

    const Span<const RhiGpuZone> zones = graphs->GetLastGpuFrame(RhiQueueType::Graphics);
    struct Pass
    {
        const char *Name;
        uint64 Ticks;
    };
    Pass passes[64];
    uint32 passCount = 0;
    uint64 begin = ~0ull;
    uint64 end = 0;
    for (const RhiGpuZone &zone : zones)
    {
        if (zone.Depth != 0 || zone.EndTicks < zone.BeginTicks)
        {
            continue;
        }
        begin = zone.BeginTicks < begin ? zone.BeginTicks : begin;
        end = zone.EndTicks > end ? zone.EndTicks : end;
        uint32 index = 0;
        while (index < passCount && passes[index].Name != zone.Name)
        {
            ++index;
        }
        if (index == passCount)
        {
            if (passCount == 64)
            {
                continue;
            }
            passes[passCount++] = {zone.Name, 0};
        }
        passes[index].Ticks += zone.EndTicks - zone.BeginTicks;
    }
    const float64 toMilliseconds = 1000.0 / static_cast<float64>(PlatformTime::TicksPerSecond());
    const float32 gpu = end > begin ? static_cast<float32>(static_cast<float64>(end - begin) * toMilliseconds) : 0;
    _history[_historyCount % HistoryLength] = gpu;
    ++_historyCount;
    DebugUI::Value("GPU frame", Format("{0:F2} ms", gpu), FrameColor(gpu));
    float32 ordered[HistoryLength];
    const uint32 count = _historyCount < HistoryLength ? _historyCount : HistoryLength;
    for (uint32 index = 0; index < count; ++index)
    {
        ordered[index] = _history[(_historyCount - count + index) % HistoryLength];
    }
    const Span<const float32> history(ordered, count);
    DebugUI::Plot(history, 0, PlotCeiling(history), 32, DebugUIStyle::Accent);
    for (uint32 shown = 0; shown < 5 && shown < passCount; ++shown)
    {
        uint32 top = shown;
        for (uint32 index = shown + 1; index < passCount; ++index)
        {
            top = passes[index].Ticks > passes[top].Ticks ? index : top;
        }
        const Pass pass = passes[top];
        passes[top] = passes[shown];
        passes[shown] = pass;
        DebugUI::Value(pass.Name != nullptr ? StringView(pass.Name) : StringView("?"),
                       Format("{0:F3} ms", static_cast<float64>(pass.Ticks) * toMilliseconds), DebugUIStyle::Dim);
    }
    const RhiMemoryBudget memory = device->GetMemoryBudget(RhiMemoryLocation::Device);
    if (memory.Budget != 0)
    {
        const float32 used = static_cast<float32>(memory.Usage) / static_cast<float32>(memory.Budget);
        DebugUI::Value("VRAM", Format("{0} / {1}", Bytes(memory.Usage), Bytes(memory.Budget)),
                       used < 0.8f    ? DebugUIStyle::Text
                       : used < 0.95f ? DebugUIStyle::Warning
                                      : DebugUIStyle::Bad);
    }
}

StringView UIPainterOverlaySection::GetTitle() const
{
    return "UI";
}

void UIPainterOverlaySection::Draw(const DebugOverlayFrame &frame)
{
    (void)frame;
    UIPainterSubsystem *painter = Engine::GetSubsystem<UIPainterSubsystem>();
    if (painter == nullptr)
    {
        return;
    }
    const UIPainterFrameStats &stats = painter->GetLastFrameStats();
    const UIGlyphCache &glyphs = painter->GetGlyphCache();
    DebugUI::Value("Commands", Format("{0}", stats.Commands));
    DebugUI::Value("Tiles | pairs", Format("{0} | {1}", stats.Tiles, stats.TilePairs));
    DebugUI::Value("Atlas", Format("{0} glyphs | {1:F0}%", glyphs.GetAtlasGlyphCount(), glyphs.GetAtlasUsage() * 100));
    DebugUI::Value("Outlines", Format("{0}", glyphs.GetOutlineCount()));
}
} // namespace Marlin
