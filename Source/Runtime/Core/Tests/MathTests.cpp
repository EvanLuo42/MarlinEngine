#include "Math/Bounds.h"
#include "Math/Color.h"
#include "Math/Easing.h"
#include "Math/Float16.h"
#include "Math/Matrix.h"
#include "Math/Random.h"
#include "Math/Transform.h"
#include "Math/Vector4Simd.h"
#include "Math/WorldPosition.h"
#include "String/Format.h"
#include "Templates/NumericLimits.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
static_assert(Vector3::Cross(Vector3::Right, Vector3::Up) == Vector3::Backward);
static_assert(Vector3::Dot(Vector3(1, 2, 3), Vector3(4, 5, 6)) == 32);
static_assert(Float16(1.5f).Bits == 0x3E00 && Float16::FromBits(0x3C00).ToFloat() == 1.0f);
static_assert(Color32::FromHex(0x11223344).ToHex() == 0x11223344);

Quaternion RandomRotation(Random &random)
{
    return Quaternion(static_cast<float32>(random.NextGaussian()), static_cast<float32>(random.NextGaussian()),
                      static_cast<float32>(random.NextGaussian()), static_cast<float32>(random.NextGaussian()))
        .Normalized();
}

Vector3 RandomVector(Random &random, float32 range)
{
    return {random.NextFloat(-range, range), random.NextFloat(-range, range), random.NextFloat(-range, range)};
}
} // namespace

MARLIN_TEST(Math, EasingCurvesRunFromZeroToOne)
{
    const EasingFunction curves[] = {&Easing::Linear, &Easing::EaseInCubic, &Easing::EaseOutCubic,
                                     &Easing::EaseInOutCubic, &Easing::EaseOutBack};
    for (const EasingFunction curve : curves)
    {
        MARLIN_EXPECT(Math::Abs(curve(0)) < 1e-5f && Math::Abs(curve(1) - 1) < 1e-5f);
    }
    MARLIN_EXPECT(Easing::EaseOutCubic(0.5f) > 0.5f && Easing::EaseInCubic(0.5f) < 0.5f);
    MARLIN_EXPECT(Math::Abs(Easing::EaseInOutCubic(0.5f) - 0.5f) < 1e-5f);
    // EaseOutBack overshoots before settling.
    MARLIN_EXPECT(Easing::EaseOutBack(0.7f) > 1);
}

MARLIN_TEST(Math, Vectors)
{
    {
        const Vector4Simd a(1, 2, 3, 4);
        const Vector4Simd b(5, 6, 7, 8);
        MARLIN_EXPECT(Vector4Simd::Dot4(a, b).X() == 70 && Vector4Simd::Dot3(a, b).W() == 38);
        MARLIN_EXPECT(Vector4Simd::Cross3(Vector4Simd(Vector3::Right), Vector4Simd(Vector3::Up))
                          .IsNearlyEqual(Vector4Simd(Vector3::Backward)));
        MARLIN_EXPECT(Math::IsNearlyEqual(Vector4Simd(3, 4, 0, 0).Normalized3().X(), 0.6f, 1e-5f));
        MARLIN_EXPECT(Vector4Simd::Min(a, b).ToVector4() == Vector4(1, 2, 3, 4));
        const Simd::Float4 estimate = Simd::ReciprocalSqrt(Simd::Splat(4));
        MARLIN_EXPECT(Math::IsNearlyEqual(estimate.y, 0.5f, 1e-5f));
        MARLIN_EXPECT(Simd::MoveMask(Simd::Float4{1, -1, 2, -2} < Simd::Splat(0)) == 0b1010);
        Simd::Byte16 bytes = {};
        bytes[3] = 0x80;
        bytes[15] = 0xFF;
        MARLIN_EXPECT(Simd::MoveMask(bytes) == ((1u << 3) | (1u << 15)));
    }
}

