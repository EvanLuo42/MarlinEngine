#include "Inflate.h"

#include "Memory/MemoryOps.h"

namespace Marlin::Inflate
{
namespace
{
constexpr uint32 FastBits = 9;
constexpr uint32 MaxBits = 15;

constexpr uint16 LengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8 LengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16 DistanceBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                     33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                     1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8 DistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                     6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// Reads bits least significant first, as deflate packs them, through a 64-bit buffer.
class Bits
{
public:
    explicit Bits(Span<const uint8> data) : _data(data)
    {
    }

    /// Tops the buffer up to more than 56 bits. Past the end it shifts in zero bytes and counts them, so a truncated
    /// stream decodes garbage that IsOverrun then rejects instead of reading out of bounds.
    void Fill()
    {
        // Away from the end, top up whole bytes with one load.
        if (_data.Count() - _position >= 8)
        {
            uint64 word = 0;
            MemCopy(&word, _data.Data() + _position, sizeof(word)); // Little-endian, as every target is.
            const uint32 bytes = (63 - _count) / 8;
            _buffer |= (bytes == 8 ? word : word & ((uint64{1} << (bytes * 8)) - 1)) << _count;
            _position += bytes;
            _count += bytes * 8;
            return;
        }
        while (_count <= 56)
        {
            uint64 byte = 0;
            if (_position < _data.Count())
            {
                byte = _data[_position++];
            }
            else
            {
                ++_overrun;
            }
            _buffer |= byte << _count;
            _count += 8;
        }
    }

    [[nodiscard]] uint32 Peek(uint32 count)
    {
        if (_count < count)
        {
            Fill();
        }
        return static_cast<uint32>(_buffer & ((uint64{1} << count) - 1));
    }

    void Drop(uint32 count)
    {
        _buffer >>= count;
        _count -= count;
    }

    uint32 Read(uint32 count)
    {
        const uint32 value = Peek(count);
        Drop(count);
        return value;
    }

    void AlignToByte()
    {
        Drop(_count % 8);
    }

    /// True once a bit that was not in the data has been consumed.
    [[nodiscard]] bool IsOverrun() const
    {
        return _count < _overrun * 8;
    }

    /// The index of the next unread byte; only meaningful at a byte boundary.
    [[nodiscard]] usize GetBytePosition() const
    {
        return _position + _overrun - _count / 8;
    }

    void Seek(usize position)
    {
        _position = position;
        _buffer = 0;
        _count = 0;
        _overrun = 0;
    }

private:
    Span<const uint8> _data;
    usize _position = 0;
    uint64 _buffer = 0;
    uint32 _count = 0;
    uint32 _overrun = 0;
};

/// A canonical Huffman code: a table for codes up to FastBits long, and the counts and sorted symbols for the rest.
struct Huffman
{
    /// The symbol in the low 9 bits and the code length above them; 0 where the code is longer than FastBits.
    uint16 Fast[1u << FastBits];
    uint16 Counts[MaxBits + 1];
    uint16 Symbols[288];

    bool Build(const uint8 *lengths, uint32 count)
    {
        MemZero(Fast, sizeof(Fast));
        MemZero(Counts, sizeof(Counts));
        for (uint32 i = 0; i < count; ++i)
        {
            ++Counts[lengths[i]];
        }
        Counts[0] = 0;
        int32 left = 1;
        for (uint32 length = 1; length <= MaxBits; ++length)
        {
            left = left * 2 - Counts[length];
            if (left < 0)
            {
                return false; // Over-subscribed; incomplete codes are allowed, as zlib allows a lone distance code.
            }
        }
        uint16 offsets[MaxBits + 2] = {};
        uint32 next[MaxBits + 1] = {};
        uint32 code = 0;
        for (uint32 length = 1; length <= MaxBits; ++length)
        {
            offsets[length + 1] = static_cast<uint16>(offsets[length] + Counts[length]);
            code = (code + Counts[length - 1]) << 1;
            next[length] = code;
        }
        for (uint32 symbol = 0; symbol < count; ++symbol)
        {
            const uint32 length = lengths[symbol];
            if (length == 0)
            {
                continue;
            }
            Symbols[offsets[length]++] = static_cast<uint16>(symbol);
            const uint32 assigned = next[length]++;
            if (length <= FastBits)
            {
                uint32 reversed = 0;
                for (uint32 bit = 0; bit < length; ++bit)
                {
                    reversed |= ((assigned >> bit) & 1u) << (length - 1 - bit);
                }
                for (uint32 slot = reversed; slot < (1u << FastBits); slot += 1u << length)
                {
                    Fast[slot] = static_cast<uint16>(symbol | (length << 9));
                }
            }
        }
        return true;
    }

