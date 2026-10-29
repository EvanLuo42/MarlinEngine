#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"

namespace Marlin
{
/// Appends bits to a byte list, least significant bit first; the last byte is padded with zeros.
class REPLICATION_API NetBitWriter
{
public:
    explicit NetBitWriter(List<uint8> &output) : _output(&output), _bitCount(output.Count() * 8)
    {
    }

    /// The low *count* bits of *value*, at most 64.
    void WriteBits(uint64 value, uint32 count);
    /// The first *count* bits of *bytes*, as another NetBitWriter wrote them.
    void WriteBits(Span<const uint8> bytes, usize count);

    void WriteBool(bool value)
    {
        WriteBits(value ? 1 : 0, 1);
    }

    /// Seven bits a byte, smallest first, with a continuation bit: small numbers take one byte.
    void WriteVarUInt(uint64 value);
    /// Zigzag, so small negative numbers stay small too.
    void WriteVarInt(int64 value);

    [[nodiscard]] usize GetBitCount() const
    {
        return _bitCount;
    }

    [[nodiscard]] usize GetByteCount() const
    {
        return (_bitCount + 7) / 8;
    }

private:
    List<uint8> *_output;
    usize _bitCount;
};

/// Reads what NetBitWriter wrote. Reading past the end gives zeros and sets HasOverflowed, so a truncated or hostile
/// message is caught once at the end instead of at every read.
class REPLICATION_API NetBitReader
{
public:
    explicit NetBitReader(Span<const uint8> input) : _input(input)
    {
    }

    uint64 ReadBits(uint32 count);

    bool ReadBool()
    {
        return ReadBits(1) != 0;
    }

    uint64 ReadVarUInt();
    int64 ReadVarInt();

    [[nodiscard]] bool HasOverflowed() const
    {
        return _overflowed;
    }

    /// True once only the last byte's padding is left.
    [[nodiscard]] bool IsAtEnd() const
    {
        return _bitPosition + 8 > _input.Count() * 8;
    }

private:
    Span<const uint8> _input;
    usize _bitPosition = 0;
    bool _overflowed = false;
};
} // namespace Marlin
