#include "Vfs/PakSignature.h"

#include "HAL/PlatformFile.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
bool EcdsaP256Scheme::Generate(List<uint8> &privateKey, List<uint8> &publicKey) const
{
    uint8 key[PlatformCrypto::EcdsaP256PrivateKeySize];
    if (!PlatformCrypto::EcdsaP256Generate(key))
    {
        return false;
    }
    privateKey.Clear();
    privateKey.AddRange(Span<const uint8>(key));
    publicKey.Clear();
    publicKey.AddRange(Span<const uint8>(key, PlatformCrypto::EcdsaP256PublicKeySize));
    MemZero(key, sizeof(key));
    return true;
}

bool EcdsaP256Scheme::Sign(Span<const uint8> privateKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
                           List<uint8> &signature) const
{
    uint8 bytes[PlatformCrypto::EcdsaP256SignatureSize];
    if (!PlatformCrypto::EcdsaP256Sign(privateKey, digest, bytes))
    {
        return false;
    }
    signature.Clear();
    signature.AddRange(Span<const uint8>(bytes));
    return true;
}

bool EcdsaP256Scheme::Verify(Span<const uint8> publicKey, const uint8 (&digest)[PlatformCrypto::Sha256Size],
                             Span<const uint8> signature) const
{
    return PlatformCrypto::EcdsaP256Verify(publicKey, digest, signature);
}

namespace PakSigning
{
String SignaturePath(StringView packagePath)
{
    StringView stem = packagePath;
    if (packagePath.EndsWith(".mpak"))
    {
        stem = packagePath.Left(packagePath.Length() - 5);
    }
    String path(stem);
    path.Append(".msig");
    return path;
}

bool ReadSignedBytes(StringView path, List<uint8> &bytes)
{
    const String native(path);
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    PakHeader header;
    bool ok = PlatformFile::GetSize(file, size) && PlatformFile::ReadAt(file, 0, &header, sizeof(header), read) &&
              read == sizeof(header) && header.Magic == PakMagic;
    const struct
    {
        uint64 Offset;
        uint64 Count;
        uint64 EntrySize;
    } tables[] = {{header.BlockTableOffset, header.BlockCount, sizeof(PakBlock)},
                  {header.StringTableOffset, header.StringTableSize, 1},
                  {header.FileTableOffset, header.FileCount, sizeof(PakFileEntry)},
                  {header.AssetTableOffset, header.AssetCount, sizeof(PakAssetEntry)},
                  {header.DependencyTableOffset, header.DependencyCount, sizeof(Guid)}};
    bytes.Clear();
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&header), sizeof(header)));
    for (const auto &table : tables)
    {
        if (!ok)
        {
            break;
        }
        ok = table.Count <= size / table.EntrySize && table.Offset <= size &&
             table.Count * table.EntrySize <= size - table.Offset;
        const usize start = bytes.Count();
        const auto length = static_cast<usize>(table.Count * table.EntrySize);
        bytes.ResizeUninitialized(start + (ok ? length : 0));
        ok = ok && (length == 0 ||
                    (PlatformFile::ReadAt(file, table.Offset, bytes.Data() + start, length, read) && read == length));
    }
    PlatformFile::Close(file);
    return ok;
}

bool ComputeDigest(Span<const uint8> signedBytes, const PakSignatureHeader &signature, Span<const uint8> blockHashes,
                   uint8 (&digest)[PlatformCrypto::Sha256Size])
{
    const Span<const uint8> parts[] = {signedBytes};
    return ComputeDigest(Span<const Span<const uint8>>(parts), signature, blockHashes, digest);
}

bool ComputeDigest(Span<const Span<const uint8>> signedParts, const PakSignatureHeader &signature,
                   Span<const uint8> blockHashes, uint8 (&digest)[PlatformCrypto::Sha256Size])
{
    PakSignatureHeader cleared = signature;
    cleared.SignatureSize = 0;
    MemZero(cleared.Signature, sizeof(cleared.Signature));
    // PlatformCrypto hashes in one call, so the parts are gathered once into one message.
    usize size = sizeof(cleared) + blockHashes.Count();
    for (const Span<const uint8> part : signedParts)
    {
        size += part.Count();
    }
    List<uint8> message;
    message.Reserve(size);
    for (const Span<const uint8> part : signedParts)
    {
        message.AddRange(part);
    }
    message.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&cleared), sizeof(cleared)));
    message.AddRange(blockHashes);
    return PlatformCrypto::Sha256(message.AsSpan(), digest);
}
} // namespace PakSigning
} // namespace Marlin