    /// The next symbol, or -1 for a code the table does not have.
    int32 Decode(Bits &bits) const
    {
        const uint32 fast = Fast[bits.Peek(FastBits)];
        if (fast != 0)
        {
            bits.Drop(fast >> 9);
            return static_cast<int32>(fast & 511u);
        }
        int32 code = 0;
        int32 first = 0;
        int32 index = 0;
        for (uint32 length = 1; length <= MaxBits; ++length)
        {
            code |= static_cast<int32>(bits.Read(1));
            const int32 count = Counts[length];
            if (code - count < first)
            {
                return Symbols[index + (code - first)];
            }
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        return -1;
    }
};

/// Decodes literals and matches up to the end-of-block code.
bool Codes(Bits &bits, const Huffman &lengths, const Huffman &distances, List<uint8> &output)
{
    // Writes through a pointer into room made ahead, rather than a checked Add per byte; the list is cut back to
    // what was written on the way out. Zlib reserves the expected size, so the room is normally there already.
    usize count = output.Count();
    const auto makeRoom = [&output](usize needed) {
        if (needed > output.Count())
        {
            const usize grown = output.Count() * 2;
            output.ResizeUninitialized(needed > grown ? needed : grown);
        }
    };
    output.ResizeUninitialized(output.Capacity() > count + 258 ? output.Capacity() : count + 4096);
    const auto finish = [&output, &count](bool ok) {
        output.ResizeUninitialized(count);
        return ok;
    };
    while (true)
    {
        const int32 symbol = lengths.Decode(bits);
        if (symbol < 0 || bits.IsOverrun())
        {
            return finish(false);
        }
        if (symbol < 256)
        {
            makeRoom(count + 1);
            output.Data()[count++] = static_cast<uint8>(symbol);
            continue;
        }
        if (symbol == 256)
        {
            return finish(true);
        }
        const uint32 index = static_cast<uint32>(symbol) - 257;
        if (index >= 29)
        {
            return finish(false);
        }
        const usize length = LengthBase[index] + bits.Read(LengthExtra[index]);
        const int32 code = distances.Decode(bits);
        if (code < 0 || code >= 30)
        {
            return finish(false);
        }
        const usize distance = DistanceBase[code] + bits.Read(DistanceExtra[code]);
        if (bits.IsOverrun() || distance > count)
        {
            return finish(false);
        }
        makeRoom(count + length);
        uint8 *out = output.Data() + count;
        const uint8 *from = out - distance;
        if (distance == 1)
        {
            MemSet(out, *from, length);
        }
        else if (distance >= 8)
        {
            // Eight bytes at a time: each chunk reads only bytes written before it.
            usize i = 0;
            for (; i + 8 <= length; i += 8)
            {
                MemCopy(out + i, from + i, 8);
            }
            for (; i < length; ++i)
            {
                out[i] = from[i];
            }
        }
        else
        {
            for (usize i = 0; i < length; ++i)
            {
                out[i] = from[i]; // Byte by byte: a short match overlaps what it writes.
            }
        }
        count += length;
    }
}

bool FixedBlock(Bits &bits, List<uint8> &output)
{
    uint8 lengths[288];
    uint8 distanceLengths[30];
    for (uint32 i = 0; i < 288; ++i)
    {
        lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
    }
    for (uint8 &length : distanceLengths)
    {
        length = 5;
    }
    Huffman literals;
    Huffman distances;
    if (!literals.Build(lengths, 288) || !distances.Build(distanceLengths, 30))
    {
        return false;
    }
    return Codes(bits, literals, distances, output);
}

bool DynamicBlock(Bits &bits, List<uint8> &output)
{
    constexpr uint8 Order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const uint32 literalCount = bits.Read(5) + 257;
    const uint32 distanceCount = bits.Read(5) + 1;
    const uint32 codeCount = bits.Read(4) + 4;
    if (literalCount > 286 || distanceCount > 30)
    {
        return false;
    }
    uint8 codeLengths[19] = {};
    for (uint32 i = 0; i < codeCount; ++i)
    {
        codeLengths[Order[i]] = static_cast<uint8>(bits.Read(3));
    }
    Huffman codes;
    if (!codes.Build(codeLengths, 19))
    {
        return false;
    }
    uint8 lengths[286 + 30] = {};
    uint32 filled = 0;
    while (filled < literalCount + distanceCount)
    {
        const int32 symbol = codes.Decode(bits);
        if (symbol < 0 || bits.IsOverrun())
        {
            return false;
        }
        if (symbol < 16)
        {
            lengths[filled++] = static_cast<uint8>(symbol);
            continue;
        }
        uint8 value = 0;
        uint32 repeat = 0;
        if (symbol == 16)
        {
            if (filled == 0)
            {
                return false;
            }
            value = lengths[filled - 1];
            repeat = 3 + bits.Read(2);
        }
        else
        {
            repeat = symbol == 17 ? 3 + bits.Read(3) : 11 + bits.Read(7);
        }
        if (filled + repeat > literalCount + distanceCount)
        {
            return false;
        }
        while (repeat-- > 0)
        {
            lengths[filled++] = value;
        }
    }
    if (lengths[256] == 0)
    {
        return false; // No end-of-block code.
    }
    Huffman literals;
    Huffman distances;
    if (!literals.Build(lengths, literalCount) || !distances.Build(lengths + literalCount, distanceCount))
    {
        return false;
    }
    return Codes(bits, literals, distances, output);
}

uint32 Adler32(Span<const uint8> bytes)
{
    constexpr uint32 Modulus = 65521;
    // The most bytes summed before b can overflow 32 bits.
    constexpr usize Chunk = 5552;
    uint32 a = 1;
    uint32 b = 0;
    for (usize start = 0; start < bytes.Count(); start += Chunk)
    {
        const usize end = start + Chunk < bytes.Count() ? start + Chunk : bytes.Count();
        for (usize i = start; i < end; ++i)
        {
            a += bytes[i];
            b += a;
        }
        a %= Modulus;
        b %= Modulus;
    }
    return (b << 16) | a;
}
} // namespace

bool Zlib(Span<const uint8> input, List<uint8> &output, usize expected)
{
    output.Clear();
    if (input.Count() < 6)
    {
        return false;
    }
    const uint32 method = input[0];
    const uint32 flags = input[1];
    if ((method & 15) != 8 || (method >> 4) > 7 || ((method << 8) | flags) % 31 != 0 || (flags & 32) != 0)
    {
        return false; // Not deflate, a window over 32 KiB, a bad check or a preset dictionary.
    }
    if (expected != 0)
    {
        output.Reserve(expected);
    }
    const Span<const uint8> data = input.Slice(2);
    Bits bits(data);
    bool last = false;
    while (!last)
    {
        last = bits.Read(1) != 0;
        const uint32 type = bits.Read(2);
        bool decoded = false;
        if (type == 0)
        {
            bits.AlignToByte();
            const usize at = bits.GetBytePosition();
            if (at + 4 > data.Count())
            {
                return false;
            }
            const uint32 length = data[at] | (uint32{data[at + 1]} << 8);
            const uint32 inverse = data[at + 2] | (uint32{data[at + 3]} << 8);
            if ((length ^ inverse) != 0xFFFFu || at + 4 + length > data.Count())
            {
                return false;
            }
            output.AddRange(data.Slice(at + 4, length));
            bits.Seek(at + 4 + length);
            decoded = true;
        }
        else if (type == 1)
        {
            decoded = FixedBlock(bits, output);
        }
        else if (type == 2)
        {
            decoded = DynamicBlock(bits, output);
        }
        if (!decoded || bits.IsOverrun() || (expected != 0 && output.Count() > expected))
        {
            return false;
        }
    }
    bits.AlignToByte();
    const usize at = bits.GetBytePosition();
    if (at + 4 > data.Count())
    {
        return false;
    }
    const uint32 stored =
        (uint32{data[at]} << 24) | (uint32{data[at + 1]} << 16) | (uint32{data[at + 2]} << 8) | uint32{data[at + 3]};
    return Adler32(output.AsSpan()) == stored && (expected == 0 || output.Count() == expected);
}
} // namespace Marlin::Inflate
