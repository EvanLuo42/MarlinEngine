#include "Vfs/PakCipher.h"

#include "HAL/PlatformCrypto.h"
#include "String/Ascii.h"

namespace Marlin
{
bool Aes256CtrCipher::Apply(Span<const uint8> key, uint64 salt, uint64 position, Span<uint8> data) const
{
    uint8 counter[PlatformCrypto::AesBlockSize];
    for (uint32 i = 0; i < 8; ++i)
    {
        counter[i] = static_cast<uint8>(salt >> (56 - 8 * i));
        counter[8 + i] = static_cast<uint8>((position << 32) >> (56 - 8 * i));
    }
    return PlatformCrypto::Aes256Ctr(key, counter, data);
}

namespace
{
/// The extension in *set* whose GetName is *name*, case-insensitively, or null.
template <typename T> const T *FindNamed(const ExtensionSet<T> &set, StringView name)
{
    for (const T *extension : set)
    {
        const StringView candidate = extension->GetName();
        if (candidate.Length() != name.Length())
        {
            continue;
        }
        usize i = 0;
        while (i < name.Length() && Ascii::ToLower(candidate[i]) == Ascii::ToLower(name[i]))
        {
            ++i;
        }
        if (i == name.Length())
        {
            return extension;
        }
    }
    return nullptr;
}
} // namespace

void PakCrypto::Build(const ExtensionContext &context)
{
    _ciphers.Build(context);
    _keys.Build(context);
    _schemes.Build(context);
    _verifyKeys.Build(context);
}

const PakCipher *PakCrypto::FindCipher(StringView name) const
{
    return FindNamed(_ciphers, name);
}

void PakCrypto::AddKey(const Guid &keyId, Span<const uint8> key)
{
    KnownKey &known = _known.Emplace();
    known.Id = keyId;
    known.Key.AddRange(key);
}

bool PakCrypto::GetKey(const Guid &keyId, List<uint8> &key) const
{
    for (const KnownKey &known : _known)
    {
        if (known.Id == keyId)
        {
            key.Clear();
            key.AddRange(known.Key.AsSpan());
            return true;
        }
    }
    for (const PakKeyProvider *provider : _keys)
    {
        key.Clear();
        if (provider->GetKey(keyId, key))
        {
            return true;
        }
    }
    return false;
}

const PakSignatureScheme *PakCrypto::FindScheme(StringView name) const
{
    return FindNamed(_schemes, name);
}

void PakCrypto::AddPublicKey(const Guid &keyId, Span<const uint8> key)
{
    KnownKey &known = _knownPublic.Emplace();
    known.Id = keyId;
    known.Key.AddRange(key);
}

bool PakCrypto::GetPublicKey(const Guid &keyId, List<uint8> &key) const
{
    for (const KnownKey &known : _knownPublic)
    {
        if (known.Id == keyId)
        {
            key.Clear();
            key.AddRange(known.Key.AsSpan());
            return true;
        }
    }
    for (const PakVerifyKeyProvider *provider : _verifyKeys)
    {
        key.Clear();
        if (provider->GetPublicKey(keyId, key))
        {
            return true;
        }
    }
    return false;
}

bool PakCrypto::RequiresSignatures() const
{
    return !_knownPublic.IsEmpty() || _verifyKeys.Count() != 0;
}
} // namespace Marlin
