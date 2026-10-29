//! Platform-accelerated cryptography: AES-256 in CTR mode, SHA-256 and ECDSA over P-256.
//!
//! Every function returns false instead of throwing when the platform call fails. Keys, digests and signatures
//! use the fixed big-endian layouts described by the size constants.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"

namespace Marlin::PlatformCrypto
{
/// The size of an AES-256 key, in bytes.
inline constexpr usize Aes256KeySize = 32;
/// The size of an AES block and of the CTR counter, in bytes.
inline constexpr usize AesBlockSize = 16;
/// The size of a SHA-256 digest, in bytes.
inline constexpr usize Sha256Size = 32;
/// The size of an ECDSA P-256 public key, in bytes: X then Y, big-endian.
inline constexpr usize EcdsaP256PublicKeySize = 64;
/// The size of an ECDSA P-256 private key, in bytes: X, Y, then the secret D, big-endian.
inline constexpr usize EcdsaP256PrivateKeySize = 96;
/// The size of an ECDSA P-256 signature, in bytes: R then S, big-endian.
inline constexpr usize EcdsaP256SignatureSize = 64;

/// Encrypts or decrypts `data` in place with AES-256 in CTR mode.
///
/// `key` is `Aes256KeySize` bytes, and `counter` is the big-endian 128-bit block counter of the first block; the
/// array is not modified. Encrypting and decrypting are the same call. The platform's hardware AES (BCrypt on
/// Windows) generates the keystream.
///
/// # Errors
/// Returns false when `key` is not `Aes256KeySize` bytes, which leaves `data` unchanged, or when the platform call
/// fails, which may leave `data` partly transformed.
///
/// # Performance
/// Allocates one buffer of up to 64 KiB plus one AES block per call, and processes the data in blocks of 64 KiB.
///
/// # Examples
///
/// ```cpp
/// uint8 key[PlatformCrypto::Aes256KeySize] = {};
/// const uint8 counter[PlatformCrypto::AesBlockSize] = {};
/// uint8 data[64] = {};
/// const bool ok = PlatformCrypto::Aes256Ctr(Span<const uint8>(key), counter, Span<uint8>(data)); // -> true
/// ```
CORE_API bool Aes256Ctr(Span<const uint8> key, const uint8 (&counter)[AesBlockSize], Span<uint8> data);

/// Computes the SHA-256 digest of `data` into `digest`.
///
/// # Errors
/// Returns false when the platform call fails.
CORE_API bool Sha256(Span<const uint8> data, uint8 (&digest)[Sha256Size]);

/// Generates a new ECDSA P-256 key pair from the platform's random source.
///
/// `privateKey` receives X, Y and D, so its first `EcdsaP256PublicKeySize` bytes are also the public key.
///
/// # Errors
/// Returns false when the platform call fails; `privateKey` is then unchanged.
CORE_API bool EcdsaP256Generate(uint8 (&privateKey)[EcdsaP256PrivateKeySize]);
/// Signs `digest` with `privateKey`, writing the R,S signature to `signature`.
///
/// `digest` is the SHA-256 hash of the message, not the message. `privateKey` is `EcdsaP256PrivateKeySize` bytes.
///
/// # Errors
/// Returns false when `privateKey` has the wrong size or the platform call fails.
CORE_API bool EcdsaP256Sign(Span<const uint8> privateKey, const uint8 (&digest)[Sha256Size],
                            uint8 (&signature)[EcdsaP256SignatureSize]);
/// Verifies that `signature` is `publicKey`'s signature over `digest`.
///
/// Returns false when the signature does not match, and when `publicKey` is not `EcdsaP256PublicKeySize` bytes or
/// `signature` is not `EcdsaP256SignatureSize` bytes.
///
/// # Examples
///
/// ```cpp
/// uint8 privateKey[PlatformCrypto::EcdsaP256PrivateKeySize];
/// uint8 digest[PlatformCrypto::Sha256Size] = {};
/// uint8 signature[PlatformCrypto::EcdsaP256SignatureSize];
/// if (PlatformCrypto::EcdsaP256Generate(privateKey))
/// {
///     const Span<const uint8> publicKey(privateKey, PlatformCrypto::EcdsaP256PublicKeySize);
///     const bool signedOk = PlatformCrypto::EcdsaP256Sign(Span<const uint8>(privateKey), digest, signature);
///     const bool valid = PlatformCrypto::EcdsaP256Verify(publicKey, digest, Span<const uint8>(signature)); // -> true
/// }
/// ```
CORE_API bool EcdsaP256Verify(Span<const uint8> publicKey, const uint8 (&digest)[Sha256Size],
                              Span<const uint8> signature);
} // namespace Marlin::PlatformCrypto
