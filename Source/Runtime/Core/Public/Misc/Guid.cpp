#include "Misc/Guid.h"

#include "HAL/PlatformSystem.h"
#include "Hash/Sha1.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
Guid Guid::NewGuid()
{
    Guid guid;
    PlatformSystem::GetSecureRandomBytes(&guid, sizeof(guid));
    guid.B = (guid.B & 0xFFFF0FFFu) | 0x00004000u;
    guid.C = (guid.C & 0x3FFFFFFFu) | 0x80000000u;
    return guid;
}

Guid Guid::FromName(const Guid &space, StringView name)
{
    uint8 bytes[16];
    const uint32 words[4] = {space.A, space.B, space.C, space.D};
    for (uint32 i = 0; i < 16; ++i)
    {
        bytes[i] = static_cast<uint8>(words[i / 4] >> (24 - 8 * (i % 4)));
    }
    Sha1 hasher;
    hasher.Update(Span<const uint8>(bytes, 16));
    hasher.Update(Span<const uint8>(reinterpret_cast<const uint8 *>(name.Data()), name.Length()));
    uint8 digest[Sha1::DigestSize];
    hasher.Finish(digest);
    uint32 result[4];
    for (uint32 i = 0; i < 4; ++i)
    {
        result[i] = (static_cast<uint32>(digest[i * 4]) << 24) | (static_cast<uint32>(digest[i * 4 + 1]) << 16) |
                    (static_cast<uint32>(digest[i * 4 + 2]) << 8) | static_cast<uint32>(digest[i * 4 + 3]);
    }
    return {result[0], (result[1] & 0xFFFF0FFFu) | 0x00005000u, (result[2] & 0x3FFFFFFFu) | 0x80000000u, result[3]};
}

void Guid::FormatTo(StringWriter &output, StringView spec) const
{
    const bool upper = !spec.IsEmpty() && Ascii::IsUpper(spec[0]);
    const bool hyphens = spec.IsEmpty() || Ascii::ToUpper(spec[0]) != 'N';
    char digits[32];
    const uint32 words[4] = {A, B, C, D};
    for (usize i = 0; i < 32; ++i)
    {
        const uint32 nibble = (words[i / 8] >> (28 - 4 * (i % 8))) & 0xF;
        digits[i] = (upper ? "0123456789ABCDEF" : "0123456789abcdef")[nibble];
    }
    if (!hyphens)
    {
        output.Append(StringView(digits, 32));
        return;
    }
    output.Append(StringView(digits, 8)).Append('-');
    output.Append(StringView(digits + 8, 4)).Append('-');
    output.Append(StringView(digits + 12, 4)).Append('-');
    output.Append(StringView(digits + 16, 4)).Append('-');
    output.Append(StringView(digits + 20, 12));
}
} // namespace Marlin
