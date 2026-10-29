#include "Replication/NetBitStream.h"

#include "Memory/MemoryOps.h"

namespace Marlin
{
void NetBitWriter::WriteBits(uint64 value, uint32 count)
{
    MARLIN_ASSERT(count <= 64);
    if (count == 0)
    {
        return;
    }
    if (count < 64)
    {
        value &= (uint64{1} << count) - 1;
    }
    // The first byte may hold earlier bits; the ones after it are new, so they are stored rather than merged.
    const auto bit = static_cast<uint32>(_bitCount % 8);
    const usize first = _bitCount / 8;
    const usize end = (_bitCount + count + 7) / 8;
    _output->Resize(end);
    uint8 *bytes = _output->Data() + first;
    if (bit == 0)
    {
        bytes[0] = 0;
    }
    bytes[0] |= static_cast<uint8>(value << bit);
    uint64 rest = value >> (8 - bit);
    for (usize i = 1; i < end - first; ++i)
    {
        bytes[i] = static_cast<uint8>(rest);
        rest >>= 8;
    }
    _bitCount += count;
}

void NetBitWriter::WriteBits(Span<const uint8> bytes, usize count)
{
    MARLIN_ASSERT(count <= bytes.Count() * 8);
    if (_bitCount % 8 == 0)
    {
        // Byte-aligned: the bytes go in whole; padding past *count* in the last one must already be zero.
        const usize whole = (count + 7) / 8;
        const usize first = _output->Count();
        _output->Resize(first + whole);
        MemCopy(_output->Data() + first, bytes.Data(), whole);
        _bitCount += count;
        return;
    }
    for (usize offset = 0; offset < count; offset += 64)
    {
        const usize left = count - offset;
        const usize byteCount = (left < 64 ? (left + 7) / 8 : 8);
        uint64 value = 0;
        MemCopy(&value, bytes.Data() + offset / 8, byteCount);
        WriteBits(value, left < 64 ? static_cast<uint32>(left) : 64);
    }
}

void NetBitWriter::WriteVarUInt(uint64 value)
{
    // RECOVERY-DECISION D1: varints are LEB128 groups of seven bits written into the bit stream.
    while (value >= 0x80)
    {
        WriteBits((value & 0x7F) | 0x80, 8);
        value >>= 7;
    }
    WriteBits(value, 8);
}

void NetBitWriter::WriteVarInt(int64 value)
{
    WriteVarUInt((static_cast<uint64>(value) << 1) ^ static_cast<uint64>(value >> 63));
}

uint64 NetBitReader::ReadBits(uint32 count)
{
    MARLIN_ASSERT(count <= 64);
    if (_bitPosition + count > _input.Count() * 8)
    {
        _overflowed = true;
        _bitPosition = _input.Count() * 8;
        return 0;
    }
    if (count == 0)
    {
        return 0;
    }
    // Up to nine bytes hold the bits: eight in one load, the ninth only when they straddle it.
    const usize first = _bitPosition / 8;
    const auto bit = static_cast<uint32>(_bitPosition % 8);
    const usize available = _input.Count() - first;
    uint64 low = 0;
    MemCopy(&low, _input.Data() + first, available < 8 ? available : 8);
    uint64 value = low >> bit;
    if (bit + count > 64)
    {
        value |= static_cast<uint64>(_input[first + 8]) << (64 - bit);
    }
    if (count < 64)
    {
        value &= (uint64{1} << count) - 1;
    }
    _bitPosition += count;
    return value;
}

uint64 NetBitReader::ReadVarUInt()
{
    uint64 value = 0;
    for (uint32 shift = 0; shift < 64; shift += 7)
    {
        const uint64 group = ReadBits(8);
        value |= (group & 0x7F) << shift;
        if ((group & 0x80) == 0 || _overflowed)
        {
            return value;
        }
    }
    // More than ten groups: not something WriteVarUInt makes.
    _overflowed = true;
    return value;
}

int64 NetBitReader::ReadVarInt()
{
    const uint64 value = ReadVarUInt();
    return static_cast<int64>(value >> 1) ^ -static_cast<int64>(value & 1);
}
} // namespace Marlin
