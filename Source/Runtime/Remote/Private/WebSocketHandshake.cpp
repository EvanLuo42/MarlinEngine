#include "WebSocketHandshake.h"

#include "Memory/MemoryOps.h"
#include "String/StringBuilder.h"

namespace Marlin::WebSocketHandshake
{
namespace
{
constexpr uint32 RotateLeft(uint32 value, uint32 count)
{
    return (value << count) | (value >> (32 - count));
}

void Compress(uint32 (&state)[5], const uint8 *block)
{
    uint32 words[80];
    for (uint32 i = 0; i < 16; ++i)
    {
        const uint8 *word = block + static_cast<usize>(i) * 4;
        words[i] = (static_cast<uint32>(word[0]) << 24) | (static_cast<uint32>(word[1]) << 16) |
                   (static_cast<uint32>(word[2]) << 8) | word[3];
    }
    for (uint32 i = 16; i < 80; ++i)
    {
        words[i] = RotateLeft(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
    }
    uint32 a = state[0];
    uint32 b = state[1];
    uint32 c = state[2];
    uint32 d = state[3];
    uint32 e = state[4];
    for (uint32 i = 0; i < 80; ++i)
    {
        uint32 f = 0;
        uint32 k = 0;
        if (i < 20)
        {
            f = (b & c) | (~b & d);
            k = 0x5A827999;
        }
        else if (i < 40)
        {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1;
        }
        else if (i < 60)
        {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDC;
        }
        else
        {
            f = b ^ c ^ d;
            k = 0xCA62C1D6;
        }
        const uint32 next = RotateLeft(a, 5) + f + e + k + words[i];
        e = d;
        d = c;
        c = RotateLeft(b, 30);
        b = a;
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}
} // namespace

void Sha1(const void *data, usize size, uint8 (&digest)[20])
{
    uint32 state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const auto *bytes = static_cast<const uint8 *>(data);
    usize offset = 0;
    for (; offset + 64 <= size; offset += 64)
    {
        Compress(state, bytes + offset);
    }
    uint8 tail[128] = {};
    const usize remaining = size - offset;
    MemCopy(tail, bytes + offset, remaining);
    tail[remaining] = 0x80;
    const usize tailSize = remaining < 56 ? 64 : 128;
    const uint64 bits = static_cast<uint64>(size) * 8;
    for (usize i = 0; i < 8; ++i)
    {
        tail[tailSize - 1 - i] = static_cast<uint8>(bits >> (8 * i));
    }
    for (usize block = 0; block < tailSize; block += 64)
    {
        Compress(state, tail + block);
    }
    for (usize i = 0; i < 5; ++i)
    {
        digest[i * 4] = static_cast<uint8>(state[i] >> 24);
        digest[i * 4 + 1] = static_cast<uint8>(state[i] >> 16);
        digest[i * 4 + 2] = static_cast<uint8>(state[i] >> 8);
        digest[i * 4 + 3] = static_cast<uint8>(state[i]);
    }
}

void Base64(const void *data, usize size, StringWriter &output)
{
    constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto *bytes = static_cast<const uint8 *>(data);
    for (usize i = 0; i < size; i += 3)
    {
        const uint32 b0 = bytes[i];
        const uint32 b1 = i + 1 < size ? bytes[i + 1] : 0;
        const uint32 b2 = i + 2 < size ? bytes[i + 2] : 0;
        const uint32 triple = (b0 << 16) | (b1 << 8) | b2;
        output.Append(Alphabet[(triple >> 18) & 63]).Append(Alphabet[(triple >> 12) & 63]);
        output.Append(i + 1 < size ? Alphabet[(triple >> 6) & 63] : '=');
        output.Append(i + 2 < size ? Alphabet[triple & 63] : '=');
    }
}

void AcceptKey(StringView key, StringWriter &output)
{
    StringBuilder<128> text(key.Trim());
    text.Append("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    uint8 digest[20];
    Sha1(text.Data(), text.Length(), digest);
    Base64(digest, sizeof(digest), output);
}
} // namespace Marlin::WebSocketHandshake
