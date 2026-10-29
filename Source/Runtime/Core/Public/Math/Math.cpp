#include "Math/Bounds.h"
#include "Math/Matrix.h"
#include "Math/Vector.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
template <typename T>
void FormatComponentsImpl(StringWriter &output, const T *components, uint32 count, StringView spec)
{
    output.Append('(');
    for (uint32 i = 0; i < count; ++i)
    {
        if (i != 0)
        {
            output.Append(", ");
        }
        const Private::FormatArg arg = Private::MakeFormatArg(components[i]);
        StringBuilder<16> placeholder;
        placeholder.Append("{0");
        if (!spec.IsEmpty())
        {
            placeholder.Append(':').Append(spec);
        }
        placeholder.Append('}');
        Private::FormatArgs(output, placeholder.AsView(), &arg, 1);
    }
    output.Append(')');
}
} // namespace

void FormatComponents(StringWriter &output, const float32 *components, uint32 count, StringView spec)
{
    FormatComponentsImpl(output, components, count, spec);
}

void FormatComponents(StringWriter &output, const float64 *components, uint32 count, StringView spec)
{
    FormatComponentsImpl(output, components, count, spec);
}

void FormatComponents(StringWriter &output, const int32 *components, uint32 count, StringView spec)
{
    FormatComponentsImpl(output, components, count, spec);
}

float32 Matrix3x4::Determinant() const
{
    const Vector4 &a = Rows[0];
    const Vector4 &b = Rows[1];
    const Vector4 &c = Rows[2];
    return a.X * (b.Y * c.Z - b.Z * c.Y) - a.Y * (b.X * c.Z - b.Z * c.X) + a.Z * (b.X * c.Y - b.Y * c.X);
}

Matrix3x4 Matrix3x4::Inverse() const
{
    const float32 determinant = Determinant();
    if (Math::Abs(determinant) < 1e-12f)
    {
        return Identity;
    }
    const float32 inverse = 1 / determinant;
    const Vector4 &a = Rows[0];
    const Vector4 &b = Rows[1];
    const Vector4 &c = Rows[2];
    Matrix3x4 result;
    result.Rows[0] = {(b.Y * c.Z - b.Z * c.Y) * inverse, (a.Z * c.Y - a.Y * c.Z) * inverse,
                      (a.Y * b.Z - a.Z * b.Y) * inverse, 0};
    result.Rows[1] = {(b.Z * c.X - b.X * c.Z) * inverse, (a.X * c.Z - a.Z * c.X) * inverse,
                      (a.Z * b.X - a.X * b.Z) * inverse, 0};
    result.Rows[2] = {(b.X * c.Y - b.Y * c.X) * inverse, (a.Y * c.X - a.X * c.Y) * inverse,
                      (a.X * b.Y - a.Y * b.X) * inverse, 0};
    const Vector3 translation = result.TransformVector(GetTranslation());
    result.Rows[0].W = -translation.X;
    result.Rows[1].W = -translation.Y;
    result.Rows[2].W = -translation.Z;
    return result;
}

bool Matrix3x4::IsNearlyEqual(const Matrix3x4 &other, float32 tolerance) const
{
    return Rows[0].IsNearlyEqual(other.Rows[0], tolerance) && Rows[1].IsNearlyEqual(other.Rows[1], tolerance) &&
           Rows[2].IsNearlyEqual(other.Rows[2], tolerance);
}

Matrix4x4 Matrix4x4::Perspective(float32 verticalFov, float32 aspect, float32 nearZ, float32 farZ)
{
    const float32 scale = 1 / Math::Tan(verticalFov * 0.5f);
    const float32 range = nearZ / (farZ - nearZ);
    return {{scale / aspect, 0, 0, 0}, {0, scale, 0, 0}, {0, 0, range, farZ * range}, {0, 0, -1, 0}};
}

Matrix4x4 Matrix4x4::PerspectiveInfinite(float32 verticalFov, float32 aspect, float32 nearZ)
{
    const float32 scale = 1 / Math::Tan(verticalFov * 0.5f);
    return {{scale / aspect, 0, 0, 0}, {0, scale, 0, 0}, {0, 0, 0, nearZ}, {0, 0, -1, 0}};
}