MARLIN_TEST(Math, Quaternion)
{
    Random random(99);
    {
        const Quaternion turn = Quaternion::FromAxisAngle(Vector3::Up, Math::HalfPi);
        MARLIN_EXPECT(turn.Rotate(Vector3::Right).IsNearlyEqual(Vector3::Forward, 1e-6f));
        MARLIN_EXPECT(turn.GetForward().IsNearlyEqual(-Vector3::Right, 1e-6f));
        int32 eulerErrors = 0;
        int32 matrixErrors = 0;
        for (int32 i = 0; i < 1000; ++i)
        {
            const float32 pitch = random.NextFloat(-1.5f, 1.5f);
            const float32 yaw = random.NextFloat(-3.1f, 3.1f);
            const float32 roll = random.NextFloat(-3.1f, 3.1f);
            const Quaternion rotation = Quaternion::FromEuler(pitch, yaw, roll);
            const Vector3 euler = rotation.ToEuler();
            eulerErrors += !Quaternion::FromEuler(euler.X, euler.Y, euler.Z).IsNearlyEqual(rotation, 1e-4f);

            const Quaternion q = RandomRotation(random);
            const Vector3 point = RandomVector(random, 10);
            const Vector3 scale(random.NextFloat(0.5f, 2), random.NextFloat(0.5f, 2), random.NextFloat(0.5f, 2));
            const Transform transform(RandomVector(random, 10), q, scale);
            const Matrix3x4 matrix = transform.ToMatrix();
            matrixErrors += !matrix.TransformPoint(point).IsNearlyEqual(transform.TransformPoint(point), 1e-3f);
            matrixErrors += !matrix.Inverse().TransformPoint(matrix.TransformPoint(point)).IsNearlyEqual(point, 1e-3f);
            const Transform uniform(transform.Translation, q, Vector3(scale.X));
            matrixErrors +=
                !uniform.Inverse().TransformPoint(uniform.TransformPoint(point)).IsNearlyEqual(point, 1e-3f);
            matrixErrors +=
                !transform.InverseTransformPoint(transform.TransformPoint(point)).IsNearlyEqual(point, 1e-3f);
            const Matrix4x4 full(matrix);
            matrixErrors += !(full * full.Inverse()).IsNearlyEqual(Matrix4x4::Identity, 1e-3f);
            const Transform child(RandomVector(random, 5), RandomRotation(random), Vector3(1.5f));
            const Transform parent(RandomVector(random, 5), q, Vector3(2.0f));
            matrixErrors += !(parent * child)
                                 .TransformPoint(point)
                                 .IsNearlyEqual(parent.TransformPoint(child.TransformPoint(point)), 1e-3f);
            matrixErrors += !(parent.ToMatrix() * child.ToMatrix()).IsNearlyEqual((parent * child).ToMatrix(), 1e-3f);
        }
        MARLIN_EXPECT(eulerErrors == 0);
        MARLIN_EXPECT(matrixErrors == 0);
        const Quaternion a = Quaternion::Identity;
        const Quaternion b = Quaternion::FromAxisAngle(Vector3::Up, 2.0f);
        MARLIN_EXPECT(Math::IsNearlyEqual(Quaternion::AngleBetween(a, Quaternion::Slerp(a, b, 0.25f)), 0.5f, 1e-4f));
        MARLIN_EXPECT(
            Quaternion::FromTo(Vector3::Right, Vector3::Up).Rotate(Vector3::Right).IsNearlyEqual(Vector3::Up, 1e-6f));
        MARLIN_EXPECT(Quaternion::FromTo(Vector3::Right, -Vector3::Right)
                          .Rotate(Vector3::Right)
                          .IsNearlyEqual(-Vector3::Right, 1e-5f));
    }
}

MARLIN_TEST(Math, Matrix)
{
    {
        const Matrix4x4 projection = Matrix4x4::Perspective(Math::HalfPi, 16.0f / 9.0f, 0.1f, 1000.0f);
        MARLIN_EXPECT(Math::IsNearlyEqual(projection.TransformPoint({0, 0, -0.1f}).Z, 1.0f, 1e-5f));
        MARLIN_EXPECT(Math::IsNearlyEqual(projection.TransformPoint({0, 0, -1000.0f}).Z, 0.0f, 1e-5f));
        const Matrix4x4 infinite = Matrix4x4::PerspectiveInfinite(Math::HalfPi, 1, 0.1f);
        MARLIN_EXPECT(Math::IsNearlyEqual(infinite.TransformPoint({0, 0, -0.1f}).Z, 1.0f, 1e-5f));
        const Matrix4x4 view = Matrix4x4::LookAt({0, 0, 10}, Vector3::Zero, Vector3::Up);
        MARLIN_EXPECT(view.TransformPoint(Vector3::Zero).IsNearlyEqual({0, 0, -10}, 1e-5f));
        const Matrix4x4 orthographic = Matrix4x4::Orthographic(20, 10, 1, 101);
        MARLIN_EXPECT(Math::IsNearlyEqual(orthographic.TransformPoint({10, 5, -1}).Z, 1.0f, 1e-5f) &&
                      Math::IsNearlyEqual(orthographic.TransformPoint({10, 5, -101}).X, 1.0f, 1e-5f));

        const Frustum frustum = Frustum::FromViewProjection(projection * view);
        MARLIN_EXPECT(frustum.Contains(Vector3::Zero) && !frustum.Contains({0, 0, 20}) &&
                      !frustum.Contains({100, 0, 0}));
        MARLIN_EXPECT(frustum.Intersects(Sphere({0, 0, 11}, 2)) && !frustum.Intersects(Sphere({0, 0, 20}, 2)));
        MARLIN_EXPECT(frustum.Intersects(Box({-1, -1, -1}, {1, 1, 1})) &&
                      !frustum.Intersects(Box({50, 0, 0}, {60, 1, 1})));
        const Frustum endless = Frustum::FromViewProjection(infinite * view);
        MARLIN_EXPECT(endless.Contains({0, 0, -100000}));
    }
}

