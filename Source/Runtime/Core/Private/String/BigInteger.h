#pragma once

#include "CoreTypes.h"
#include "Diagnostics/Assert.h"
#include "Templates/Bit.h"

namespace Marlin::Private
{
/// A fixed-capacity unsigned integer for exact float conversion; 4608 bits covers every double and the digits
/// that parsing keeps.
class BigInteger
{
public:
    static constexpr uint32 MaxLimbs = 144;

    BigInteger() = default;

    explicit BigInteger(uint64 value)
    {
        Set(value);
    }

    void Set(uint64 value)
    {
        _limbs[0] = static_cast<uint32>(value);
        _limbs[1] = static_cast<uint32>(value >> 32);
        _count = _limbs[1] != 0 ? 2 : static_cast<uint32>(_limbs[0] != 0);
    }

    [[nodiscard]] bool IsZero() const
    {
        return _count == 0;
    }

    [[nodiscard]] uint32 BitLength() const
    {
        return _count == 0 ? 0 : (_count - 1) * 32 + static_cast<uint32>(32 - CountLeadingZeros(_limbs[_count - 1]));
    }

    /// Bits [start, start + 64), and whether any bit below `start` is set.
    [[nodiscard]] uint64 Bits64(uint32 start, bool &belowNonZero) const
    {
        uint64 value = 0;
        for (uint32 i = 0; i < 64; i += 32)
        {
            value |= static_cast<uint64>(Word32(start + i)) << i;
        }
        belowNonZero = false;
        const uint32 fullLimbs = start / 32;
        for (uint32 i = 0; i < fullLimbs && i < _count; ++i)
        {
            belowNonZero |= _limbs[i] != 0;
        }
        if (start % 32 != 0 && fullLimbs < _count)
        {
            belowNonZero |= (_limbs[fullLimbs] & ((1u << (start % 32)) - 1)) != 0;
        }
        return value;
    }

    void MultiplyAdd(uint32 factor, uint32 addend = 0)
    {
        uint64 carry = addend;
        for (uint32 i = 0; i < _count; ++i)
        {
            const uint64 product = static_cast<uint64>(_limbs[i]) * factor + carry;
            _limbs[i] = static_cast<uint32>(product);
            carry = product >> 32;
        }
        if (carry != 0)
        {
            MARLIN_ASSERT(_count < MaxLimbs);
            _limbs[_count++] = static_cast<uint32>(carry);
        }
    }

    void MultiplyPow10(uint32 exponent)
    {
        while (exponent >= 9)
        {
            MultiplyAdd(1000000000);
            exponent -= 9;
        }
        constexpr uint32 Small[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
        if (exponent != 0)
        {
            MultiplyAdd(Small[exponent]);
        }
    }

    void ShiftLeft(uint32 bits)
    {
        if (_count == 0 || bits == 0)
        {
            return;
        }
        const uint32 limbShift = bits / 32;
        const uint32 bitShift = bits % 32;
        MARLIN_ASSERT(_count + limbShift + 1 <= MaxLimbs);
        _limbs[_count + limbShift] = 0;
        if (bitShift == 0)
        {
            for (uint32 i = _count; i-- != 0;)
            {
                _limbs[i + limbShift] = _limbs[i];
            }
        }
        else
        {
            for (uint32 i = _count; i-- != 0;)
            {
                _limbs[i + limbShift + 1] |= _limbs[i] >> (32 - bitShift);
                _limbs[i + limbShift] = _limbs[i] << bitShift;
            }
        }
        for (uint32 i = 0; i < limbShift; ++i)
        {
            _limbs[i] = 0;
        }
        _count += limbShift + 1;
        Trim();
    }

    void ShiftRightOne()
    {
        for (uint32 i = 0; i < _count; ++i)
        {
            _limbs[i] = (_limbs[i] >> 1) | (i + 1 < _count ? _limbs[i + 1] << 31 : 0);
        }
        Trim();
    }

    /// Sets this to 2^exponent.
    void SetPow2(uint32 exponent)
    {
        Set(1);
        ShiftLeft(exponent);
    }

    /// Requires this >= other.
    void Subtract(const BigInteger &other)
    {
        int64 borrow = 0;
        for (uint32 i = 0; i < _count; ++i)
        {
            const int64 difference = static_cast<int64>(_limbs[i]) - other.Word(i) - borrow;
            _limbs[i] = static_cast<uint32>(difference);
            borrow = difference < 0 ? 1 : 0;
        }
        Trim();
    }

    [[nodiscard]] static int32 Compare(const BigInteger &a, const BigInteger &b)
    {
        if (a._count != b._count)
        {
            return a._count < b._count ? -1 : 1;
        }
        for (uint32 i = a._count; i-- != 0;)
        {
            if (a._limbs[i] != b._limbs[i])
            {
                return a._limbs[i] < b._limbs[i] ? -1 : 1;
            }
        }
        return 0;
    }

    /// Divides by `divisor` when the quotient is below 10, leaving the remainder.
    uint32 DivideDigit(const BigInteger &divisor)
    {
        uint32 quotient = 0;
        while (Compare(*this, divisor) >= 0)
        {
            Subtract(divisor);
            ++quotient;
        }
        MARLIN_CHECK_SLOW(quotient < 10);
        return quotient;
    }

private:
    [[nodiscard]] uint32 Word(uint32 index) const
    {
        return index < _count ? _limbs[index] : 0;
    }

    /// 32 bits starting at bit `start`.
    [[nodiscard]] uint32 Word32(uint32 start) const
    {
        const uint32 index = start / 32;
        const uint32 shift = start % 32;
        if (shift == 0)
        {
            return Word(index);
        }
        return (Word(index) >> shift) | (Word(index + 1) << (32 - shift));
    }

    void Trim()
    {
        while (_count != 0 && _limbs[_count - 1] == 0)
        {
            --_count;
        }
    }

    uint32 _count = 0;
    uint32 _limbs[MaxLimbs] = {};
};
} // namespace Marlin::Private
