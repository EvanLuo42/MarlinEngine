#include "UIPainter/UIOutline.h"

#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "Templates/Utility.h"

namespace Marlin
{
namespace
{
Vector2 NormalizeOr(Vector2 vector)
{
    const float32 length = vector.Length();
    return length > 1e-12f ? vector / length : Vector2(1, 0);
}
} // namespace

Vector2 UIPathSegment::StartTangent() const
{
    return NormalizeOr(Quad && (Control - P0).LengthSquared() > 1e-12f ? Control - P0 : P1 - P0);
}

Vector2 UIPathSegment::EndTangent() const
{
    return NormalizeOr(Quad && (P1 - Control).LengthSquared() > 1e-12f ? P1 - Control : P1 - P0);
}

usize GetPathContours(const UIPath &path, List<UIPathContour> &contours)
{
    const Span<const UIPath::Verb> verbs = path.GetVerbs();
    const Span<const Vector2> points = path.GetPoints();
    usize point = 0;
    Vector2 current;
    usize count = 0;
    for (const UIPath::Verb verb : verbs)
    {
        switch (verb)
        {
        case UIPath::Verb::Move:
            current = points[point++];
            if (count == contours.Count())
            {
                contours.AddDefaulted();
            }
            contours[count].Segments.Clear();
            contours[count].Start = current;
            contours[count].Closed = false;
            ++count;
            break;
        case UIPath::Verb::Line: {
            const Vector2 next = points[point++];
            if ((next - current).LengthSquared() > 1e-12f)
            {
                contours[count - 1].Segments.Add(UIPathSegment{current, (current + next) * 0.5f, next, false});
            }
            current = next;
            break;
        }
        case UIPath::Verb::Quad: {
            const Vector2 control = points[point];
            const Vector2 next = points[point + 1];
            point += 2;
            if ((next - current).LengthSquared() > 1e-12f || (control - current).LengthSquared() > 1e-12f)
            {
                contours[count - 1].Segments.Add(UIPathSegment{current, control, next, true});
            }
            current = next;
            break;
        }
        case UIPath::Verb::Close:
            contours[count - 1].Closed = true;
            if ((contours[count - 1].Start - current).LengthSquared() > 1e-12f)
            {
                const Vector2 start = contours[count - 1].Start;
                contours[count - 1].Segments.Add(UIPathSegment{current, (current + start) * 0.5f, start, false});
            }
            current = contours[count - 1].Start;
            break;
        }
    }
    return count;
}

uint32 AppendOutline(const UIPath &path, List<UIGpuCurve> &curvesOut, List<uint32> &words)
{
    List<UIPathContour> contours;
    const usize contourCount = GetPathContours(path, contours);
    const auto curveOffset = static_cast<uint32>(curvesOut.Count());
    const auto addCurve = [&curvesOut](Vector2 p0, Vector2 control, Vector2 p1) {
        UIGpuCurve &curve = curvesOut.AddDefaulted();
        const float32 points[] = {p0.X, p0.Y, control.X, control.Y, p1.X, p1.Y};
        MemCopy(curve.Points, points, sizeof(points));
    };
    for (UIPathContour &contour : contours.AsSpan().Slice(0, contourCount))
    {
        if (!contour.Closed && !contour.Segments.IsEmpty())
        {
            const Vector2 end = contour.Segments.Last().P1;
            if ((end - contour.Start).LengthSquared() > 1e-12f)
            {
                contour.Segments.Add(UIPathSegment{end, (end + contour.Start) * 0.5f, contour.Start, false});
            }
        }
        // The coverage shader solves each curve along its rays as monotonic in x and in y, so quadratics are split
        // at their extrema.
        for (const UIPathSegment &segment : contour.Segments)
        {
            float32 splits[2] = {};
            uint32 splitCount = 0;
            for (uint32 axis = 0; segment.Quad && axis < 2; ++axis)
            {
                const float32 a = axis == 0 ? segment.P0.X : segment.P0.Y;
                const float32 b = axis == 0 ? segment.Control.X : segment.Control.Y;
                const float32 c = axis == 0 ? segment.P1.X : segment.P1.Y;
                const float32 denominator = a - 2 * b + c;
                const float32 t = Math::Abs(denominator) > 1e-12f ? (a - b) / denominator : 0;
                if (t > 1e-4f && t < 1 - 1e-4f)
                {
                    splits[splitCount++] = t;
                }
            }
            if (splitCount == 2 && splits[0] > splits[1])
            {
                Swap(splits[0], splits[1]);
            }
            Vector2 p0 = segment.P0;
            Vector2 control = segment.Control;
            float32 done = 0;
            for (uint32 index = 0; index < splitCount; ++index)
            {
                const float32 t = (splits[index] - done) / (1 - done);
                const Vector2 first = p0 + (control - p0) * t;
                const Vector2 second = control + (segment.P1 - control) * t;
                const Vector2 middle = first + (second - first) * t;
                addCurve(p0, first, middle);
                p0 = middle;
                control = second;
                done = splits[index];
            }
            addCurve(p0, control, segment.P1);
        }
    }
    const auto curveCount = static_cast<uint32>(curvesOut.Count()) - curveOffset;
    if (curveCount == 0)
    {
        return ~0u;
    }
    const Span<const UIGpuCurve> curves(curvesOut.Data() + curveOffset, curveCount);
    float32 minX = curves[0].Points[0];
    float32 minY = curves[0].Points[1];
    float32 maxX = minX;
    float32 maxY = minY;
    for (const UIGpuCurve &curve : curves)
    {
        for (uint32 point = 0; point < 3; ++point)
        {
            minX = Min(minX, curve.Points[point * 2]);
            maxX = Max(maxX, curve.Points[point * 2]);
            minY = Min(minY, curve.Points[point * 2 + 1]);
            maxY = Max(maxY, curve.Points[point * 2 + 1]);
        }
    }
    const uint32 bands = Math::Clamp((curveCount + 3) / 4, 1u, 16u);
    const auto offset = static_cast<uint32>(words.Count());
    UIGpuOutlineHeader header;
    header.CurveOffset = curveOffset;
    header.CurveCount = curveCount;
    header.BandsY = bands;
    header.BandsX = bands;
    header.Bounds[0] = minX;
    header.Bounds[1] = minY;
    header.Bounds[2] = maxX;
    header.Bounds[3] = maxY;
    words.AddRange(Span<const uint32>(reinterpret_cast<const uint32 *>(&header), sizeof(header) / sizeof(uint32)));
    const auto rangesStart = static_cast<uint32>(words.Count());
    for (uint32 index = 0; index < bands * 4; ++index)
    {
        words.Add(0);
    }
    for (uint32 axis = 0; axis < 2; ++axis)
    {
        const float32 low = axis == 0 ? minY : minX;
        const float32 size = Max((axis == 0 ? maxY - minY : maxX - minX) / static_cast<float32>(bands), 1e-6f);
        for (uint32 band = 0; band < bands; ++band)
        {
            const float32 bandLow = low + size * static_cast<float32>(band) - size * 0.01f;
            const float32 bandHigh = low + size * static_cast<float32>(band + 1) + size * 0.01f;
            const auto start = static_cast<uint32>(words.Count());
            for (uint32 index = 0; index < curveCount; ++index)
            {
                const float32 *points = curves[index].Points;
                const float32 a = points[axis == 0 ? 1 : 0];
                const float32 b = points[axis == 0 ? 3 : 2];
                const float32 c = points[axis == 0 ? 5 : 4];
                if (Max(a, Max(b, c)) >= bandLow && Min(a, Min(b, c)) <= bandHigh)
                {
                    words.Add(index);
                }
            }
            const uint32 slot = rangesStart + (axis * bands + band) * 2;
            words[slot] = start;
            words[slot + 1] = static_cast<uint32>(words.Count()) - start;
        }
    }
    return offset;
}

bool UIEncodedOutline::Encode(const UIPath &path)
{
    Curves.Clear();
    Words.Clear();
    Bounds = path.GetBounds();
    if (AppendOutline(path, Curves, Words) == ~0u)
    {
        Curves.Clear();
        Words.Clear();
        return false;
    }
    return true;
}

uint32 AppendOutline(const UIEncodedOutline &outline, List<UIGpuCurve> &curves, List<uint32> &words)
{
    if (outline.IsEmpty())
    {
        return ~0u;
    }
    const auto curveBase = static_cast<uint32>(curves.Count());
    const auto offset = static_cast<uint32>(words.Count());
    curves.AddRange(outline.Curves.AsSpan());
    words.AddRange(outline.Words.AsSpan());
    UIGpuOutlineHeader header;
    MemCopy(&header, outline.Words.Data(), sizeof(header));
    // The header's curve offset and the band ranges' word offsets are absolute; band curve lists are relative.
    words[offset] = header.CurveOffset + curveBase;
    const usize ranges = offset + sizeof(header) / sizeof(uint32);
    for (usize band = 0; band < static_cast<usize>(header.BandsX + header.BandsY); ++band)
    {
        words[ranges + band * 2] += offset;
    }
    return offset;
}
} // namespace Marlin
