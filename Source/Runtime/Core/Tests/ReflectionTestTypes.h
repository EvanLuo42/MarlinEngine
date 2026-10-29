#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Memory/UniquePtr.h"
#include "String/Name.h"
#include "Templates/Optional.h"
#include "ReflectionTestTypes.gen.h"

namespace Marlin::Tests
{
/// How a body moves.
MENUM()
enum class TestMotion : uint8
{
    /// Never moves.
    Static = 0,
    /// Moved by forces, as [`TestPoint`] and [the docs](`TestMotion`) say, `a * b`.
    ///
    /// Only the first paragraph becomes the Tooltip.
    Dynamic = 4,
};

MENUM(Flags)
enum class TestFlags : uint8
{
    None = 0,
    Red = 1 << 0,
    Green = 1 << 1,
    Blue = 1 << 2,
};

MSTRUCT(Tuple)
struct TestPoint
{
    MGENERATED_BODY()

    MPROPERTY(Range = (0, 10), Unit = Meter)
    float32 X = 0;
    /// Up.
    MPROPERTY()
    float32 Y[2] = {};
};

MINTERFACE()
class ITestCounter
{
    MGENERATED_BODY()
public:
    virtual ~ITestCounter() = default;
    MFUNCTION(ScriptCallable)
    virtual int32 Count() const = 0;
};

MCLASS(Version = 3)
class TestBase : public ITestCounter
{
    MGENERATED_BODY()
public:
    ~TestBase() override = default;

    MPROPERTY(ConsoleVariable = "test.Speed", Flags = Cheat | Scalability)
    static float32 Speed;

    MFUNCTION()
    float32 Move(const TestPoint &to, float32 speed = 1.5f)
    {
        _points.Add(to);
        return speed * 2;
    }

    int32 Count() const override
    {
        return static_cast<int32>(_points.Count());
    }

    /// Doubles a number.
    MFUNCTION(ConsoleCommand = "test.Twice")
    static int32 Twice(int32 value)
    {
        return value * 2;
    }

private:
    MPROPERTY(Name = "text \"quoted\"")
    List<TestPoint> _points;
};

MCLASS(Version = 2)
class TestDerived final : public TestBase
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    Optional<int32> Maybe;
    MPROPERTY()
    UniquePtr<TestBase> Child;
    MPROPERTY()
    HashMap<Name, float32> Weights;
    MPROPERTY()
    TestMotion Motion = TestMotion::Dynamic;
    MPROPERTY()
    TestFlags Colors = TestFlags::None;
};
} // namespace Marlin::Tests
