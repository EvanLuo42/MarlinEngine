#include "Hash/Sha1.h"

#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
constexpr uint32 Rotate(uint32 value, uint32 bits)
{
    return (value << bits) | (value >> (32 - bits));
}
} // namespace

Sha1::Sha1()
{
    Reset();
}

void Sha1::Reset()
{
    _state[0] = 0x67452301u;
    _state[1] = 0xEFCDAB89u;
    _state[2] = 0x98BADCFEu;
    _state[3] = 0x10325476u;
    _state[4] = 0xC3D2E1F0u;
    _length = 0;
}

void Sha1::Compress(const uint8 *block)
{
    uint32 words[80];
    for (uint32 i = 0; i < 16; ++i)
    {
        words[i] = (static_cast<uint32>(block[i * 4]) << 24) | (static_cast<uint32>(block[i * 4 + 1]) << 16) |
                   (static_cast<uint32>(block[i * 4 + 2]) << 8) | static_cast<uint32>(block[i * 4 + 3]);
    }
    for (uint32 i = 16; i < 80; ++i)
    {
        words[i] = Rotate(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }
    uint32 a = _state[0];
    uint32 b = _state[1];
    uint32 c = _state[2];
    uint32 d = _state[3];
    uint32 e = _state[4];
    for (uint32 i = 0; i < 80; ++i)
    {
        uint32 f = 0;
        uint32 k = 0;
        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999u;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        const uint32 next = Rotate(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = Rotate(b, 30);
        b = a;
        a = next;
    }
    _state[0] += a;
    _state[1] += b;
    _state[2] += c;
    _state[3] += d;
    _state[4] += e;
}

void Sha1::Update(Span<const uint8> data)
{
    usize offset = 0;
    usize buffered = static_cast<usize>(_length % 64);
    _length += data.Count();
    if (buffered != 0)
    {
        const usize take = data.Count() < 64 - buffered ? data.Count() : 64 - buffered;
        MemCopy(_buffer + buffered, data.Data(), take);
        offset = take;
        buffered += take;
        if (buffered < 64)
        {
            return;
        }
        Compress(_buffer);
    }
    for (; offset + 64 <= data.Count(); offset += 64)
    {
        Compress(data.Data() + offset);
    }
    if (offset < data.Count())
    {
        MemCopy(_buffer, data.Data() + offset, data.Count() - offset);
    }
}

void Sha1::Finish(uint8 (&digest)[DigestSize])
{
    const uint64 bits = _length * 8;
    uint8 padding[72] = {0x80};
    const usize buffered = static_cast<usize>(_length % 64);
    const usize padLength = (buffered < 56 ? 56 : 120) - buffered;
    for (uint32 i = 0; i < 8; ++i)
    {
        padding[padLength + i] = static_cast<uint8>(bits >> (56 - 8 * i));
    }
    Update(Span<const uint8>(padding, padLength + 8));
    for (uint32 i = 0; i < 5; ++i)
    {
        digest[i * 4] = static_cast<uint8>(_state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8>(_state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8>(_state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8>(_state[i]);
    }
}

void Sha1::Hash(Span<const uint8> data, uint8 (&digest)[DigestSize])
{
    Sha1 hasher;
    hasher.Update(data);
    hasher.Finish(digest);
}
} // namespace Marlin
