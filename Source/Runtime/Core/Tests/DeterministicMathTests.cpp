#include "Testing/Testing.h"

#include "Math/DeterministicMath.h"
#include "Templates/Bit.h"

namespace Marlin::Tests
{
namespace
{
/// Whether `actual` is within `ulps` units in the last place of `expected`, or both are NaN.
bool Close(float64 actual, float64 expected, int64 ulps = 4)
{
    if (actual != actual || expected != expected)
    {
        return actual != actual && expected != expected;
    }
    if (actual == expected)
    {
        return true;
    }
    const int64 a = BitCast<int64>(actual);
    const int64 b = BitCast<int64>(expected);
    if ((a < 0) != (b < 0))
    {
        return false;
    }
    const int64 distance = a > b ? a - b : b - a;
    return distance <= ulps;
}

template <typename TFunction, typename TReference>
int64 CheckRange(float64 low, float64 high, int32 steps, TFunction &&function, TReference &&reference, int64 ulps,
                 float64 &worst)
{
    int64 failures = 0;
    for (int32 i = 0; i <= steps; ++i)
    {
        const float64 x = low + (high - low) * i / steps;
        const float64 actual = function(x);
        const float64 expected = reference(x);
        if (!Close(actual, expected, ulps))
        {
            ++failures;
            worst = x;
        }
    }
    return failures;
}
} // namespace

MARLIN_TEST(DeterministicMath, MatchesTheReferenceWithinAFewUlps)
{
    struct Case
    {
        const char *Name;
        float64 (*Function)(float64);
        float64 (*Reference)(float64);
        float64 Low;
        float64 High;
        int64 Ulps;
    };
    const Case cases[] = {
        {"sin", [](float64 x) { return DeterministicMath::Sin(x); }, [](float64 x) { return __builtin_sin(x); }, -100,
         100, 2},
        {"cos", [](float64 x) { return DeterministicMath::Cos(x); }, [](float64 x) { return __builtin_cos(x); }, -100,
         100, 2},
        {"tan", [](float64 x) { return DeterministicMath::Tan(x); }, [](float64 x) { return __builtin_tan(x); }, -1.5,
         1.5, 4},
        {"asin", [](float64 x) { return DeterministicMath::Asin(x); }, [](float64 x) { return __builtin_asin(x); },
         -0.999, 0.999, 4},
        {"acos", [](float64 x) { return DeterministicMath::Acos(x); }, [](float64 x) { return __builtin_acos(x); },
         -0.999, 0.999, 4},
        {"atan", [](float64 x) { return DeterministicMath::Atan(x); }, [](float64 x) { return __builtin_atan(x); }, -50,
         50, 2},
        {"exp", [](float64 x) { return DeterministicMath::Exp(x); }, [](float64 x) { return __builtin_exp(x); }, -700,
         700, 2},
        {"exp2", [](float64 x) { return DeterministicMath::Exp2(x); }, [](float64 x) { return __builtin_exp2(x); },
         -1000, 1000, 2},
        {"log", [](float64 x) { return DeterministicMath::Log(x); }, [](float64 x) { return __builtin_log(x); }, 1e-6,
         1e6, 2},
        {"log2", [](float64 x) { return DeterministicMath::Log2(x); }, [](float64 x) { return __builtin_log2(x); },
         1e-6, 1e6, 2},
        {"sinh", [](float64 x) { return DeterministicMath::Sinh(x); }, [](float64 x) { return __builtin_sinh(x); },
         -20, 20, 4},
        {"cosh", [](float64 x) { return DeterministicMath::Cosh(x); }, [](float64 x) { return __builtin_cosh(x); },
         -20, 20, 4},
        {"tanh", [](float64 x) { return DeterministicMath::Tanh(x); }, [](float64 x) { return __builtin_tanh(x); },
         -20, 20, 4},
    };
    for (const Case &entry : cases)
    {
        float64 worst = 0;
        const int64 failures = CheckRange(entry.Low, entry.High, 20011, entry.Function, entry.Reference, entry.Ulps, worst);
        MARLIN_EXPECTF(failures == 0, "{0}: {1} values off by more than {2} ulps, e.g. {3}", entry.Name, failures,
                       entry.Ulps, worst);
    }
    float64 worst = 0;
    const int64 failures = CheckRange(
        0.01, 20, 4001, [](float64 y) { return DeterministicMath::Pow(1.7, y); },
        [](float64 y) { return __builtin_pow(1.7, y); }, 4, worst);
    MARLIN_EXPECTF(failures == 0, "pow: {0} values off, e.g. {1}", failures, worst);
    for (const float64 y : {-3.5, -0.75, 0.25, 0.6, 2.0, 7.25})
    {
        for (const float64 x : {-2.0, 0.6, 1.4, 4.0})
        {
            MARLIN_EXPECTF(DeterministicMath::Atan2(y, x) == DeterministicMath::Atan2(y, x) &&
                               Close(DeterministicMath::Atan2(y, x), __builtin_atan2(y, x), 2),
                           "atan2({0}, {1})", y, x);
        }
    }
}

MARLIN_TEST(DeterministicMath, HandlesSpecialValues)
{
    const float64 infinity = __builtin_inf();
    const float64 nan = __builtin_nan("");
    MARLIN_EXPECT(DeterministicMath::Sin(infinity) != DeterministicMath::Sin(infinity));
    MARLIN_EXPECT(DeterministicMath::Sin(0.0) == 0.0 && DeterministicMath::Cos(0.0) == 1.0);
    MARLIN_EXPECT(DeterministicMath::Exp(1000.0) == infinity && DeterministicMath::Exp(-1000.0) == 0.0);
    MARLIN_EXPECT(DeterministicMath::Exp(0.0) == 1.0 && DeterministicMath::Log(1.0) == 0.0);
    MARLIN_EXPECT(DeterministicMath::Log(0.0) == -infinity && DeterministicMath::Log(-1.0) != DeterministicMath::Log(-1.0));
    MARLIN_EXPECT(DeterministicMath::Log2(8.0) == 3.0 && DeterministicMath::Exp2(10.0) == 1024.0);
    MARLIN_EXPECT(DeterministicMath::Pow(2.0, 10.0) == 1024.0 && DeterministicMath::Pow(-2.0, 3.0) == -8.0);
    MARLIN_EXPECT(DeterministicMath::Pow(-2.0, 0.5) != DeterministicMath::Pow(-2.0, 0.5));
    MARLIN_EXPECT(DeterministicMath::Pow(nan, 0.0) == 1.0 && DeterministicMath::Pow(0.0, -1.0) == infinity);
    MARLIN_EXPECT(DeterministicMath::Pow(0.5, infinity) == 0.0 && DeterministicMath::Pow(-1.0, infinity) == 1.0);
    MARLIN_EXPECT(DeterministicMath::Atan(infinity) == DeterministicMath::Atan2(1.0, 0.0));
    MARLIN_EXPECT(DeterministicMath::Tanh(100.0) == 1.0 && DeterministicMath::Tanh(-100.0) == -1.0);
    MARLIN_EXPECT(DeterministicMath::Asin(2.0) != DeterministicMath::Asin(2.0));
    MARLIN_EXPECT(DeterministicMath::Sin(1e9) == DeterministicMath::Sin(1e9) && DeterministicMath::Sin(1e9) <= 1.0);
    MARLIN_EXPECT(DeterministicMath::Sin(1.0f) == static_cast<float32>(DeterministicMath::Sin(1.0)));
    MARLIN_EXPECT(DeterministicMath::Scale(1.0, -1074) == 4.9406564584124654e-324);
}
} // namespace Marlin::Tests
