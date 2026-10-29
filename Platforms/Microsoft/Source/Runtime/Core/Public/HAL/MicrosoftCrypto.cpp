#include "HAL/PlatformCrypto.h"

#include "HAL/MicrosoftHeaders.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"

#include <bcrypt.h>

namespace Marlin::PlatformCrypto
{
namespace
{
constexpr usize ChunkBlocks = 4096; // Counter blocks per BCryptEncrypt call: 64 KB of keystream.

void Increment(uint8 (&counter)[AesBlockSize])
{
    for (usize i = AesBlockSize; i-- > 0;)
    {
        if (++counter[i] != 0)
        {
            break;
        }
    }
}
} // namespace

bool Aes256Ctr(Span<const uint8> key, const uint8 (&counter)[AesBlockSize], Span<uint8> data)
{
    if (key.Count() != Aes256KeySize)
    {
        return false;
    }
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(BCRYPT_AES_ECB_ALG_HANDLE, &handle, nullptr, 0,
                                                   const_cast<PUCHAR>(key.Data()), static_cast<ULONG>(key.Count()), 0)))
    {
        return false;
    }
    const usize chunkBytes =
        (data.Count() < ChunkBlocks * AesBlockSize ? data.Count() : ChunkBlocks * AesBlockSize) + AesBlockSize;
    auto *keystream = static_cast<uint8 *>(Memory::Allocate(chunkBytes));
    uint8 block[AesBlockSize];
    MemCopy(block, counter, AesBlockSize);
    bool ok = true;
    for (usize offset = 0; ok && offset < data.Count();)
    {
        const usize bytes =
            data.Count() - offset < chunkBytes - AesBlockSize ? data.Count() - offset : chunkBytes - AesBlockSize;
        const usize blocks = (bytes + AesBlockSize - 1) / AesBlockSize;
        for (usize i = 0; i < blocks; ++i)
        {
            MemCopy(keystream + i * AesBlockSize, block, AesBlockSize);
            Increment(block);
        }
        ULONG written = 0;
        const auto size = static_cast<ULONG>(blocks * AesBlockSize);
        ok = BCRYPT_SUCCESS(BCryptEncrypt(handle, keystream, size, nullptr, nullptr, 0, keystream, size, &written, 0));
        for (usize i = 0; ok && i < bytes; ++i)
        {
            data.Data()[offset + i] ^= keystream[i];
        }
        offset += bytes;
    }
    Memory::Free(keystream);
    BCryptDestroyKey(handle);
    return ok;
}

bool Sha256(Span<const uint8> data, uint8 (&digest)[Sha256Size])
{
    return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(data.Data()),
                                     static_cast<ULONG>(data.Count()), digest, Sha256Size));
}

namespace
{
constexpr usize EcdsaCoordinateSize = 32;

/// Imports *key*, the public or private key layout of PlatformCrypto.h, which is BCrypt's blob after its header.
BCRYPT_KEY_HANDLE ImportEcdsaKey(Span<const uint8> key, bool isPrivate)
{
    uint8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + EcdsaP256PrivateKeySize];
    auto *header = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob);
    header->dwMagic = isPrivate ? BCRYPT_ECDSA_PRIVATE_P256_MAGIC : BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = EcdsaCoordinateSize;
    MemCopy(blob + sizeof(BCRYPT_ECCKEY_BLOB), key.Data(), key.Count());
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr,
                                            isPrivate ? BCRYPT_ECCPRIVATE_BLOB : BCRYPT_ECCPUBLIC_BLOB, &handle, blob,
                                            static_cast<ULONG>(sizeof(BCRYPT_ECCKEY_BLOB) + key.Count()), 0)))
    {
        return nullptr;
    }
    return handle;
}
} // namespace

bool EcdsaP256Generate(uint8 (&privateKey)[EcdsaP256PrivateKeySize])
{
    BCRYPT_KEY_HANDLE handle = nullptr;
    if (!BCRYPT_SUCCESS(BCryptGenerateKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, &handle, 256, 0)))
    {
        return false;
    }
    uint8 blob[sizeof(BCRYPT_ECCKEY_BLOB) + EcdsaP256PrivateKeySize];
    ULONG written = 0;
    const bool ok = BCRYPT_SUCCESS(BCryptFinalizeKeyPair(handle, 0)) &&
                    BCRYPT_SUCCESS(BCryptExportKey(handle, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob, sizeof(blob),
                                                   &written, 0)) &&
                    written == sizeof(blob);
    if (ok)
    {
        MemCopy(privateKey, blob + sizeof(BCRYPT_ECCKEY_BLOB), EcdsaP256PrivateKeySize);
    }
    BCryptDestroyKey(handle);
    return ok;
}

bool EcdsaP256Sign(Span<const uint8> privateKey, const uint8 (&digest)[Sha256Size],
                   uint8 (&signature)[EcdsaP256SignatureSize])
{
    if (privateKey.Count() != EcdsaP256PrivateKeySize)
    {
        return false;
    }
    BCRYPT_KEY_HANDLE handle = ImportEcdsaKey(privateKey, true);
    if (handle == nullptr)
    {
        return false;
    }
    ULONG written = 0;
    const bool ok = BCRYPT_SUCCESS(BCryptSignHash(handle, nullptr, const_cast<PUCHAR>(digest), Sha256Size, signature,
                                                  EcdsaP256SignatureSize, &written, 0)) &&
                    written == EcdsaP256SignatureSize;
    BCryptDestroyKey(handle);
    return ok;
}

bool EcdsaP256Verify(Span<const uint8> publicKey, const uint8 (&digest)[Sha256Size], Span<const uint8> signature)
{
    if (publicKey.Count() != EcdsaP256PublicKeySize || signature.Count() != EcdsaP256SignatureSize)
    {
        return false;
    }
    BCRYPT_KEY_HANDLE handle = ImportEcdsaKey(publicKey, false);
    if (handle == nullptr)
    {
        return false;
    }
    const bool ok =
        BCRYPT_SUCCESS(BCryptVerifySignature(handle, nullptr, const_cast<PUCHAR>(digest), Sha256Size,
                                             const_cast<PUCHAR>(signature.Data()), EcdsaP256SignatureSize, 0));
    BCryptDestroyKey(handle);
    return ok;
}
} // namespace Marlin::PlatformCrypto
