#include "ScriptMath.h"

#include "Math/DeterministicMath.h"
#include "Memory/MemoryOps.h"
#include "ScriptVM/ScriptBytecode.h"
#include "Templates/Bit.h"
#include "Templates/TypeTraits.h"

// RECOVERY-DECISION SB7: Math operands are register bytes in MIR memory layout: vector lanes packed, matrix rows
// at the MIR row stride (the row vector's size aligned to its alignment, so a 3-lane row takes 4 lanes), bools 4
// bytes. Transcendental functions go through DeterministicMath so every platform gets the same bits; the others use
// correctly rounded operations. Round rounds half to even.

namespace Marlin
{
namespace
{
template <typename T> void Load(T &value, const uint8 *registers, uint32 lane)
{
    MemCopy(&value, registers + lane * sizeof(T), sizeof(T));
}

template <typename T> void Store(uint8 *registers, uint32 lane, T value)
{
    MemCopy(registers + lane * sizeof(T), &value, sizeof(T));
}

template <typename T> T Transcendental(ScriptMathOp op, T x, T y)
{
    switch (op)
    {
    case ScriptMathOp::Exp:
        return DeterministicMath::Exp(x);
    case ScriptMathOp::Exp2:
        return DeterministicMath::Exp2(x);
    case ScriptMathOp::Log:
        return DeterministicMath::Log(x);
    case ScriptMathOp::Log2:
        return DeterministicMath::Log2(x);
    case ScriptMathOp::Pow:
        return DeterministicMath::Pow(x, y);
    case ScriptMathOp::Sin:
        return DeterministicMath::Sin(x);
    case ScriptMathOp::Cos:
        return DeterministicMath::Cos(x);
    case ScriptMathOp::Tan:
        return DeterministicMath::Tan(x);
    case ScriptMathOp::Asin:
        return DeterministicMath::Asin(x);
    case ScriptMathOp::Acos:
        return DeterministicMath::Acos(x);
    case ScriptMathOp::Atan:
        return DeterministicMath::Atan(x);
    case ScriptMathOp::Atan2:
        return DeterministicMath::Atan2(x, y);
    case ScriptMathOp::Sinh:
        return DeterministicMath::Sinh(x);
    case ScriptMathOp::Cosh:
        return DeterministicMath::Cosh(x);
    case ScriptMathOp::Tanh:
        return DeterministicMath::Tanh(x);
    default:
        return x;
    }
}

template <typename T> T Square(T x)
{
    return x * x;
}

/// The determinant of the *n* x *n* matrix in *m*, rows of 4 entries.
template <typename T> T DeterminantOf(const T (&m)[4][4], uint32 n)
{
    if (n == 1)
    {
        return m[0][0];
    }
    if (n == 2)
    {
        return m[0][0] * m[1][1] - m[0][1] * m[1][0];
    }
    T result = 0;
    for (uint32 column = 0; column < n; ++column)
    {
        T minor[4][4] = {};
        for (uint32 r = 1; r < n; ++r)
        {
            for (uint32 c = 0, k = 0; c < n; ++c)
            {
                if (c != column)
                {
                    minor[r - 1][k++] = m[r][c];
                }
            }
        }
        const T term = m[0][column] * DeterminantOf(minor, n - 1);
        result = (column % 2) == 0 ? result + term : result - term;
    }
    return result;
}

/// Determinant and Inverse of a square matrix of *n* rows.
template <typename T> bool Matrix(ScriptMathOp op, uint32 n, uint8 *result, const uint8 *b)
{
    if (n < 2 || n > 4)
    {
        return false;
    }
    const uint32 stride = static_cast<uint32>(sizeof(T)) * (n == 3 ? 4 : n);
    T m[4][4] = {};
    for (uint32 r = 0; r < n; ++r)
    {
        for (uint32 c = 0; c < n; ++c)
        {
            Load(m[r][c], b + r * stride, c);
        }
    }
    const T determinant = DeterminantOf(m, n);
    if (op == ScriptMathOp::Determinant)
    {
        Store(result, 0, determinant);
        return true;
    }
    if (op != ScriptMathOp::Inverse)
    {
        return false;
    }
    // The adjugate over the determinant; a singular matrix gives infinities and NaNs, as on the GPU.
    T inverse[4][4] = {};
    for (uint32 r = 0; r < n; ++r)
    {
        for (uint32 c = 0; c < n; ++c)
        {
            T minor[4][4] = {};
            for (uint32 i = 0, mi = 0; i < n; ++i)
            {
                if (i == r)
                {
                    continue;
                }
                for (uint32 j = 0, mj = 0; j < n; ++j)
                {
                    if (j != c)
                    {
                        minor[mi][mj++] = m[i][j];
                    }
                }
                ++mi;
            }
            const T cofactor = DeterminantOf(minor, n - 1);
            inverse[c][r] = ((r + c) % 2 == 0 ? cofactor : -cofactor) / determinant;
        }
    }
    for (uint32 r = 0; r < n; ++r)
    {
        for (uint32 c = 0; c < n; ++c)
        {
            Store(result + r * stride, c, inverse[r][c]);
        }
    }
    return true;
}

template <typename T> T Fma(T a, T b, T c)
{
    if constexpr (sizeof(T) == 4)
    {
        return __builtin_fmaf(a, b, c);
    }
    else
    {
        return __builtin_fma(a, b, c);
    }
}

template <typename T> T SqrtOf(T x)
{
    if constexpr (sizeof(T) == 4)
    {
        return __builtin_sqrtf(x);
    }
    else
    {
        return __builtin_sqrt(x);
    }
}

template <typename T> T MinOf(T a, T b)
{
    return a != a ? b : (b != b ? a : (b < a ? b : a));
}

template <typename T> T MaxOf(T a, T b)
{
    return a != a ? b : (b != b ? a : (b > a ? b : a));
}

template <typename T> T Dot(const uint8 *a, const uint8 *b, uint32 lanes)
{
    T sum = 0;
    for (uint32 i = 0; i < lanes; ++i)
    {
        T x;
        T y;
        Load(x, a, i);
        Load(y, b, i);
        sum += x * y;
    }
    return sum;
}

template <typename T> bool Float(ScriptMathOp op, uint32 lanes, uint8 *result, const uint8 *b, const uint8 *c,
                                 const uint8 *d)
{
    switch (op)
    {
    case ScriptMathOp::Dot:
        Store(result, 0, Dot<T>(b, c, lanes));
        return true;
    case ScriptMathOp::Length:
        Store(result, 0, SqrtOf(Dot<T>(b, b, lanes)));
        return true;
    case ScriptMathOp::Distance: {
        T sum = 0;
        for (uint32 i = 0; i < lanes; ++i)
        {
            T x;
            T y;
            Load(x, b, i);
            Load(y, c, i);
            sum += Square(x - y);
        }
        Store(result, 0, SqrtOf(sum));
        return true;
    }
    case ScriptMathOp::Normalize: {
        const T length = SqrtOf(Dot<T>(b, b, lanes));
        for (uint32 i = 0; i < lanes; ++i)
        {
            T x;
            Load(x, b, i);
            Store(result, i, x / length);
        }
        return true;
    }
    case ScriptMathOp::Cross: {
        if (lanes != 3)
        {
            return false;
        }
        T a[3];
        T e[3];
        for (uint32 i = 0; i < 3; ++i)
        {
            Load(a[i], b, i);
            Load(e[i], c, i);
        }
        Store(result, 0, a[1] * e[2] - a[2] * e[1]);
        Store(result, 1, a[2] * e[0] - a[0] * e[2]);
        Store(result, 2, a[0] * e[1] - a[1] * e[0]);
        return true;
    }
    case ScriptMathOp::Reflect: {
        const T scale = 2 * Dot<T>(c, b, lanes);
        for (uint32 i = 0; i < lanes; ++i)
        {
            T incident;
            T normal;
            Load(incident, b, i);
            Load(normal, c, i);
            Store(result, i, incident - scale * normal);
        }
        return true;
    }
    case ScriptMathOp::Refract: {
        T eta;
        Load(eta, d, 0);
        const T cosine = Dot<T>(c, b, lanes);
        const T k = 1 - eta * eta * (1 - cosine * cosine);
        for (uint32 i = 0; i < lanes; ++i)
        {
            T incident;
            T normal;
            Load(incident, b, i);
            Load(normal, c, i);
            Store(result, i, k < 0 ? T(0) : eta * incident - (eta * cosine + SqrtOf(k)) * normal);
        }
        return true;
    }
    case ScriptMathOp::FaceForward: {
        const bool keep = Dot<T>(d, c, lanes) < 0;
        for (uint32 i = 0; i < lanes; ++i)
        {
            T normal;
            Load(normal, b, i);
            Store(result, i, keep ? normal : -normal);
        }
        return true;
    }
    case ScriptMathOp::PackHalf2x16: {
        if (sizeof(T) != 4 || lanes != 2)
        {
            return false;
        }
        T x;
        T y;
        Load(x, b, 0);
        Load(y, b, 1);
        const uint32 packed = BitCast<uint16>(static_cast<_Float16>(x)) |
                              (static_cast<uint32>(BitCast<uint16>(static_cast<_Float16>(y))) << 16);
        Store(result, 0, packed);
        return true;
    }
    case ScriptMathOp::Any:
    case ScriptMathOp::All:
    case ScriptMathOp::CountBits:
    case ScriptMathOp::FirstBitHigh:
    case ScriptMathOp::FirstBitLow:
    case ScriptMathOp::ReverseBits:
    case ScriptMathOp::UnpackHalf2x16:
    case ScriptMathOp::Determinant:
    case ScriptMathOp::Inverse:
        return false;
    default:
        break;
    }
    // Lane-wise operations; read every operand first, since the result may share their registers.
    T out[4];
    for (uint32 i = 0; i < lanes; ++i)
    {
        T x;
        T y = 0;
        T z = 0;
        Load(x, b, i);
        if (c != nullptr)
        {
            Load(y, c, i);
        }
        if (d != nullptr)
        {
            Load(z, d, i);
        }
        switch (op)
        {
        case ScriptMathOp::Abs:
            out[i] = x < 0 ? -x : (x == 0 ? T(0) : x);
            break;
        case ScriptMathOp::Sign:
            out[i] = x > 0 ? T(1) : x < 0 ? T(-1) : T(0);
            break;
        case ScriptMathOp::Floor:
            out[i] = sizeof(T) == 4 ? static_cast<T>(__builtin_floorf(static_cast<float32>(x)))
                                    : static_cast<T>(__builtin_floor(static_cast<float64>(x)));
            break;
        case ScriptMathOp::Ceil:
            out[i] = sizeof(T) == 4 ? static_cast<T>(__builtin_ceilf(static_cast<float32>(x)))
                                    : static_cast<T>(__builtin_ceil(static_cast<float64>(x)));
            break;
        case ScriptMathOp::Round:
            out[i] = sizeof(T) == 4 ? static_cast<T>(__builtin_rintf(static_cast<float32>(x)))
                                    : static_cast<T>(__builtin_rint(static_cast<float64>(x)));
            break;
        case ScriptMathOp::Trunc:
            out[i] = sizeof(T) == 4 ? static_cast<T>(__builtin_truncf(static_cast<float32>(x)))
                                    : static_cast<T>(__builtin_trunc(static_cast<float64>(x)));
            break;
        case ScriptMathOp::Fract: {
            const T floor = sizeof(T) == 4 ? static_cast<T>(__builtin_floorf(static_cast<float32>(x)))
                                           : static_cast<T>(__builtin_floor(static_cast<float64>(x)));
            out[i] = x - floor;
            break;
        }
        case ScriptMathOp::Sqrt:
            out[i] = SqrtOf(x);
            break;
        case ScriptMathOp::InverseSqrt:
            out[i] = T(1) / SqrtOf(x);
            break;
        case ScriptMathOp::Exp:
        case ScriptMathOp::Exp2:
        case ScriptMathOp::Log:
        case ScriptMathOp::Log2:
        case ScriptMathOp::Pow:
        case ScriptMathOp::Sin:
        case ScriptMathOp::Cos:
        case ScriptMathOp::Tan:
        case ScriptMathOp::Asin:
        case ScriptMathOp::Acos:
        case ScriptMathOp::Atan:
        case ScriptMathOp::Atan2:
        case ScriptMathOp::Sinh:
        case ScriptMathOp::Cosh:
        case ScriptMathOp::Tanh:
            out[i] = Transcendental<T>(op, x, y);
            break;
        case ScriptMathOp::Min:
            out[i] = MinOf(x, y);
            break;
        case ScriptMathOp::Max:
            out[i] = MaxOf(x, y);
            break;
        case ScriptMathOp::Clamp:
            out[i] = MinOf(MaxOf(x, y), z);
            break;
        case ScriptMathOp::Lerp:
            out[i] = x + (y - x) * z;
            break;
        case ScriptMathOp::Step:
            out[i] = y >= x ? T(1) : T(0);
            break;
        case ScriptMathOp::SmoothStep: {
            const T t = MinOf(MaxOf((z - x) / (y - x), T(0)), T(1));
            out[i] = t * t * (T(3) - T(2) * t);
            break;
        }
        case ScriptMathOp::Fma:
            out[i] = Fma(x, y, z);
            break;
        case ScriptMathOp::IsNan:
        case ScriptMathOp::IsInf: {
            const bool is = op == ScriptMathOp::IsNan ? x != x : (x == x && x - x != x - x);
            out[i] = is ? T(1) : T(0);
            break;
        }
        default:
            return false;
        }
    }
    if (op == ScriptMathOp::IsNan || op == ScriptMathOp::IsInf)
    {
        for (uint32 i = 0; i < lanes; ++i)
        {
            Store(result, i, out[i] != 0 ? uint32{1} : uint32{0});
        }
        return true;
    }
    for (uint32 i = 0; i < lanes; ++i)
    {
        Store(result, i, out[i]);
    }
    return true;
}

template <typename T> bool IsNegative(T value)
{
    if constexpr (IsSigned<T>)
    {
        return value < 0;
    }
    else
    {
        (void)value;
        return false;
    }
}

template <typename T> uint32 FirstBitHigh(T value)
{
    using TUnsigned = MakeUnsigned<T>;
    TUnsigned bits = static_cast<TUnsigned>(value);
    if constexpr (IsSigned<T>)
    {
        // For a negative value, the first bit that differs from the sign.
        if (IsNegative(value))
        {
            bits = static_cast<TUnsigned>(~bits);
        }
    }
    if (bits == 0)
    {
        return ~0u;
    }
    return static_cast<uint32>(FloorLog2(bits));
}

template <typename T> bool Integer(ScriptMathOp op, uint32 lanes, uint8 *result, const uint8 *b, const uint8 *c,
                                   const uint8 *d)
{
    using TUnsigned = MakeUnsigned<T>;
    if (op == ScriptMathOp::Dot)
    {
        TUnsigned sum = 0;
        for (uint32 i = 0; i < lanes; ++i)
        {
            T x;
            T y;
            Load(x, b, i);
            Load(y, c, i);
            sum = static_cast<TUnsigned>(sum + static_cast<TUnsigned>(x) * static_cast<TUnsigned>(y));
        }
        Store(result, 0, static_cast<T>(sum));
        return true;
    }
    T out[4];
    for (uint32 i = 0; i < lanes; ++i)
    {
        T x;
        T y = 0;
        T z = 0;
        Load(x, b, i);
        if (c != nullptr)
        {
            Load(y, c, i);
        }
        if (d != nullptr)
        {
            Load(z, d, i);
        }
        switch (op)
        {
        case ScriptMathOp::Abs:
            out[i] = IsNegative(x) ? static_cast<T>(TUnsigned(0) - static_cast<TUnsigned>(x)) : x;
            break;
        case ScriptMathOp::Sign:
            out[i] = x > 0 ? T(1) : IsNegative(x) ? static_cast<T>(-1) : T(0);
            break;
        case ScriptMathOp::Min:
            out[i] = y < x ? y : x;
            break;
        case ScriptMathOp::Max:
            out[i] = y > x ? y : x;
            break;
        case ScriptMathOp::Clamp: {
            const T low = x < y ? y : x;
            out[i] = low > z ? z : low;
            break;
        }
        case ScriptMathOp::CountBits:
            out[i] = static_cast<T>(PopCount(static_cast<TUnsigned>(x)));
            break;
        case ScriptMathOp::FirstBitHigh:
            out[i] = static_cast<T>(static_cast<int32>(FirstBitHigh(x)));
            break;
        case ScriptMathOp::FirstBitLow:
            out[i] = x == 0 ? static_cast<T>(~TUnsigned(0))
                            : static_cast<T>(CountTrailingZeros(static_cast<TUnsigned>(x)));
            break;
        case ScriptMathOp::ReverseBits: {
            TUnsigned bits = static_cast<TUnsigned>(x);
            TUnsigned reversed = 0;
            for (uint32 bit = 0; bit < sizeof(T) * 8; ++bit)
            {
                reversed = static_cast<TUnsigned>((reversed << 1) | (bits & 1));
                bits = static_cast<TUnsigned>(bits >> 1);
            }
            out[i] = static_cast<T>(reversed);
            break;
        }
        default:
            return false;
        }
    }
    for (uint32 i = 0; i < lanes; ++i)
    {
        Store(result, i, out[i]);
    }
    return true;
}
} // namespace

bool ExecuteScriptMath(uint32 word, uint8 *result, const uint8 *b, const uint8 *c, const uint8 *d)
{
    const auto op = static_cast<ScriptMathOp>(word & 0xFF);
    const auto number = static_cast<ScriptNumber>((word >> 8) & 0xF);
    const uint32 lanes = (word >> 12) & 0xF;
    if (lanes == 0 || lanes > 4 || static_cast<uint32>(op) >= static_cast<uint32>(ScriptMathOp::Count))
    {
        return false;
    }
    switch (number)
    {
    case ScriptNumber::F32:
        if (op == ScriptMathOp::Determinant || op == ScriptMathOp::Inverse)
        {
            return Matrix<float32>(op, lanes, result, b);
        }
        return Float<float32>(op, lanes, result, b, c, d);
    case ScriptNumber::F64:
        if (op == ScriptMathOp::Determinant || op == ScriptMathOp::Inverse)
        {
            return Matrix<float64>(op, lanes, result, b);
        }
        return Float<float64>(op, lanes, result, b, c, d);
    case ScriptNumber::I32:
        return Integer<int32>(op, lanes, result, b, c, d);
    case ScriptNumber::U32:
        if (op == ScriptMathOp::UnpackHalf2x16)
        {
            uint32 packed;
            Load(packed, b, 0);
            Store(result, 0, static_cast<float32>(BitCast<_Float16>(static_cast<uint16>(packed))));
            Store(result, 1, static_cast<float32>(BitCast<_Float16>(static_cast<uint16>(packed >> 16))));
            return true;
        }
        return Integer<uint32>(op, lanes, result, b, c, d);
    case ScriptNumber::I64:
        return Integer<int64>(op, lanes, result, b, c, d);
    case ScriptNumber::U64:
        return Integer<uint64>(op, lanes, result, b, c, d);
    case ScriptNumber::Bool: {
        if (op != ScriptMathOp::Any && op != ScriptMathOp::All)
        {
            return false;
        }
        bool any = false;
        bool all = true;
        for (uint32 i = 0; i < lanes; ++i)
        {
            uint32 lane;
            Load(lane, b, i);
            any = any || lane != 0;
            all = all && lane != 0;
        }
        Store(result, 0, (op == ScriptMathOp::Any ? any : all) ? uint32{1} : uint32{0});
        return true;
    }
    default:
        return false;
    }
}
} // namespace Marlin