MARLIN_TEST(Math, Bounds)
{
    {
        Box box;
        MARLIN_EXPECT(!box.IsValid());
        box.Add({1, 2, 3}).Add({-1, 0, 5});
        MARLIN_EXPECT(box.IsValid() && box.GetCenter() == Vector3(0, 1, 4) && box.Contains({0, 1, 4}));
        const Box moved = box.Transformed(Matrix3x4::FromTranslation({10, 0, 0}));
        MARLIN_EXPECT(moved.Min.IsNearlyEqual({9, 0, 3}) && moved.Max.IsNearlyEqual({11, 2, 5}));
        const Box rotated =
            Box({-1, -1, -1}, {1, 1, 1})
                .Transformed(Transform({}, Quaternion::FromAxisAngle(Vector3::Up, Math::Pi / 4)).ToMatrix());
        MARLIN_EXPECT(Math::IsNearlyEqual(rotated.Max.X, 1.41421356f, 1e-4f));
        const Ray ray({-5, 0.5f, 0.5f}, Vector3::Right);
        MARLIN_EXPECT(Math::IsNearlyEqual(ray.Intersect(Box({0, 0, 0}, {1, 1, 1})), 5.0f, 1e-5f));
        MARLIN_EXPECT(ray.Intersect(Box({0, 2, 0}, {1, 3, 1})) < 0);
        MARLIN_EXPECT(Math::IsNearlyEqual(ray.Intersect(Sphere({5, 0.5f, 0.5f}, 1)), 9.0f, 1e-5f));
        MARLIN_EXPECT(
            Math::IsNearlyEqual(ray.Intersect(Plane::FromPointNormal({2, 0, 0}, -Vector3::Right)), 7.0f, 1e-5f));
        MARLIN_EXPECT(Math::IsNearlyEqual(Plane::FromPoints({0, 0, 0}, {1, 0, 0}, {0, 1, 0}).Normal.Z, 1.0f));
    }
}

MARLIN_TEST(Math, Float16)
{
    {
        int32 halfErrors = 0;
        for (uint32 bits = 0; bits < 0x10000; ++bits)
        {
            const Float16 half = Float16::FromBits(static_cast<uint16>(bits));
            const float32 value = half.ToFloat();
            if (value == value)
            {
                halfErrors += Float16(value).Bits != bits;
            }
        }
        MARLIN_EXPECT(halfErrors == 0);
        MARLIN_EXPECT(Float16(65520.0f).ToFloat() == NumericLimits<float32>::Infinity() &&
                      Float16(1e-8f).ToFloat() == 0);

        const LinearColor gray = LinearColor::FromSrgb({128, 128, 128});
        MARLIN_EXPECT(Math::IsNearlyEqual(gray.R, 0.2158605f, 1e-5f) && gray.ToSrgb() == Color32(128, 128, 128));
        int32 colorErrors = 0;
        for (uint32 value = 0; value < 256; ++value)
        {
            const auto byte = static_cast<uint8>(value);
            colorErrors += LinearColor::FromSrgb({byte, byte, byte}).ToSrgb() != Color32(byte, byte, byte);
        }
        MARLIN_EXPECT(colorErrors == 0);
        const LinearColor red = LinearColor::FromHsv(0, 1, 1);
        const LinearColor cyan = LinearColor::FromHsv(180, 1, 1);
        MARLIN_EXPECT(red == LinearColor(1, 0, 0) && cyan == LinearColor(0, 1, 1));
    }
}

MARLIN_TEST(Math, WorldPosition)
{
    {
        const WorldPosition far(1e9, 2.5, -3e8);
        const WorldPosition near = far + Vector3(0.25f, 0.5f, -0.125f);
        MARLIN_EXPECT(near.ToRelative(far) == Vector3(0.25f, 0.5f, -0.125f));
        MARLIN_EXPECT(Format("{0:F1}", Vector3(1, 2.5f, -3)) == "(1.0, 2.5, -3.0)");
        MARLIN_EXPECT(Format("{0}", IntVector2(3, -4)) == "(3, -4)");
        MARLIN_EXPECT(Format("{0:F0}", far) == "(1000000000, 3, -300000000)");
    }
}
} // namespace Marlin::Tests
