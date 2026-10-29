#include "UIPainter/UIPainter.h"

#include "UIPainter/UIOutline.h"

#include "Diagnostics/Log.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

MARLIN_DEFINE_LOG_CATEGORY(LogUIPainter);

namespace Marlin
{
namespace
{
constexpr float32 AntialiasMargin = 1.5f;

void SetPremultiplied(float32 (&target)[4], const LinearColor &color)
{
    const float32 alpha = Math::Saturate(color.A);
    target[0] = color.R * alpha;
    target[1] = color.G * alpha;
    target[2] = color.B * alpha;
    target[3] = alpha;
}

float32 AsData(uint32 value)
{
    return BitCast<float32>(value);
}

Vector2 Normalize(Vector2 vector)
{
    const float32 length = vector.Length();
    return length > 1e-12f ? vector / length : Vector2(1, 0);
}

Vector2 Perpendicular(Vector2 vector)
{
    return {-vector.Y, vector.X};
}

float32 Cross(Vector2 a, Vector2 b)
{
    return a.X * b.Y - a.Y * b.X;
}

UIRect PointsBounds(Span<const Vector2> points)
{
    Vector2 min = points[0];
    Vector2 max = points[0];
    for (const Vector2 &point : points)
    {
        min = {Min(min.X, point.X), Min(min.Y, point.Y)};
        max = {Max(max.X, point.X), Max(max.Y, point.Y)};
    }
    return UIRect::FromCorners(min, max);
}

UICornerRadii ClampRadii(const UICornerRadii &radii, const UIRect &rect)
{
    const float32 limit = Max(Min(rect.Width, rect.Height) * 0.5f, 0.0f);
    return {Math::Clamp(radii.TopLeft, 0.0f, limit), Math::Clamp(radii.TopRight, 0.0f, limit),
            Math::Clamp(radii.BottomRight, 0.0f, limit), Math::Clamp(radii.BottomLeft, 0.0f, limit)};
}

} // namespace

void UICommandList::Reset()
{
    // In reverse, so that the next recording's canvases take back the lists of the same canvases, the root first.
    for (usize index = Canvases.Count(); index-- > 0;)
    {
        Canvases[index].Commands.Clear();
        _spareCommands.Add(Move(Canvases[index].Commands));
    }
    Canvases.Clear();
    Brushes.Clear();
    Stops.Clear();
    Transforms.Clear();
    Clips.Clear();
    Curves.Clear();
    OutlineWords.Clear();
}

UICanvas &UICommandList::AddCanvas()
{
    UICanvas &canvas = Canvases.AddDefaulted();
    if (!_spareCommands.IsEmpty())
    {
        canvas.Commands = _spareCommands.Pop();
    }
    return canvas;
}

usize UICommandList::GetCommandCount() const
{
    usize count = 0;
    for (const UICanvas &canvas : Canvases)
    {
        count += canvas.Commands.Count();
    }
    return count;
}

void UICommandList::Append(const UICommandList &other)
{
    if (other.Canvases.IsEmpty())
    {
        return;
    }
    if (Canvases.IsEmpty())
    {
        List<List<UIGpuCommand>> spare = Move(_spareCommands);
        *this = other;
        _spareCommands = Move(spare);
        return;
    }
    const auto brushBase = static_cast<uint32>(Brushes.Count());
    const auto stopBase = static_cast<uint32>(Stops.Count());
    const auto transformBase = static_cast<uint32>(Transforms.Count()) - 1;
    const auto clipBase = static_cast<uint32>(Clips.Count()) - 1;
    const auto curveBase = static_cast<uint32>(Curves.Count());
    const auto wordBase = static_cast<uint32>(OutlineWords.Count());
    const auto canvasBase = static_cast<uint32>(Canvases.Count()) - 1;
    const auto moveTransform = [&](uint32 index) { return index == 0 ? 0 : index + transformBase; };
    const auto moveClip = [&](uint32 index) { return index == 0 ? 0 : index + clipBase; };
    const auto moveCanvas = [&](uint32 index) { return index == 0 ? 0 : index + canvasBase; };

    for (UIGpuBrush brush : other.Brushes)
    {
        brush.StopOffset += stopBase;
        Brushes.Add(brush);
    }
    Stops.AddRange(other.Stops.AsSpan());
    Transforms.AddRange(other.Transforms.AsSpan().Slice(1));
    for (usize index = 1; index < other.Clips.Count(); ++index)
    {
        UIGpuClip clip = other.Clips[index];
        clip.Transform = moveTransform(clip.Transform);
        clip.Parent = moveClip(clip.Parent);
        Clips.Add(clip);
    }
    Curves.AddRange(other.Curves.AsSpan());
    usize word = 0;
    while (word < other.OutlineWords.Count())
    {
        UIGpuOutlineHeader header;
        MemCopy(&header, other.OutlineWords.Data() + word, sizeof(header));
        const usize ranges = (header.BandsX + header.BandsY) * 2;
        usize end = word + sizeof(header) / sizeof(uint32) + ranges;
        for (usize band = 0; band < header.BandsX + header.BandsY; ++band)
        {
            const usize slot = word + sizeof(header) / sizeof(uint32) + band * 2;
            end = Max<usize>(end, other.OutlineWords[slot] + other.OutlineWords[slot + 1]);
        }
        header.CurveOffset += curveBase;
        OutlineWords.AddRange(
            Span<const uint32>(reinterpret_cast<const uint32 *>(&header), sizeof(header) / sizeof(uint32)));
        for (usize index = word + sizeof(header) / sizeof(uint32);
             index < word + sizeof(header) / sizeof(uint32) + ranges; index += 2)
        {
            OutlineWords.Add(other.OutlineWords[index] + wordBase);
            OutlineWords.Add(other.OutlineWords[index + 1]);
        }
        OutlineWords.AddRange(other.OutlineWords.AsSpan().Slice(
            word + sizeof(header) / sizeof(uint32) + ranges, end - (word + sizeof(header) / sizeof(uint32) + ranges)));
        word = end;
    }

    for (usize index = 0; index < other.Canvases.Count(); ++index)
    {
        const UICanvas &source = other.Canvases[index];
        UICanvas *target = nullptr;
        if (index == 0)
        {
            target = &Canvases[0];
        }
        else
        {
            target = &AddCanvas();
            target->Parent = moveCanvas(source.Parent);
            target->Opacity = source.Opacity;
            target->Blur = source.Blur;
            target->BackdropBlur = source.BackdropBlur;
            target->Bounds = source.Bounds;
        }
        const auto commandBase = static_cast<uint32>(target->Commands.Count());
        for (UIGpuCommand command : source.Commands)
        {
            command.Brush += brushBase;
            command.Clip = moveClip(command.Clip);
            command.Transform = moveTransform(command.Transform);
            command.Group += commandBase;
            const UICommandKind kind = command.GetKind();
            if (kind == UICommandKind::Outline &&
                (command.GetFlags() & UICommandFlags::GlyphCache) == UICommandFlags::None)
            {
                command.Data[0] = BitCast<float32>(BitCast<uint32>(command.Data[0]) + wordBase);
            }
            else if (kind == UICommandKind::Canvas || kind == UICommandKind::Backdrop)
            {
                command.Data[4] = BitCast<float32>(moveCanvas(BitCast<uint32>(command.Data[4])));
            }
            target->Commands.Add(command);
        }
    }
}

UIPainter::UIPainter() = default;
UIPainter::~UIPainter() = default;

void UIPainter::Begin(UICommandList &commands, uint32 width, uint32 height, float32 scale)
{
    _commands = &commands;
    _width = width;
    _height = height;
    _scale = scale > 0 ? scale : 1;
    commands.Reset();
    const UIRect target(0, 0, static_cast<float32>(width), static_cast<float32>(height));
    UICanvas &root = commands.AddCanvas();
    root.Bounds = target;
    commands.Transforms.AddDefaulted();
    UIGpuClip &none = commands.Clips.AddDefaulted();
    none.DeviceRect[0] = 0;
    none.DeviceRect[1] = 0;
    none.DeviceRect[2] = target.Width;
    none.DeviceRect[3] = target.Height;
    _transforms.Clear();
    _transforms.Add(UITransform::Scale(_scale, _scale));
    _transformIndices.Clear();
    _transformIndices.Add(~0u);
    _clips.Clear();
    _clips.Add({0, target});
    _layers.Clear();
    _layers.Add({});
    _group = ~0u;
    _lastBrushIndex = ~0u;
}

void UIPainter::End()
{
    if (_commands == nullptr)
    {
        return;
    }
    if (_clips.Count() > 1 || _transforms.Count() > 1 || _layers.Count() > 1)
    {
        MARLIN_LOG(LogUIPainter, Warning, "UIPainter ended with {0} clips, {1} transforms and {2} layers still pushed",
                   _clips.Count() - 1, _transforms.Count() - 1, _layers.Count() - 1);
    }
    while (_layers.Count() > 1)
    {
        PopLayer();
    }
    _clips.Clear();
    _transforms.Clear();
    _transformIndices.Clear();
    _commands = nullptr;
}

Vector2 UIPainter::GetSize() const
{
    return {static_cast<float32>(_width) / _scale, static_cast<float32>(_height) / _scale};
}

UIRect UIPainter::GetClipBounds() const
{
    return _clips.IsEmpty() ? UIRect() : _clips.Last().DeviceBounds;
}

bool UIPainter::IsClippedOut(const UIRect &rect) const
{
    return _commands == nullptr || GetTransform()
                                       .TransformBounds(rect)
                                       .Inflate(AntialiasMargin)
                                       .Intersect(_clips.Last().DeviceBounds)
                                       .IsEmpty();
}

UICanvas &UIPainter::GetCanvas()
{
    return _commands->Canvases[_layers.Last().Canvas];
}

uint32 UIPainter::AddBrush(const UIBrush &brush)
{
    if (_commands == nullptr)
    {
        return 0;
    }
    UIGpuBrush gpu;
    gpu.Kind = static_cast<uint32>(brush.Kind);
    SetPremultiplied(gpu.Color, brush.Color);
    if (brush.Kind != UIBrushKind::Solid)
    {
        gpu.Geometry[0] = brush.Start.X;
        gpu.Geometry[1] = brush.Start.Y;
        gpu.Geometry[2] = brush.Kind == UIBrushKind::Linear   ? brush.End.X
                          : brush.Kind == UIBrushKind::Radial ? brush.Radius
                                                              : brush.StartAngle;
        gpu.Geometry[3] = brush.End.Y;
        const usize count = Min(brush.Stops.Count(), static_cast<usize>(UIBrush::MaxStops));
        gpu.StopOffset = static_cast<uint32>(_commands->Stops.Count());
        gpu.StopCount = static_cast<uint32>(count);
        for (usize index = 0; index < count; ++index)
        {
            UIGpuGradientStop &stop = _commands->Stops.AddDefaulted();
            stop.Offset = brush.Stops[index].Offset;
            SetPremultiplied(stop.Color, brush.Stops[index].Color);
        }
        if (count == 0)
        {
            gpu.Kind = static_cast<uint32>(UIGpuBrushKind::Solid);
        }
    }
    else if (_lastBrushIndex != ~0u && _lastBrush.Kind == gpu.Kind && _lastBrush.Color[0] == gpu.Color[0] &&
             _lastBrush.Color[1] == gpu.Color[1] && _lastBrush.Color[2] == gpu.Color[2] &&
             _lastBrush.Color[3] == gpu.Color[3])
    {
        return _lastBrushIndex;
    }
    _lastBrush = gpu;
    _lastBrushIndex = static_cast<uint32>(_commands->Brushes.Count());
    _commands->Brushes.Add(gpu);
    return _lastBrushIndex;
}

uint32 UIPainter::AddTransform()
{
    if (_transformIndices.Last() != ~0u)
    {
        return _transformIndices.Last();
    }
    const UITransform inverse = GetTransform().Inverse();
    UIGpuTransform &gpu = _commands->Transforms.AddDefaulted();
    gpu.M11 = inverse.M11;
    gpu.M12 = inverse.M12;
    gpu.M21 = inverse.M21;
    gpu.M22 = inverse.M22;
    gpu.Dx = inverse.Dx;
    gpu.Dy = inverse.Dy;
    gpu.PixelSize = Math::Sqrt(Math::Abs(inverse.GetDeterminant()));
    _transformIndices.Last() = static_cast<uint32>(_commands->Transforms.Count() - 1);
    return _transformIndices.Last();
}

UIGpuCommand *UIPainter::AddCommand(UICommandKind kind, const UIRect &localBounds, float32 margin, uint32 brush,
                                    UICommandFlags flags)
{
    if (_commands == nullptr)
    {
        return nullptr;
    }
    const UIRect device =
        GetTransform().TransformBounds(localBounds).Inflate(margin).Intersect(_clips.Last().DeviceBounds);
    if (device.IsEmpty())
    {
        return nullptr;
    }
    UICanvas &canvas = GetCanvas();
    const auto index = static_cast<uint32>(canvas.Commands.Count());
    UIGpuCommand &command = canvas.Commands.AddDefaulted();
    command.Header = static_cast<uint32>(kind) | (static_cast<uint32>(flags) << 8);
    command.Brush = brush;
    command.Clip = _clips.Last().Index;
    command.Transform = AddTransform();
    command.Group = _group != ~0u ? _group : index;
    command.Bounds[0] = device.X;
    command.Bounds[1] = device.Y;
    command.Bounds[2] = device.X + device.Width;
    command.Bounds[3] = device.Y + device.Height;
    if (_layers.Count() > 1)
    {
        canvas.Bounds = canvas.Bounds.Union(device);
    }
    return &command;
}

UIGpuCommand *UIPainter::AddDeviceCommand(UICommandKind kind, const UIRect &deviceBounds, uint32 brush)
{
    UICanvas &canvas = GetCanvas();
    const auto index = static_cast<uint32>(canvas.Commands.Count());
    UIGpuCommand &command = canvas.Commands.AddDefaulted();
    command.Header = static_cast<uint32>(kind);
    command.Brush = brush;
    command.Group = index;
    command.Bounds[0] = deviceBounds.X;
    command.Bounds[1] = deviceBounds.Y;
    command.Bounds[2] = deviceBounds.X + deviceBounds.Width;
    command.Bounds[3] = deviceBounds.Y + deviceBounds.Height;
    return &command;
}

void UIPainter::BeginGroup()
{
    _group = static_cast<uint32>(GetCanvas().Commands.Count());
}

void UIPainter::EndGroup()
{
    _group = ~0u;
}

void UIPainter::FillRect(const UIRect &rect, const UIBrush &brush)
{
    FillRoundedRect(rect, {}, brush);
}

void UIPainter::FillRoundedRect(const UIRect &rect, const UICornerRadii &radii, const UIBrush &brush)
{
    if (rect.IsEmpty())
    {
        return;
    }
    const UICornerRadii clamped = ClampRadii(radii, rect);
    if (UIGpuCommand *command = AddCommand(UICommandKind::RoundedRect, rect, AntialiasMargin, AddBrush(brush)))
    {
        const float32 data[] = {rect.X,
                                rect.Y,
                                rect.X + rect.Width,
                                rect.Y + rect.Height,
                                clamped.TopLeft,
                                clamped.TopRight,
                                clamped.BottomRight,
                                clamped.BottomLeft,
                                0};
        MemCopy(command->Data, data, sizeof(data));
    }
}

void UIPainter::StrokeRoundedRect(const UIRect &rect, const UICornerRadii &radii, float32 width, const UIBrush &brush)
{
    if (rect.IsEmpty() || width <= 0)
    {
        return;
    }
    const UICornerRadii clamped = ClampRadii(radii, rect);
    if (UIGpuCommand *command = AddCommand(UICommandKind::RoundedRect, rect, AntialiasMargin, AddBrush(brush)))
    {
        const float32 data[] = {rect.X,
                                rect.Y,
                                rect.X + rect.Width,
                                rect.Y + rect.Height,
                                clamped.TopLeft,
                                clamped.TopRight,
                                clamped.BottomRight,
                                clamped.BottomLeft,
                                width};
        MemCopy(command->Data, data, sizeof(data));
    }
}

void UIPainter::FillEllipse(Vector2 center, Vector2 radii, const UIBrush &brush)
{
    StrokeEllipse(center, radii, 0, brush);
}

void UIPainter::StrokeEllipse(Vector2 center, Vector2 radii, float32 width, const UIBrush &brush)
{
    if (radii.X <= 0 || radii.Y <= 0 || width < 0)
    {
        return;
    }
    const UIRect bounds = UIRect::FromCorners(center - radii, center + radii).Inflate(width * 0.5f);
    if (UIGpuCommand *command = AddCommand(UICommandKind::Ellipse, bounds, AntialiasMargin, AddBrush(brush)))
    {
        const float32 data[] = {center.X, center.Y, radii.X, radii.Y, width};
        MemCopy(command->Data, data, sizeof(data));
    }
}

void UIPainter::DrawBoxShadow(const UIRect &rect, const UICornerRadii &radii, float32 blur, float32 spread,
                              const LinearColor &color)
{
    const UIRect shape = rect.Inflate(spread);
    if (shape.IsEmpty() || color.A <= 0)
    {
        return;
    }
    const auto grow = [spread](float32 radius) { return radius > 0 ? Max(radius + spread, 0.0f) : 0.0f; };
    const UICornerRadii clamped =
        ClampRadii({grow(radii.TopLeft), grow(radii.TopRight), grow(radii.BottomRight), grow(radii.BottomLeft)}, shape);
    const float32 sigma = Max(blur, 0.0f);
    if (UIGpuCommand *command =
            AddCommand(UICommandKind::BoxShadow, shape.Inflate(sigma * 3), AntialiasMargin, AddBrush(color)))
    {
        const float32 data[] = {shape.X,
                                shape.Y,
                                shape.X + shape.Width,
                                shape.Y + shape.Height,
                                clamped.TopLeft,
                                clamped.TopRight,
                                clamped.BottomRight,
                                clamped.BottomLeft,
                                sigma};
        MemCopy(command->Data, data, sizeof(data));
    }
}

void UIPainter::FillOutline(const UIEncodedOutline &outline, Vector2 origin, float32 scale, const UIBrush &brush,
                            UIFillRule rule)
{
    if (_commands == nullptr || outline.IsEmpty() || !(scale > 0))
    {
        return;
    }
    const UIRect bounds(origin.X + outline.Bounds.X * scale, origin.Y + outline.Bounds.Y * scale,
                        outline.Bounds.Width * scale, outline.Bounds.Height * scale);
    if (IsClippedOut(bounds))
    {
        return;
    }
    const uint32 record = AppendOutline(outline, _commands->Curves, _commands->OutlineWords);
    const UICommandFlags flags = rule == UIFillRule::EvenOdd ? UICommandFlags::EvenOdd : UICommandFlags::None;
    if (UIGpuCommand *command = AddCommand(UICommandKind::Outline, bounds, AntialiasMargin, AddBrush(brush), flags))
    {
        command->Data[0] = AsData(record);
        command->Data[1] = origin.X;
        command->Data[2] = origin.Y;
        command->Data[3] = scale;
    }
}

void UIPainter::FillPath(const UIPath &path, const UIBrush &brush, UIFillRule rule)
{
    if (_commands == nullptr || path.IsEmpty())
    {
        return;
    }
    const UIRect bounds = path.GetBounds();
    const UIRect device = GetTransform().TransformBounds(bounds).Inflate(AntialiasMargin);
    if (device.Intersect(_clips.Last().DeviceBounds).IsEmpty())
    {
        return;
    }
    const uint32 outline = AppendOutline(path, _commands->Curves, _commands->OutlineWords);
    if (outline == ~0u)
    {
        return;
    }
    const UICommandFlags flags = rule == UIFillRule::EvenOdd ? UICommandFlags::EvenOdd : UICommandFlags::None;
    if (UIGpuCommand *command = AddCommand(UICommandKind::Outline, bounds, AntialiasMargin, AddBrush(brush), flags))
    {
        command->Data[0] = AsData(outline);
        command->Data[1] = 0;
        command->Data[2] = 0;
        command->Data[3] = 1;
    }
}

void UIPainter::AddStrokePieces(Span<const UIPathContour> contours, Vector2 origin, const UIStrokeStyle &style,
                                uint32 brush)
{
    const float32 half = style.Width * 0.5f;
    const auto disk = [&](Vector2 center) {
        if (UIGpuCommand *command =
                AddCommand(UICommandKind::Disk, UIRect::FromCorners(center - Vector2(half), center + Vector2(half)),
                           AntialiasMargin, brush))
        {
            command->Data[0] = center.X;
            command->Data[1] = center.Y;
            command->Data[2] = half;
        }
    };
    const auto polygon = [&](Vector2 a, Vector2 b, Vector2 c, Vector2 d) {
        const Vector2 corners[] = {a, b, c, d};
        if (UIGpuCommand *command = AddCommand(UICommandKind::Polygon, PointsBounds(corners), AntialiasMargin, brush))
        {
            for (uint32 index = 0; index < 4; ++index)
            {
                command->Data[index * 2] = corners[index].X;
                command->Data[index * 2 + 1] = corners[index].Y;
            }
        }
    };
    const auto join = [&](Vector2 point, Vector2 incoming, Vector2 outgoing) {
        const float32 turn = Cross(incoming, outgoing);
        if (Math::Abs(turn) < 1e-5f && Vector2::Dot(incoming, outgoing) > 0)
        {
            return;
        }
        if (style.Join == UILineJoin::Round)
        {
            disk(point);
            return;
        }
        const float32 side = turn > 0 ? -1.0f : 1.0f;
        const Vector2 n0 = Perpendicular(incoming) * side;
        const Vector2 n1 = Perpendicular(outgoing) * side;
        const Vector2 a = point + n0 * half;
        const Vector2 b = point + n1 * half;
        const Vector2 bisector = n0 + n1;
        if (style.Join == UILineJoin::Miter && bisector.LengthSquared() > 1e-8f)
        {
            const Vector2 direction = Normalize(bisector);
            const float32 cosine = Vector2::Dot(direction, n0);
            if (cosine > 1e-4f && 1 / cosine <= style.MiterLimit)
            {
                polygon(point, a, point + direction * (half / cosine), b);
                return;
            }
        }
        polygon(point, a, b, point);
    };
    const auto cap = [&](Vector2 point, Vector2 outward) {
        if (style.Cap == UILineCap::Round)
        {
            disk(point);
        }
        else if (style.Cap == UILineCap::Square)
        {
            const Vector2 normal = Perpendicular(outward) * half;
            const Vector2 forward = outward * half;
            polygon(point + normal, point + normal + forward, point - normal + forward, point - normal);
        }
    };

    for (const UIPathContour &contour : contours)
    {
        if (contour.Segments.IsEmpty())
        {
            if (style.Cap == UILineCap::Round)
            {
                disk(contour.Start + origin);
            }
            continue;
        }
        const usize last = contour.Segments.Count() - 1;
        const bool endsMeet = contour.Closed || style.Cap != UILineCap::Butt;
        for (usize index = 0; index <= last; ++index)
        {
            UIPathSegment segment = contour.Segments[index];
            segment.P0 += origin;
            segment.Control += origin;
            segment.P1 += origin;
            const uint32 joined = (index > 0 || endsMeet ? 1u : 0u) | (index < last || endsMeet ? 2u : 0u);
            const Vector2 corners[] = {segment.P0, segment.Control, segment.P1};
            const UIRect bounds = PointsBounds(corners).Inflate(half);
            const UICommandKind kind = segment.Quad ? UICommandKind::StrokeQuad : UICommandKind::StrokeLine;
            if (UIGpuCommand *command = AddCommand(kind, bounds, AntialiasMargin, brush))
            {
                if (segment.Quad)
                {
                    const float32 data[] = {
                        segment.P0.X, segment.P0.Y, segment.Control.X, segment.Control.Y, segment.P1.X,
                        segment.P1.Y, half,         AsData(joined)};
                    MemCopy(command->Data, data, sizeof(data));
                }
                else
                {
                    const float32 data[] = {segment.P0.X, segment.P0.Y, segment.P1.X,
                                            segment.P1.Y, half,         AsData(joined)};
                    MemCopy(command->Data, data, sizeof(data));
                }
            }
        }
        for (usize index = 1; index < contour.Segments.Count(); ++index)
        {
            join(contour.Segments[index].P0 + origin, contour.Segments[index - 1].EndTangent(),
                 contour.Segments[index].StartTangent());
        }
        if (contour.Closed)
        {
            join(contour.Segments[0].P0 + origin, contour.Segments.Last().EndTangent(),
                 contour.Segments[0].StartTangent());
        }
        else
        {
            cap(contour.Segments[0].P0 + origin, -contour.Segments[0].StartTangent());
            cap(contour.Segments.Last().P1 + origin, contour.Segments.Last().EndTangent());
        }
    }
}

void UIPainter::StrokePath(const UIPath &path, const UIStrokeStyle &style, const UIBrush &brush)
{
    if (_commands == nullptr || path.IsEmpty() || style.Width <= 0)
    {
        return;
    }
    const uint32 index = AddBrush(brush);
    const usize count = GetPathContours(path, _contours);
    BeginGroup();
    AddStrokePieces(_contours.AsSpan().Slice(0, count), {}, style, index);
    EndGroup();
}

void UIPainter::StrokeContours(Span<const UIPathContour> contours, Vector2 origin, const UIStrokeStyle &style,
                               const UIBrush &brush)
{
    if (_commands == nullptr || contours.IsEmpty() || style.Width <= 0)
    {
        return;
    }
    const uint32 index = AddBrush(brush);
    BeginGroup();
    AddStrokePieces(contours, origin, style, index);
    EndGroup();
}

void UIPainter::DrawLine(Vector2 from, Vector2 to, const UIStrokeStyle &style, const UIBrush &brush)
{
    if (_commands == nullptr || style.Width <= 0)
    {
        return;
    }
    // The one contour GetPathContours would make of the line, without building a path.
    if (_contours.IsEmpty())
    {
        _contours.AddDefaulted();
    }
    UIPathContour &contour = _contours[0];
    contour.Segments.Clear();
    contour.Start = from;
    contour.Closed = false;
    if ((to - from).LengthSquared() > 1e-12f)
    {
        contour.Segments.Add(UIPathSegment{from, (from + to) * 0.5f, to, false});
    }
    const uint32 index = AddBrush(brush);
    BeginGroup();
    AddStrokePieces(_contours.AsSpan().Slice(0, 1), {}, style, index);
    EndGroup();
}

void UIPainter::DrawText(const UIShapedRun &run, Vector2 origin, const UIBrush &brush)
{
    if (_commands == nullptr || !run.Face.IsValid() || run.Size <= 0)
    {
        return;
    }
    const uint32 index = AddBrush(brush);
    for (const UIShapedGlyph &glyph : run.Glyphs)
    {
        if (glyph.Bounds.IsEmpty())
        {
            continue;
        }
        const Vector2 position = origin + glyph.Position;
        const UIRect bounds(position.X + glyph.Bounds.X, position.Y + glyph.Bounds.Y, glyph.Bounds.Width,
                            glyph.Bounds.Height);
        if (UIGpuCommand *command = AddCommand(UICommandKind::Glyph, bounds, AntialiasMargin, index))
        {
            const float32 data[] = {AsData(run.Face.Id),
                                    AsData(glyph.GlyphId),
                                    position.X,
                                    position.Y,
                                    run.Size,
                                    glyph.Bounds.X,
                                    glyph.Bounds.Y,
                                    glyph.Bounds.X + glyph.Bounds.Width,
                                    glyph.Bounds.Y + glyph.Bounds.Height};
            MemCopy(command->Data, data, sizeof(data));
        }
    }
}

void UIPainter::DrawImage(UIImage image, const UIRect &rect, const UIImageDraw &draw)
{
    if (!image.IsValid() || rect.IsEmpty())
    {
        return;
    }
    UICommandFlags flags = image.IsFrameSlot ? UICommandFlags::FrameSlot : UICommandFlags::None;
    if (draw.Sampling == UISampling::Point)
    {
        flags |= UICommandFlags::PointSampling;
    }
    if (UIGpuCommand *command =
            AddCommand(UICommandKind::Image, rect, AntialiasMargin, AddBrush(UIBrush(draw.Tint)), flags))
    {
        const float32 radius = Math::Clamp(draw.CornerRadius, 0.0f, Min(rect.Width, rect.Height) * 0.5f);
        const float32 data[] = {rect.X,
                                rect.Y,
                                rect.X + rect.Width,
                                rect.Y + rect.Height,
                                draw.Uv.X,
                                draw.Uv.Y,
                                draw.Uv.X + draw.Uv.Width,
                                draw.Uv.Y + draw.Uv.Height,
                                radius,
                                AsData(image.Value),
                                AsData(static_cast<uint32>(image.Range))};
        MemCopy(command->Data, data, sizeof(data));
    }
}

void UIPainter::PushClip(const UIRect &rect, const UICornerRadii &radii)
{
    if (_commands == nullptr)
    {
        return;
    }
    const ClipState &parent = _clips.Last();
    const UIRect device = GetTransform().TransformBounds(rect).Intersect(parent.DeviceBounds);
    const UICornerRadii clamped = ClampRadii(radii, rect);
    const bool shaped = !clamped.IsZero() || !GetTransform().IsAxisAligned();
    const UIGpuClip &parentClip = _commands->Clips[parent.Index];
    UIGpuClip clip;
    clip.DeviceRect[0] = device.X;
    clip.DeviceRect[1] = device.Y;
    clip.DeviceRect[2] = device.X + device.Width;
    clip.DeviceRect[3] = device.Y + device.Height;
    clip.Rect[0] = rect.X;
    clip.Rect[1] = rect.Y;
    clip.Rect[2] = rect.X + rect.Width;
    clip.Rect[3] = rect.Y + rect.Height;
    clip.Radii[0] = clamped.TopLeft;
    clip.Radii[1] = clamped.TopRight;
    clip.Radii[2] = clamped.BottomRight;
    clip.Radii[3] = clamped.BottomLeft;
    clip.Transform = shaped ? AddTransform() : 0;
    clip.Parent = parentClip.Shaped != 0 ? parent.Index : parentClip.Parent;
    clip.Shaped = shaped ? 1 : 0;
    _commands->Clips.Add(clip);
    _clips.Add({static_cast<uint32>(_commands->Clips.Count() - 1), device});
}

void UIPainter::PopClip()
{
    if (_clips.Count() > 1)
    {
        _clips.Pop();
    }
}

void UIPainter::PushTransform(const UITransform &transform)
{
    if (_commands == nullptr)
    {
        return;
    }
    _transforms.Add(transform.Then(GetTransform()));
    _transformIndices.Add(~0u);
}

void UIPainter::PopTransform()
{
    if (_transforms.Count() > 1)
    {
        _transforms.Pop();
        _transformIndices.Pop();
    }
}

void UIPainter::PushLayer(float32 opacity, float32 blur)
{
    PushLayer(UILayer{opacity, blur, 0});
}

void UIPainter::PushLayer(const UILayer &layer)
{
    if (_commands == nullptr)
    {
        return;
    }
    const float32 scale = Math::Sqrt(Math::Abs(GetTransform().GetDeterminant()));
    const auto canvasIndex = static_cast<uint32>(_commands->Canvases.Count());
    LayerState state;
    state.Canvas = canvasIndex;
    if (layer.BackdropBlur > 0)
    {
        state.BackdropCommand = static_cast<uint32>(GetCanvas().Commands.Count());
        AddDeviceCommand(UICommandKind::Backdrop, {}, AddBrush(LinearColor::White));
    }
    state.ParentCommand = static_cast<uint32>(GetCanvas().Commands.Count());
    UIBrush tint(LinearColor(1, 1, 1, Math::Saturate(layer.Opacity)));
    AddDeviceCommand(UICommandKind::Canvas, {}, AddBrush(tint));
    UICanvas &canvas = _commands->AddCanvas();
    canvas.Parent = _layers.Last().Canvas;
    canvas.Opacity = Math::Saturate(layer.Opacity);
    canvas.Blur = Max(layer.Blur, 0.0f) * scale;
    canvas.BackdropBlur = Max(layer.BackdropBlur, 0.0f) * scale;
    _layers.Add(state);
}

void UIPainter::PopLayer()
{
    if (_layers.Count() <= 1)
    {
        return;
    }
    const LayerState state = _layers.Pop();
    UICanvas &canvas = _commands->Canvases[state.Canvas];
    UIRect bounds = canvas.Bounds;
    if (!bounds.IsEmpty())
    {
        bounds = bounds.Inflate(Math::Ceil(canvas.Blur * 3)).Intersect(_clips.Last().DeviceBounds);
    }
    bounds = UIRect::FromCorners({Math::Floor(bounds.X), Math::Floor(bounds.Y)},
                                 {Math::Ceil(bounds.X + bounds.Width), Math::Ceil(bounds.Y + bounds.Height)});
    canvas.Bounds = bounds;
    UICanvas &parent = GetCanvas();
    const auto patch = [&](uint32 index) {
        UIGpuCommand &command = parent.Commands[index];
        command.Bounds[0] = bounds.X;
        command.Bounds[1] = bounds.Y;
        command.Bounds[2] = bounds.X + bounds.Width;
        command.Bounds[3] = bounds.Y + bounds.Height;
        command.Data[0] = bounds.X;
        command.Data[1] = bounds.Y;
        command.Data[2] = bounds.X + bounds.Width;
        command.Data[3] = bounds.Y + bounds.Height;
        command.Data[4] = AsData(state.Canvas);
    };
    patch(state.ParentCommand);
    if (state.BackdropCommand != ~0u)
    {
        patch(state.BackdropCommand);
    }
    if (_layers.Count() > 1 && !bounds.IsEmpty())
    {
        parent.Bounds = parent.Bounds.Union(bounds);
    }
}
} // namespace Marlin