Matrix4x4 Matrix4x4::Orthographic(float32 width, float32 height, float32 nearZ, float32 farZ)
{
    const float32 range = 1 / (farZ - nearZ);
    return {{2 / width, 0, 0, 0}, {0, 2 / height, 0, 0}, {0, 0, range, farZ * range}, {0, 0, 0, 1}};
}

Matrix4x4 Matrix4x4::LookAt(Vector3 eye, Vector3 target, Vector3 up)
{
    const Vector3 forward = (target - eye).Normalized();
    const Vector3 right = Vector3::Cross(forward, up).Normalized();
    const Vector3 cameraUp = Vector3::Cross(right, forward);
    return {Vector4(right, -Vector3::Dot(right, eye)),
            Vector4(cameraUp, -Vector3::Dot(cameraUp, eye)),
            Vector4(-forward, Vector3::Dot(forward, eye)),
            {0, 0, 0, 1}};
}

float32 Matrix4x4::Determinant() const
{
    const float32 *m = &Rows[0].X;
    const float32 s0 = m[0] * m[5] - m[1] * m[4];
    const float32 s1 = m[0] * m[6] - m[2] * m[4];
    const float32 s2 = m[0] * m[7] - m[3] * m[4];
    const float32 s3 = m[1] * m[6] - m[2] * m[5];
    const float32 s4 = m[1] * m[7] - m[3] * m[5];
    const float32 s5 = m[2] * m[7] - m[3] * m[6];
    const float32 c5 = m[10] * m[15] - m[11] * m[14];
    const float32 c4 = m[9] * m[15] - m[11] * m[13];
    const float32 c3 = m[9] * m[14] - m[10] * m[13];
    const float32 c2 = m[8] * m[15] - m[11] * m[12];
    const float32 c1 = m[8] * m[14] - m[10] * m[12];
    const float32 c0 = m[8] * m[13] - m[9] * m[12];
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

Matrix4x4 Matrix4x4::Inverse() const
{
    const float32 *m = &Rows[0].X;
    const float32 s0 = m[0] * m[5] - m[1] * m[4];
    const float32 s1 = m[0] * m[6] - m[2] * m[4];
    const float32 s2 = m[0] * m[7] - m[3] * m[4];
    const float32 s3 = m[1] * m[6] - m[2] * m[5];
    const float32 s4 = m[1] * m[7] - m[3] * m[5];
    const float32 s5 = m[2] * m[7] - m[3] * m[6];
    const float32 c5 = m[10] * m[15] - m[11] * m[14];
    const float32 c4 = m[9] * m[15] - m[11] * m[13];
    const float32 c3 = m[9] * m[14] - m[10] * m[13];
    const float32 c2 = m[8] * m[15] - m[11] * m[12];
    const float32 c1 = m[8] * m[14] - m[10] * m[12];
    const float32 c0 = m[8] * m[13] - m[9] * m[12];
    const float32 determinant = s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
    if (Math::Abs(determinant) < 1e-20f)
    {
        return Identity;
    }
    const float32 k = 1 / determinant;
    return {{(m[5] * c5 - m[6] * c4 + m[7] * c3) * k, (-m[1] * c5 + m[2] * c4 - m[3] * c3) * k,
             (m[13] * s5 - m[14] * s4 + m[15] * s3) * k, (-m[9] * s5 + m[10] * s4 - m[11] * s3) * k},
            {(-m[4] * c5 + m[6] * c2 - m[7] * c1) * k, (m[0] * c5 - m[2] * c2 + m[3] * c1) * k,
             (-m[12] * s5 + m[14] * s2 - m[15] * s1) * k, (m[8] * s5 - m[10] * s2 + m[11] * s1) * k},
            {(m[4] * c4 - m[5] * c2 + m[7] * c0) * k, (-m[0] * c4 + m[1] * c2 - m[3] * c0) * k,
             (m[12] * s4 - m[13] * s2 + m[15] * s0) * k, (-m[8] * s4 + m[9] * s2 - m[11] * s0) * k},
            {(-m[4] * c3 + m[5] * c1 - m[6] * c0) * k, (m[0] * c3 - m[1] * c1 + m[2] * c0) * k,
             (-m[12] * s3 + m[13] * s1 - m[14] * s0) * k, (m[8] * s3 - m[9] * s1 + m[10] * s0) * k}};
}

bool Matrix4x4::IsNearlyEqual(const Matrix4x4 &other, float32 tolerance) const
{
    for (uint32 i = 0; i < 4; ++i)
    {
        if (!Rows[i].IsNearlyEqual(other.Rows[i], tolerance))
        {
            return false;
        }
    }
    return true;
}

Frustum Frustum::FromViewProjection(const Matrix4x4 &viewProjection)
{
    const Vector4 *rows = viewProjection.Rows;
    Frustum frustum;
    frustum.Planes[Left] = Plane::Normalize(rows[3] + rows[0]);
    frustum.Planes[Right] = Plane::Normalize(rows[3] - rows[0]);
    frustum.Planes[Bottom] = Plane::Normalize(rows[3] + rows[1]);
    frustum.Planes[Top] = Plane::Normalize(rows[3] - rows[1]);
    frustum.Planes[Near] = Plane::Normalize(rows[3] - rows[2]);
    if (rows[2].XYZ().LengthSquared() < 1e-12f)
    {
        frustum.Planes[Far] = {Vector3::Up, 1e30f};
    }
    else
    {
        frustum.Planes[Far] = Plane::Normalize(rows[2]);
    }
    return frustum;
}

bool Frustum::Contains(Vector3 point) const
{
    for (const Plane &plane : Planes)
    {
        if (plane.SignedDistance(point) < 0)
        {
            return false;
        }
    }
    return true;
}

bool Frustum::Intersects(const Sphere &sphere) const
{
    for (const Plane &plane : Planes)
    {
        if (plane.SignedDistance(sphere.Center) < -sphere.Radius)
        {
            return false;
        }
    }
    return true;
}

bool Frustum::Intersects(const Box &box) const
{
    const Vector3 center = box.GetCenter();
    const Vector3 extent = box.GetExtent();
    for (const Plane &plane : Planes)
    {
        const float32 radius = Vector3::Dot(Vector3::Abs(plane.Normal), extent);
        if (plane.SignedDistance(center) < -radius)
        {
            return false;
        }
    }
    return true;
}

float32 Ray::Intersect(const Box &box) const
{
    float32 nearest = 0;
    float32 farthest = 1e30f;
    for (uint32 axis = 0; axis < 3; ++axis)
    {
        const float32 origin = Origin[axis];
        const float32 direction = Direction[axis];
        const float32 low = box.Min[axis];
        const float32 high = box.Max[axis];
        if (Math::Abs(direction) < 1e-12f)
        {
            if (origin < low || origin > high)
            {
                return -1;
            }
            continue;
        }
        const float32 inverse = 1 / direction;
        float32 t0 = (low - origin) * inverse;
        float32 t1 = (high - origin) * inverse;
        if (t0 > t1)
        {
            const float32 swap = t0;
            t0 = t1;
            t1 = swap;
        }
        nearest = t0 > nearest ? t0 : nearest;
        farthest = t1 < farthest ? t1 : farthest;
        if (nearest > farthest)
        {
            return -1;
        }
    }
    return nearest;
}

float32 Ray::Intersect(const Sphere &sphere) const
{
    const Vector3 offset = Origin - sphere.Center;
    const float32 b = Vector3::Dot(offset, Direction);
    const float32 c = offset.LengthSquared() - sphere.Radius * sphere.Radius;
    if (c <= 0)
    {
        return 0;
    }
    const float32 discriminant = b * b - c;
    if (b > 0 || discriminant < 0)
    {
        return -1;
    }
    return -b - Math::Sqrt(discriminant);
}

float32 Ray::Intersect(const Plane &plane) const
{
    const float32 denominator = Vector3::Dot(plane.Normal, Direction);
    if (Math::Abs(denominator) < 1e-12f)
    {
        return -1;
    }
    const float32 distance = -plane.SignedDistance(Origin) / denominator;
    return distance >= 0 ? distance : -1;
}
} // namespace Marlin
