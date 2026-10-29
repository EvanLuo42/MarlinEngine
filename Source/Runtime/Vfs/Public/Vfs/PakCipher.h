#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "Reflection/ExtensionSet.h"
#include "String/StringView.h"
#include "Vfs/PakSignature.h"
#include "Vfs/PakCipher.gen.h"

namespace Marlin
{
/// Encrypts packages (Documentation/Architecture/06 §5.2), chosen by name like CompressionCodec: the package header
/// names the cipher and the key, and a process decrypts with whatever cipher of that name is linked. Ciphers must
/// keep lengths, so the block table stays true. Implementations are stateless and safe from any thread.
MCLASS(Abstract, ExtensionPoint)
class VFS_API PakCipher
{
    MGENERATED_BODY()
public:
    PakCipher() = default;
    virtual ~PakCipher() = default;
    MARLIN_NONCOPYABLE(PakCipher);

    /// Such as "Aes256Ctr"; written into package headers, so it never changes.
    [[nodiscard]] virtual StringView GetName() const = 0;
    [[nodiscard]] virtual usize GetKeySize() const = 0;
    /// Encrypts or decrypts *data* in place, a whole piece of the package: a block, by index, or a table, at its
    /// Pak*TablePosition. *salt* is the package's PakHeader::Salt.
    virtual bool Apply(Span<const uint8> key, uint64 salt, uint64 position, Span<uint8> data) const = 0;
};

/// AES-256 in counter mode through the platform's crypto library. The counter block is the salt, then the position
/// shifted up 32 bits, big-endian: every piece of up to 64 GiB counts through its own range, so no two 16 bytes under
/// one key and salt share a counter.
MCLASS()
class VFS_API Aes256CtrCipher final : public PakCipher
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "Aes256Ctr";
    }
    [[nodiscard]] usize GetKeySize() const override
    {
        return 32;
    }
    bool Apply(Span<const uint8> key, uint64 salt, uint64 position, Span<uint8> data) const override;
};

/// Hands out the keys of encrypted packages by key id. Keys never come from config: a product supplies a provider
/// that holds or derives them, from the executable or a platform's secure storage.
MCLASS(Abstract, ExtensionPoint)
class VFS_API PakKeyProvider
{
    MGENERATED_BODY()
public:
    PakKeyProvider() = default;
    virtual ~PakKeyProvider() = default;
    MARLIN_NONCOPYABLE(PakKeyProvider);

    /// Writes the key *keyId* names into *key*; false if this provider does not have it.
    virtual bool GetKey(const Guid &keyId, List<uint8> &key) const = 0;
};

/// The ciphers, signature schemes and key providers linked into the process, plus keys given directly; what
/// PakFile::Open decrypts and verifies with.
class VFS_API PakCrypto
{
public:
    PakCrypto() = default;
    MARLIN_NONCOPYABLE(PakCrypto);

    void Build(const ExtensionContext &context);
    /// The cipher named *name*, case-insensitively, or null; like CompressionCodecSet::Find. Immutable once built
    /// and its keys added.
    [[nodiscard]] const PakCipher *FindCipher(StringView name) const;
    /// Adds a key directly; GetKey returns it before asking the providers.
    void AddKey(const Guid &keyId, Span<const uint8> key);
    /// The key *keyId* names, from the added keys or else the first provider that has it.
    bool GetKey(const Guid &keyId, List<uint8> &key) const;

    /// The signature scheme named *name*, case-insensitively, or null.
    [[nodiscard]] const PakSignatureScheme *FindScheme(StringView name) const;
    /// Adds a public key directly; GetPublicKey returns it before asking the providers.
    void AddPublicKey(const Guid &keyId, Span<const uint8> key);
    /// The public key *keyId* names, from the added keys or else the first PakVerifyKeyProvider that has it.
    bool GetPublicKey(const Guid &keyId, List<uint8> &key) const;
    /// Whether packages must be signed: a PakVerifyKeyProvider is linked or a public key was added.
    [[nodiscard]] bool RequiresSignatures() const;

private:
    struct KnownKey
    {
        Guid Id;
        List<uint8> Key;
    };

    ExtensionSet<PakCipher> _ciphers;
    ExtensionSet<PakKeyProvider> _keys;
    ExtensionSet<PakSignatureScheme> _schemes;
    ExtensionSet<PakVerifyKeyProvider> _verifyKeys;
    List<KnownKey> _known;
    List<KnownKey> _knownPublic;
};
} // namespace Marlin
