#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "HAL/PlatformCrypto.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Vfs/PakFormat.h"
#include "Vfs/PakSignature.gen.h"

namespace Marlin
{
/// Signs packages (Documentation/Architecture/06 §5.3), chosen by name like PakCipher: the .msig names the scheme and
/// the key, and a process verifies with whatever scheme of that name is linked. Implementations are stateless and
/// safe from any thread.
MCLASS(Abstract, ExtensionPoint)
class VFS_API PakSignatureScheme
{
    MGENERATED_BODY()
public:
    PakSignatureScheme() = default;
    virtual ~PakSignatureScheme() = default;
    MARLIN_NONCOPYABLE(PakSignatureScheme);

    /// Such as "EcdsaP256"; written into .msig files, so it never changes.
    [[nodiscard]] virtual StringView GetName() const = 0;
    /// Makes a key pair: the private key the build machine signs with, and the public key the game verifies with.
    virtual bool Generate(List<uint8> &privateKey, List<uint8> &publicKey) const = 0;
    /// Signs *digest*, a SHA-256, into *signature*, at most sizeof(PakSignatureHeader::Signature) bytes.
    virtual bool Sign(Span<const uint8> privateKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
                      List<uint8> &signature) const = 0;
    [[nodiscard]] virtual bool Verify(Span<const uint8> publicKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
                                      Span<const uint8> signature) const = 0;
};

/// ECDSA over P-256 through the platform's crypto library.
MCLASS()
class VFS_API EcdsaP256Scheme final : public PakSignatureScheme
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "EcdsaP256";
    }
    bool Generate(List<uint8> &privateKey, List<uint8> &publicKey) const override;
    bool Sign(Span<const uint8> privateKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
              List<uint8> &signature) const override;
    [[nodiscard]] bool Verify(Span<const uint8> publicKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
                              Span<const uint8> signature) const override;
};

/// Hands out the public keys packages are verified with, by key id. Linking one makes a packaged game require signed
/// packages: Shipping mounts no other (02 §5). Keys never come from config or files beside the game, which players
/// can change; a product's provider holds them in the executable.
MCLASS(Abstract, ExtensionPoint)
class VFS_API PakVerifyKeyProvider
{
    MGENERATED_BODY()
public:
    PakVerifyKeyProvider() = default;
    virtual ~PakVerifyKeyProvider() = default;
    MARLIN_NONCOPYABLE(PakVerifyKeyProvider);

    /// Writes the public key *keyId* names into *key*; false if this provider does not have it.
    virtual bool GetPublicKey(const Guid &keyId, List<uint8> &key) const = 0;
};

namespace PakSigning
{
/// `<name>.msig` for package `<name>.mpak`.
[[nodiscard]] VFS_API String SignaturePath(StringView packagePath);
/// Reads what a signature covers from the package at native *path*: its header and tables as stored. False if the
/// file is not a package or its tables lie outside it.
VFS_API bool ReadSignedBytes(StringView path, List<uint8> &bytes);
/// The digest a signature signs: *signedBytes* from ReadSignedBytes, then *signature* with its Signature and
/// SignatureSize zeroed, then *blockHashes*.
VFS_API bool ComputeDigest(Span<const uint8> signedBytes, const PakSignatureHeader &signature,
                           Span<const uint8> blockHashes, uint8 (&digest)[PlatformCrypto::Sha256Size]);
/// The same with the signed bytes in parts, one after another, such as the header and tables a mount already holds;
/// they are gathered once into the hashed message, never into a copy of their own first.
VFS_API bool ComputeDigest(Span<const Span<const uint8>> signedParts, const PakSignatureHeader &signature,
                           Span<const uint8> blockHashes, uint8 (&digest)[PlatformCrypto::Sha256Size]);
} // namespace PakSigning
} // namespace Marlin
