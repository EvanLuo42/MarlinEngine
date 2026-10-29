#include "UIPainterRenderer/UIGlyphCache.h"

#include "Math/MathUtility.h"
#include "Templates/Bit.h"
#include "Templates/Utility.h"
#include "UIPainter/UIOutline.h"
#include "UIText/UITextSubsystem.h"

namespace Marlin
{
namespace
{
constexpr int32 Padding = 1;
constexpr uint32 MaxSlot = 128;

UITransform ToForward(const UIGpuTransform &inverse)
{
    UITransform transform{inverse.M11, inverse.M12, inverse.M21, inverse.M22, inverse.Dx, inverse.Dy};
    return transform.Inverse();
}

void Drop(UIGpuCommand &command)
{
    command.Bounds[0] = 0;
    command.Bounds[1] = 0;
    command.Bounds[2] = 0;
    command.Bounds[3] = 0;
}
} // namespace

uint32 UIGlyphCache::FindOutline(const UITextSubsystem &text, UIFontFace face, uint32 glyph)
{
    const OutlineKey key{face.Id, glyph};
    if (const uint32 *record = _outlines.Find(key))
    {
        return *record;
    }
    UIPath path;
    const uint32 record = text.GetGlyphOutline(face, glyph, path) ? AppendOutline(path, _curves, _words) : ~0u;
    _outlines.FindOrAdd(key) = record;
    return record;
}

bool UIGlyphCache::Allocate(uint32 width, uint32 height, uint16 &x, uint16 &y)
{
    if (_shelfX + width > AtlasSize)
    {
        _shelfY += _shelfHeight;
        _shelfX = 0;
        _shelfHeight = 0;
    }
    if (_shelfY + height > AtlasSize || width > AtlasSize)
    {
        return false;
    }
    x = static_cast<uint16>(_shelfX);
    y = static_cast<uint16>(_shelfY);
    _shelfX += width;
    _shelfHeight = Max(_shelfHeight, height);
    return true;
}

void UIGlyphCache::ResetAtlas()
{
    _atlas.Clear();
    _shelfX = 0;
    _shelfY = 0;
    _shelfHeight = 0;
    _pendingGlyphs.Clear();
    _clearAtlas = true;
}

void UIGlyphCache::Resolve(UICommandList &commands, const UITextSubsystem &text)
{
    if (!ResolveAll(commands, text))
    {
        ResetAtlas();
        (void)ResolveAll(commands, text);
    }
}

const UIGlyphCache::TransformInfo &UIGlyphCache::GetTransformInfo(const UICommandList &commands, uint32 index)
{
    TransformInfo &info = _transformInfos[index];
    if (!info.Known)
    {
        info.Forward = ToForward(commands.Transforms[index]);
        const UITransform &forward = info.Forward;
        info.AxisAligned = forward.M12 == 0 && forward.M21 == 0 && forward.M11 > 0 &&
                           Math::Abs(forward.M11 - forward.M22) <= forward.M11 * 1e-4f;
        info.DeviceScale = info.AxisAligned ? forward.M11 : Math::Sqrt(Math::Abs(forward.GetDeterminant()));
        info.Known = true;
    }
    return info;
}

bool UIGlyphCache::ResolveAll(UICommandList &commands, const UITextSubsystem &text)
{
    // One pass finds every glyph's outline and atlas slot, and only then are the commands rewritten, so that an atlas
    // that fills up partway can be emptied and the frame resolved again from its Glyph commands.
    _resolved.Clear();
    _transformInfos.Clear();
    _transformInfos.Resize(commands.Transforms.Count());
    for (UICanvas &canvas : commands.Canvases)
    {
        for (UIGpuCommand &command : canvas.Commands)
        {
            if (command.GetKind() != UICommandKind::Glyph)
            {
                continue;
            }
            ResolvedGlyph &resolved = _resolved.AddDefaulted();
            resolved.Command = &command;
            const UIFontFace face{BitCast<uint32>(command.Data[0])};
            const uint32 glyph = BitCast<uint32>(command.Data[1]);
            const Vector2 origin(command.Data[2], command.Data[3]);
            const float32 size = command.Data[4];
            if (!text.IsReady(face))
            {
                continue;
            }
            const uint32 record = FindOutline(text, face, glyph);
            if (record == ~0u)
            {
                continue;
            }
            resolved.Record = record;
            const TransformInfo &transform = GetTransformInfo(commands, command.Transform);
            const float32 deviceScale = transform.DeviceScale;
            const float32 pixelsPerEm = size * deviceScale;
            if (!transform.AxisAligned || pixelsPerEm > MaxAtlasSize)
            {
                continue;
            }
            const Vector2 device = transform.Forward.Apply(origin);
            const float32 snappedX = Math::Floor(device.X);
            const auto phase =
                static_cast<uint32>(Math::Floor((device.X - snappedX) * static_cast<float32>(SubpixelPhases))) %
                SubpixelPhases;
            const float32 phaseOffset = static_cast<float32>(phase) / static_cast<float32>(SubpixelPhases);
            const AtlasKey key{face.Id, glyph, static_cast<uint32>(Math::Floor(pixelsPerEm * 8 + 0.5f)), phase};
            resolved.SnappedX = snappedX;
            resolved.SnappedY = Math::Floor(device.Y + 0.5f);
            if (const UIAtlasEntry *found = _atlas.Find(key))
            {
                resolved.Entry = *found;
                resolved.Atlas = true;
                continue;
            }
            const float32 scale = static_cast<float32>(key.Size) / 8;
            const float32 ratio = deviceScale * (scale / Max(pixelsPerEm, 1e-6f));
            const auto left = static_cast<int32>(Math::Floor(command.Data[5] * ratio + phaseOffset)) - Padding;
            const auto top = static_cast<int32>(Math::Floor(command.Data[6] * ratio)) - Padding;
            const auto right = static_cast<int32>(Math::Ceil(command.Data[7] * ratio + phaseOffset)) + Padding;
            const auto bottom = static_cast<int32>(Math::Ceil(command.Data[8] * ratio)) + Padding;
            const auto width = static_cast<uint32>(Max(right - left, 1));
            const auto height = static_cast<uint32>(Max(bottom - top, 1));
            if (width > MaxSlot || height > MaxSlot)
            {
                continue;
            }
            uint16 x = 0;
            uint16 y = 0;
            if (!Allocate(width, height, x, y))
            {
                return false;
            }
            resolved.Entry = {x,
                              y,
                              static_cast<uint16>(width),
                              static_cast<uint16>(height),
                              static_cast<int16>(left),
                              static_cast<int16>(top)};
            resolved.Atlas = true;
            _atlas.FindOrAdd(key) = resolved.Entry;
            UiAtlasGlyph &job = _pendingGlyphs.AddDefaulted();
            job.Record = record;
            job.X = x;
            job.Y = y;
            job.Width = width;
            job.Height = height;
            job.Scale = scale;
            job.OriginX = phaseOffset - static_cast<float32>(left);
            job.OriginY = -static_cast<float32>(top);
            job.Gamma = 1 - 0.2f * Math::Saturate((16 - scale) / 8);
        }
    }

    for (const ResolvedGlyph &resolved : _resolved)
    {
        UIGpuCommand &command = *resolved.Command;
        if (resolved.Record == ~0u)
        {
            Drop(command);
        }
        else if (resolved.Atlas)
        {
            const UIAtlasEntry &entry = resolved.Entry;
            const UIGpuTransform &inverse = commands.Transforms[command.Transform];
            const float32 x0 = resolved.SnappedX + static_cast<float32>(entry.Left);
            const float32 y0 = resolved.SnappedY + static_cast<float32>(entry.Top);
            const float32 x1 = x0 + static_cast<float32>(entry.Width);
            const float32 y1 = y0 + static_cast<float32>(entry.Height);
            const UITransform toLocal{inverse.M11, inverse.M12, inverse.M21, inverse.M22, inverse.Dx, inverse.Dy};
            const Vector2 localMin = toLocal.Apply({x0, y0});
            const Vector2 localMax = toLocal.Apply({x1, y1});
            const float32 atlasSize = static_cast<float32>(AtlasSize);
            const float32 data[] = {localMin.X,
                                    localMin.Y,
                                    localMax.X,
                                    localMax.Y,
                                    static_cast<float32>(entry.X) / atlasSize,
                                    static_cast<float32>(entry.Y) / atlasSize,
                                    static_cast<float32>(entry.X + entry.Width) / atlasSize,
                                    static_cast<float32>(entry.Y + entry.Height) / atlasSize};
            for (uint32 index = 0; index < 8; ++index)
            {
                command.Data[index] = data[index];
            }
            command.Header = (command.Header & ~0xFFu) | static_cast<uint32>(UICommandKind::AtlasGlyph);
            command.Bounds[0] = Max(command.Bounds[0], x0);
            command.Bounds[1] = Max(command.Bounds[1], y0);
            command.Bounds[2] = Min(command.Bounds[2], x1);
            command.Bounds[3] = Min(command.Bounds[3], y1);
        }
        else
        {
            // Record, origin and size, from the Glyph command's face, glyph, origin and size.
            const float32 size = command.Data[4];
            command.Header =
                static_cast<uint32>(UICommandKind::Outline) | (static_cast<uint32>(UICommandFlags::GlyphCache) << 8);
            command.Data[0] = BitCast<float32>(resolved.Record);
            command.Data[1] = command.Data[2];
            command.Data[2] = command.Data[3];
            command.Data[3] = size;
        }
    }
    return true;
}

void UIGlyphCache::TakeUploads(List<UiAtlasGlyph> &atlasGlyphs, bool &clearAtlas)
{
    atlasGlyphs = Move(_pendingGlyphs);
    _pendingGlyphs.Clear();
    clearAtlas = _clearAtlas;
    _clearAtlas = false;
}
} // namespace Marlin
