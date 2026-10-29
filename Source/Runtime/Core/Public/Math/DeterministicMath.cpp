#include "Math/DeterministicMath.h"

#include "Templates/Bit.h"

#pragma clang fp contract(off)

namespace Marlin
{
namespace
{
constexpr float64 ToInt = 6755399441055744.0;
constexpr float64 Ln2Hi = 6.93147180369123816490e-01;
constexpr float64 Ln2Lo = 1.90821492927058770002e-10;
constexpr float64 InvLn2 = 1.44269504088896338700e+00;
constexpr float64 Pi = 3.14159265358979311600e+00;
constexpr float64 PiLo = 1.22464679914735317720e-16;
constexpr float64 HalfPi = 1.57079632679489655800e+00;
constexpr float64 TwoPi = 6.28318530717958623200e+00;

uint32 High(float64 x)
{
    return static_cast<uint32>(BitCast<uint64>(x) >> 32);
}

uint32 Low(float64 x)
{
    return static_cast<uint32>(BitCast<uint64>(x));
}

float64 FromWords(uint32 high, uint32 low)
{
    return BitCast<float64>((static_cast<uint64>(high) << 32) | low);
}

bool IsNan(float64 x)
{
    return x != x;
}

float64 Abs(float64 x)
{
    return BitCast<float64>(BitCast<uint64>(x) & ~(uint64{1} << 63));
}

float64 KernelSin(float64 x, float64 y, bool hasTail)
{
    constexpr float64 S1 = -1.66666666666666324348e-01;
    constexpr float64 S2 = 8.33333333332248946124e-03;
    constexpr float64 S3 = -1.98412698298579493134e-04;
    constexpr float64 S4 = 2.75573137070700676789e-06;
    constexpr float64 S5 = -2.50507602534068634195e-08;
    constexpr float64 S6 = 1.58969099521155010221e-10;
    const float64 z = x * x;
    const float64 v = z * x;
    const float64 r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
    if (!hasTail)
    {
        return x + v * (S1 + z * r);
    }
    return x - ((z * (0.5 * y - v * r) - y) - v * S1);
}

float64 KernelCos(float64 x, float64 y)
{
    constexpr float64 C1 = 4.16666666666666019037e-02;
    constexpr float64 C2 = -1.38888888888741095749e-03;
    constexpr float64 C3 = 2.48015872894767294178e-05;
    constexpr float64 C4 = -2.75573143513906633035e-07;
    constexpr float64 C5 = 2.08757232129817482790e-09;
    constexpr float64 C6 = -1.13596475577881948265e-11;
    const float64 z = x * x;
    const float64 w = z * z;
    const float64 r = z * (C1 + z * (C2 + z * C3)) + w * w * (C4 + z * (C5 + z * C6));
    const float64 hz = 0.5 * z;
    const float64 one = 1.0 - hz;
    return one + (((1.0 - one) - hz) + (z * r - x * y));
}

int32 ReducePiOver2Medium(float64 x, float64 &y0, float64 &y1)
{
    constexpr float64 InvPiOver2 = 6.36619772367581382433e-01;
    constexpr float64 PiOver2A = 1.57079632673412561417e+00;
    constexpr float64 PiOver2ATail = 6.07710050650619224932e-11;
    constexpr float64 PiOver2B = 6.07710050630396597660e-11;
    constexpr float64 PiOver2BTail = 2.02226624879595063154e-21;
    constexpr float64 PiOver2C = 2.02226624871116645580e-21;
    constexpr float64 PiOver2CTail = 8.47842766036889956997e-32;
    const float64 fn = x * InvPiOver2 + ToInt - ToInt;
    const int32 n = static_cast<int32>(fn);
    float64 r = x - fn * PiOver2A;
    float64 w = fn * PiOver2ATail;
    y0 = r - w;
    const int32 ex = static_cast<int32>((High(x) >> 20) & 0x7FF);
    int32 ey = static_cast<int32>((High(y0) >> 20) & 0x7FF);
    if (ex - ey > 16)
    {
        float64 t = r;
        w = fn * PiOver2B;
        r = t - w;
        w = fn * PiOver2BTail - ((t - r) - w);
        y0 = r - w;
        ey = static_cast<int32>((High(y0) >> 20) & 0x7FF);
        if (ex - ey > 49)
        {
            t = r;
            w = fn * PiOver2C;
            r = t - w;
            w = fn * PiOver2CTail - ((t - r) - w);
            y0 = r - w;
        }
    }
    y1 = (r - y0) - w;
    return n;
}

int32 ReducePiOver2(float64 x, float64 &y0, float64 &y1)
{
    if ((High(x) & 0x7FFFFFFF) >= 0x413921FB)
    {
        const float64 reduced = __builtin_fmod(Abs(x), TwoPi);
        return ReducePiOver2Medium(x < 0 ? -reduced : reduced, y0, y1);
    }
    return ReducePiOver2Medium(x, y0, y1);
}

float64 SinhSeries(float64 x)
{
    const float64 x2 = x * x;
    const float64 tail = 1.0 / 6227020800.0 + x2 * (1.0 / 1307674368000.0 + x2 * (1.0 / 355687428096000.0));
    return x +
           x * x2 *
               (1.0 / 6.0 +
                x2 * (1.0 / 120.0 + x2 * (1.0 / 5040.0 + x2 * (1.0 / 362880.0 + x2 * (1.0 / 39916800.0 + x2 * tail)))));
}

void Split(float64 a, float64 &high, float64 &low)
{
    const float64 c = 134217729.0 * a;
    high = c - (c - a);
    low = a - high;
}

void TwoProduct(float64 a, float64 b, float64 &product, float64 &error)
{
    product = a * b;
    float64 ah = 0;
    float64 al = 0;
    float64 bh = 0;
    float64 bl = 0;
    Split(a, ah, al);
    Split(b, bh, bl);
    error = ((ah * bh - product) + ah * bl + al * bh) + al * bl;
}

void Log2Parts(float64 x, float64 &resultHigh, float64 &resultLow)
{
    constexpr float64 Lg1 = 6.666666666666735130e-01;
    constexpr float64 Lg2 = 3.999999999940941908e-01;
    constexpr float64 Lg3 = 2.857142874366239149e-01;
    constexpr float64 Lg4 = 2.222219843214978396e-01;
    constexpr float64 Lg5 = 1.818357216161805012e-01;
    constexpr float64 Lg6 = 1.531383769920937332e-01;
    constexpr float64 Lg7 = 1.479819860511658591e-01;
    constexpr float64 InvLn2Hi = 1.44269504072144627571e+00;
    constexpr float64 InvLn2Lo = 1.67517131648865118353e-10;
    uint32 hx = High(x);
    int32 k = 0;
    if (hx < 0x00100000)
    {
        k -= 54;
        x *= 18014398509481984.0;
        hx = High(x);
    }
    hx += 0x3FF00000 - 0x3FE6A09E;
    k += static_cast<int32>(hx >> 20) - 0x3FF;
    hx = (hx & 0x000FFFFF) + 0x3FE6A09E;
    x = FromWords(hx, Low(x));
    const float64 f = x - 1.0;
    const float64 hfsq = 0.5 * f * f;
    const float64 s = f / (2.0 + f);
    const float64 z = s * s;
    const float64 w = z * z;
    const float64 t1 = w * (Lg2 + w * (Lg4 + w * Lg6));
    const float64 t2 = z * (Lg1 + w * (Lg3 + w * (Lg5 + w * Lg7)));
    const float64 r = t2 + t1;
    float64 hi = f - hfsq;
    hi = FromWords(High(hi), 0);
    const float64 lo = f - hi - hfsq + s * (hfsq + r);
    float64 valueHigh = hi * InvLn2Hi;
    float64 valueLow = (lo + hi) * InvLn2Lo + lo * InvLn2Hi;
    const float64 y = k;
    const float64 sum = y + valueHigh;
    valueLow += (y - sum) + valueHigh;
    valueHigh = sum;
    resultHigh = valueHigh;
    resultLow = valueLow;
}

bool IsInteger(float64 x)
{
    return __builtin_trunc(x) == x;
}

bool IsOddInteger(float64 x)
{
    return IsInteger(x) && Abs(x) < 9007199254740992.0 && __builtin_fmod(x, 2.0) != 0;
}
} // namespace

float64 DeterministicMath::Scale(float64 x, int32 n)
{
    float64 y = x;
    if (n > 1023)
    {
        y *= 8.98846567431157953865e+307;
        n -= 1023;
        if (n > 1023)
        {
            y *= 8.98846567431157953865e+307;
            n -= 1023;
            n = n > 1023 ? 1023 : n;
        }
    }
    else if (n < -1022)
    {
        constexpr float64 Down = 2.2250738585072014e-308 * 9007199254740992.0;
        y *= Down;
        n += 1022 - 53;
        if (n < -1022)
        {
            y *= Down;
            n += 1022 - 53;
            n = n < -1022 ? -1022 : n;
        }
    }
    return y * BitCast<float64>(static_cast<uint64>(0x3FF + n) << 52);
}

float64 DeterministicMath::Sin(float64 x)
{
    const uint32 ix = High(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB)
    {
        return ix < 0x3E500000 ? x : KernelSin(x, 0, false);
    }
    if (ix >= 0x7FF00000)
    {
        return x - x;
    }
    float64 y0 = 0;
    float64 y1 = 0;
    switch (ReducePiOver2(x, y0, y1) & 3)
    {
    case 0:
        return KernelSin(y0, y1, true);
    case 1:
        return KernelCos(y0, y1);
    case 2:
        return -KernelSin(y0, y1, true);
    default:
        return -KernelCos(y0, y1);
    }
}

float64 DeterministicMath::Cos(float64 x)
{
    const uint32 ix = High(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB)
    {
        return ix < 0x3E46A09E ? 1.0 : KernelCos(x, 0);
    }
    if (ix >= 0x7FF00000)
    {
        return x - x;
    }
    float64 y0 = 0;
    float64 y1 = 0;
    switch (ReducePiOver2(x, y0, y1) & 3)
    {
    case 0:
        return KernelCos(y0, y1);
    case 1:
        return -KernelSin(y0, y1, true);
    case 2:
        return -KernelCos(y0, y1);
    default:
        return KernelSin(y0, y1, true);
    }
}

float64 DeterministicMath::Tan(float64 x)
{
    const uint32 ix = High(x) & 0x7FFFFFFF;
    if (ix <= 0x3FE921FB)
    {
        return ix < 0x3E400000 ? x : KernelSin(x, 0, false) / KernelCos(x, 0);
    }
    if (ix >= 0x7FF00000)
    {
        return x - x;
    }
    float64 y0 = 0;
    float64 y1 = 0;
    const int32 n = ReducePiOver2(x, y0, y1);
    const float64 sine = KernelSin(y0, y1, true);
    const float64 cosine = KernelCos(y0, y1);
    return (n & 1) == 0 ? sine / cosine : -cosine / sine;
}

float64 DeterministicMath::Atan(float64 x)
{
    constexpr float64 AtanHi[] = {4.63647609000806093515e-01, 7.85398163397448278999e-01, 9.82793723247329054082e-01,
                                  1.57079632679489655800e+00};
    constexpr float64 AtanLo[] = {2.26987774529616870924e-17, 3.06161699786838301793e-17, 1.39033110312309984516e-17,
                                  6.12323399573676603587e-17};
    constexpr float64 AT[] = {3.33333333333329318027e-01,  -1.99999999998764832476e-01, 1.42857142725034663711e-01,
                              -1.11111104054623557880e-01, 9.09088713343650656196e-02,  -7.69187620504482999495e-02,
                              6.66107313738753120669e-02,  -5.83357013379057348645e-02, 4.97687799461593236017e-02,
                              -3.65315727442169155270e-02, 1.62858201153657823623e-02};
    const uint32 hx = High(x);
    const uint32 ix = hx & 0x7FFFFFFF;
    if (ix >= 0x44100000)
    {
        if (IsNan(x))
        {
            return x + x;
        }
        const float64 z = AtanHi[3] + AtanLo[3];
        return (hx >> 31) != 0 ? -z : z;
    }
    int32 id = -1;
    if (ix < 0x3FDC0000)
    {
        if (ix < 0x3E400000)
        {
            return x;
        }
    }
    else
    {
        x = Abs(x);
        if (ix < 0x3FF30000)
        {
            if (ix < 0x3FE60000)
            {
                id = 0;
                x = (2.0 * x - 1.0) / (2.0 + x);
            }
            else
            {
                id = 1;
                x = (x - 1.0) / (x + 1.0);
            }
        }
        else if (ix < 0x40038000)
        {
            id = 2;
            x = (x - 1.5) / (1.0 + 1.5 * x);
        }
        else
        {
            id = 3;
            x = -1.0 / x;
        }
    }
    const float64 z = x * x;
    const float64 w = z * z;
    const float64 s1 = z * (AT[0] + w * (AT[2] + w * (AT[4] + w * (AT[6] + w * (AT[8] + w * AT[10])))));
    const float64 s2 = w * (AT[1] + w * (AT[3] + w * (AT[5] + w * (AT[7] + w * AT[9]))));
    if (id < 0)
    {
        return x - x * (s1 + s2);
    }
    const float64 result = AtanHi[id] - ((x * (s1 + s2) - AtanLo[id]) - x);
    return (hx >> 31) != 0 ? -result : result;
}

float64 DeterministicMath::Atan2(float64 y, float64 x)
{
    if (IsNan(x) || IsNan(y))
    {
        return x + y;
    }
    const bool yNegative = (High(y) >> 31) != 0;
    const bool xNegative = (High(x) >> 31) != 0;
    if (y == 0)
    {
        return xNegative ? (yNegative ? -Pi : Pi) : y;
    }
    if (x == 0)
    {
        return yNegative ? -HalfPi : HalfPi;
    }
    const bool xInfinite = Abs(x) == __builtin_inf();
    const bool yInfinite = Abs(y) == __builtin_inf();
    if (xInfinite)
    {
        const float64 angle = yInfinite ? (xNegative ? 3 * Pi / 4 : Pi / 4) : (xNegative ? Pi : 0.0);
        return yNegative ? -angle : angle;
    }
    if (yInfinite)
    {
        return yNegative ? -HalfPi : HalfPi;
    }
    const int32 exponent = static_cast<int32>((High(y) >> 20) & 0x7FF) - static_cast<int32>((High(x) >> 20) & 0x7FF);
    float64 z = 0;
    if (exponent > 60)
    {
        z = HalfPi + 0.5 * PiLo;
    }
    else if (xNegative && exponent < -60)
    {
        z = 0;
    }
    else
    {
        z = Atan(Abs(y / x));
    }
    if (!xNegative)
    {
        return yNegative ? -z : z;
    }
    return yNegative ? (z - PiLo) - Pi : Pi - (z - PiLo);
}

float64 DeterministicMath::Asin(float64 x)
{
    if (Abs(x) > 1.0)
    {
        return (x - x) / (x - x);
    }
    return Atan2(x, __builtin_sqrt((1.0 - x) * (1.0 + x)));
}

float64 DeterministicMath::Acos(float64 x)
{
    if (Abs(x) > 1.0)
    {
        return (x - x) / (x - x);
    }
    return Atan2(__builtin_sqrt((1.0 - x) * (1.0 + x)), x);
}

float64 DeterministicMath::Exp(float64 x)
{
    constexpr float64 P1 = 1.66666666666666019037e-01;
    constexpr float64 P2 = -2.77777777770155933842e-03;
    constexpr float64 P3 = 6.61375632143793436117e-05;
    constexpr float64 P4 = -1.65339022054652515390e-06;
    constexpr float64 P5 = 4.13813679705723846039e-08;
    if (IsNan(x))
    {
        return x + x;
    }
    if (x > 709.782712893383973096)
    {
        return __builtin_inf();
    }
    if (x < -745.13321910194110842)
    {
        return 0;
    }
    const uint32 ix = High(x) & 0x7FFFFFFF;
    if (ix < 0x3E300000)
    {
        return 1.0 + x;
    }
    float64 hi = x;
    float64 lo = 0;
    int32 k = 0;
    if (ix > 0x3FD62E42)
    {
        k = static_cast<int32>(InvLn2 * x + (x < 0 ? -0.5 : 0.5));
        hi = x - k * Ln2Hi;
        lo = k * Ln2Lo;
        x = hi - lo;
    }
    const float64 xx = x * x;
    const float64 c = x - xx * (P1 + xx * (P2 + xx * (P3 + xx * (P4 + xx * P5))));
    const float64 y = 1.0 + (x * c / (2.0 - c) - lo + hi);
    return k == 0 ? y : Scale(y, k);
}

float64 DeterministicMath::Exp2(float64 x)
{
    if (IsNan(x))
    {
        return x + x;
    }
    if (x >= 1024.0)
    {
        return __builtin_inf();
    }
    if (x < -1080.0)
    {
        return 0;
    }
    const float64 k = x + ToInt - ToInt;
    const float64 f = x - k;
    float64 fh = 0;
    float64 fl = 0;
    Split(f, fh, fl);
    return Scale(Exp(fh * Ln2Hi + (fl * Ln2Hi + f * Ln2Lo)), static_cast<int32>(k));
}

float64 DeterministicMath::Log(float64 x)
{
    if (IsNan(x) || x < 0)
    {
        return (x - x) / (x - x);
    }
    if (x == 0)
    {
        return -__builtin_inf();
    }
    if (x == __builtin_inf())
    {
        return x;
    }
    float64 high = 0;
    float64 low = 0;
    Log2Parts(x, high, low);
    float64 hh = 0;
    float64 hl = 0;
    Split(high, hh, hl);
    return hh * Ln2Hi + (hl * Ln2Hi + (high * Ln2Lo + low * (Ln2Hi + Ln2Lo)));
}

float64 DeterministicMath::Log2(float64 x)
{
    if (IsNan(x) || x < 0)
    {
        return (x - x) / (x - x);
    }
    if (x == 0)
    {
        return -__builtin_inf();
    }
    if (x == __builtin_inf())
    {
        return x;
    }
    float64 high = 0;
    float64 low = 0;
    Log2Parts(x, high, low);
    return high + low;
}

float64 DeterministicMath::Pow(float64 x, float64 y)
{
    if (y == 0 || x == 1.0)
    {
        return 1.0;
    }
    if (IsNan(x) || IsNan(y))
    {
        return x + y;
    }
    const float64 infinity = __builtin_inf();
    const float64 ax = Abs(x);
    if (Abs(y) == infinity)
    {
        if (ax == 1.0)
        {
            return 1.0;
        }
        return (ax < 1.0) == (y < 0) ? infinity : 0.0;
    }
    const bool odd = IsOddInteger(y);
    if (x == 0 || ax == infinity)
    {
        const bool large = (x == 0) == (y < 0);
        const float64 magnitude = large ? infinity : 0.0;
        return odd && (High(x) >> 31) != 0 ? -magnitude : magnitude;
    }
    if (x < 0 && !IsInteger(y))
    {
        return (x - x) / (x - x);
    }
    float64 high = 0;
    float64 low = 0;
    Log2Parts(ax, high, low);
    float64 product = 0;
    float64 error = 0;
    TwoProduct(y, high, product, error);
    error += y * low;
    float64 result = 0;
    if (product + error > 1100.0)
    {
        result = infinity;
    }
    else if (product + error < -1200.0)
    {
        result = 0;
    }
    else
    {
        const float64 k = product + ToInt - ToInt;
        const float64 f = (product - k) + error;
        float64 fh = 0;
        float64 fl = 0;
        Split(f, fh, fl);
        result = Scale(Exp(fh * Ln2Hi + (fl * Ln2Hi + f * Ln2Lo)), static_cast<int32>(k));
    }
    return x < 0 && odd ? -result : result;
}

float64 DeterministicMath::Sinh(float64 x)
{
    const float64 ax = Abs(x);
    if (ax < 0.5)
    {
        return SinhSeries(x);
    }
    if (ax > 710.0)
    {
        return x * __builtin_inf();
    }
    const float64 e = ax < 709.0 ? Exp(ax) : Exp(0.5 * ax);
    const float64 result = ax < 709.0 ? 0.5 * (e - 1.0 / e) : 0.5 * e * e;
    return x < 0 ? -result : result;
}

float64 DeterministicMath::Cosh(float64 x)
{
    const float64 ax = Abs(x);
    if (IsNan(x))
    {
        return x * x;
    }
    if (ax > 710.5)
    {
        return __builtin_inf();
    }
    if (ax >= 709.0)
    {
        const float64 e = Exp(0.5 * ax);
        return 0.5 * e * e;
    }
    const float64 e = Exp(ax);
    return 0.5 * (e + 1.0 / e);
}

float64 DeterministicMath::Tanh(float64 x)
{
    const float64 ax = Abs(x);
    if (IsNan(x))
    {
        return x + x;
    }
    float64 result = 0;
    if (ax > 22.0)
    {
        result = 1.0;
    }
    else if (ax < 0.5)
    {
        result = SinhSeries(ax) / Cosh(ax);
    }
    else
    {
        result = 1.0 - 2.0 / (Exp(2.0 * ax) + 1.0);
    }
    return x < 0 ? -result : result;
}
} // namespace Marlin
