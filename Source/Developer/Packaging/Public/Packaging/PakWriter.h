#pragma once

#include "Compression/CompressionCodec.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Vfs/PakCipher.h"
#include "Vfs/PakFormat.h"

namespace Marlin
{
class StringWriter;

/// How a package is encrypted (Documentation/Architecture/06 §5.2): the cipher, the id of the key it names in the
/// header, and the key itself, which only the packager holds.
struct PakEncryption
{
    const PakCipher *Cipher = nullptr;
    Guid KeyId;
    List<uint8> Key;
};

/// Builds one package (PakFormat.h, Documentation/Architecture/06 §5) in memory and writes it: files and assets go
/// into the logical stream in the order added, so that a load order lays assets out as the game reads them, then the
/// stream is cut into blocks compressed in parallel with the package's one codec, and encrypted if asked.
class PACKAGING_API PakWriter
{
public:
    /// *codec* null stores every block; *level* is the codec's. *encryption*, if not null, must outlive the writer.
    PakWriter(StringView chunk, const CompressionCodec *codec, int32 level, uint32 blockSize = PakDefaultBlockSize,
              const PakEncryption *encryption = nullptr);
    MARLIN_NONCOPYABLE(PakWriter);

    void AddFile(StringView mountPoint, StringView layer, StringView path, Span<const uint8> bytes);
    /// *dependencies* are the Guids the block references, in its own order, which the package lists so loads can
    /// start them before the block is read (PakAssetEntry).
    void AddAsset(const Guid &uuid, Span<const uint8> block, Span<const uint8> bulk,
                  Span<const Guid> dependencies = {});
    /// The same with block and bulk already one after another in *content*, the block its first *blockSize* bytes,
    /// taken without a copy.
    void AddAsset(const Guid &uuid, List<uint8> content, usize blockSize, Span<const Guid> dependencies = {});
    /// For a patch: entries that tell mounts and stores below the package the release removed a file or object.
    void AddDeletedFile(StringView mountPoint, StringView layer, StringView path);
    void AddDeletedAsset(const Guid &uuid);

    [[nodiscard]] bool IsEmpty() const
    {
        return _files.IsEmpty() && _assets.IsEmpty();
    }

    /// Writes the package to native path *path* through a temporary file and a rename. With *blockHashes*, also
    /// gives the SHA-256 of each block as stored, as a signature covers them (PakFormat.h), worked out while the
    /// blocks are compressed instead of by reading the package back.
    bool Write(StringView path, StringWriter &error, List<uint8> *blockHashes = nullptr) const;

private:
    struct File
    {
        String MountPoint;
        String Layer;
        String Path;
        List<uint8> Bytes;
        bool Deleted = false;
    };
    struct Asset
    {
        Guid Uuid;
        /// The block, then the bulk.
        List<uint8> Content;
        usize BlockSize = 0;
        List<Guid> Dependencies;
        bool Deleted = false;
    };

    String _chunk;
    const CompressionCodec *_codec;
    int32 _level;
    uint32 _blockSize;
    const PakEncryption *_encryption;
    List<File> _files;
    List<Asset> _assets;
};
} // namespace Marlin
