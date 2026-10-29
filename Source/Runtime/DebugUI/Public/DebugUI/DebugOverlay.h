#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "String/StringView.h"
#include "DebugUI/DebugOverlay.gen.h"

namespace Marlin
{
/// What every overlay section sees in a frame.
struct DebugOverlayFrame
{
    const FrameTime &Time;
    /// The intervals of the last frames in milliseconds, oldest first.
    Span<const float32> FrameMilliseconds;
};

/// A section of the debug overlay (09 §3): a stateless provider found through
/// reflection, drawn under its title with DebugUI rows while ui.DebugOverlay is on, in `MCLASS(Priority = N)` order
/// (higher first). State it plots over time belongs to the section object, which lives as long as the overlay.
MCLASS(Abstract, ExtensionPoint)
class DEBUGUI_API DebugOverlaySection
{
    MGENERATED_BODY()
public:
    DebugOverlaySection() = default;
    virtual ~DebugOverlaySection() = default;
    MARLIN_NONCOPYABLE(DebugOverlaySection);

    [[nodiscard]] virtual StringView GetTitle() const = 0;
    virtual void Draw(const DebugOverlayFrame &frame) = 0;
};

/// Frame rate, frame interval and its history.
MCLASS(Priority = 300)
class DEBUGUI_API FrameOverlaySection final : public DebugOverlaySection
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetTitle() const override;
    void Draw(const DebugOverlayFrame &frame) override;
};

/// The device, the main surface, GPU frame time from RenderGraph's timestamps, its costliest passes and memory.
MCLASS(Priority = 200)
class DEBUGUI_API GpuOverlaySection final : public DebugOverlaySection
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetTitle() const override;
    void Draw(const DebugOverlayFrame &frame) override;

private:
    static constexpr uint32 HistoryLength = 120;

    float32 _history[HistoryLength] = {};
    uint32 _historyCount = 0;
};

/// What UIPainter drew: commands, tiles and the glyph cache.
MCLASS(Priority = 100)
class DEBUGUI_API UIPainterOverlaySection final : public DebugOverlaySection
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetTitle() const override;
    void Draw(const DebugOverlayFrame &frame) override;
};
} // namespace Marlin
// RECOVERY-DECISION D2: the head of the DebugOverlaySection comment (line 19, up to "09 §3)") was lost; reworded.
