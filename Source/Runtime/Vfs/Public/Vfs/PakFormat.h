#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Misc/Guid.h"

namespace Marlin
{
/// A package (.mpak): the files and converted objects of one chunk, which MarlinPackager writes and PakFile mounts.
/// It starts with a PakHeader; offsets count from the start of the file and every field is stored as laid out here
/// (Documentation/Architecture/06 §5), little-endian. After the header comes the data: a
/// logical stream cut into blocks of BlockSize bytes, each compressed on its own with the named codec, or stored when
/// that does not make it smaller. Then come the tables, which are never compressed: blocks, strings, files, assets,
/// dependencies.
inline constexpr uint32 PakMagic = 0x4B41504Du;
inline constexpr uint32 PakVersion = 1;
inline constexpr uint32 PakDefaultBlockSize = 64 << 10;

struct PakHeader
{
    uint32 Magic = PakMagic;
    uint32 Version = PakVersion;
    uint32 BlockSize = PakDefaultBlockSize;
    uint32 Reserved = 0;
    /// The CompressionCodec name, zero-padded, that the blocks are compressed with (Documentation/Architecture/06
    /// §5.1); empty if every block is stored.
    char Codec[16] = {};
    /// The chunk, such as Base, Language_zh-Hans or Patch_0003, zero-padded.
    char Chunk[32] = {};
    uint64 LogicalSize = 0;
    uint64 BlockTableOffset = 0;
    uint64 BlockCount = 0;
    uint64 StringTableOffset = 0;
    uint64 StringTableSize = 0;
    uint64 FileTableOffset = 0;
    uint64 FileCount = 0;
    uint64 AssetTableOffset = 0;
    uint64 AssetCount = 0;
    /// The PakCipher the package is encrypted with, zero-padded, and the key it asks PakKeyProvider for; empty if
    /// it is not encrypted.
    char Cipher[16] = {};
    Guid KeyId;
    /// Random per package, so two packages under one key never share a key stream.
    uint64 Salt = 0;
    /// The Guids assets reference, which PakAssetEntry ranges index into.
    uint64 DependencyTableOffset = 0;
    uint64 DependencyCount = 0;
};

/// Where the cipher's position counts from for each table; blocks use their index.
inline constexpr uint64 PakBlockTablePosition = 0xFFFFFF00u;
inline constexpr uint64 PakStringTablePosition = 0xFFFFFF01u;
inline constexpr uint64 PakFileTablePosition = 0xFFFFFF02u;
inline constexpr uint64 PakAssetTablePosition = 0xFFFFFF03u;
inline constexpr uint64 PakDependencyTablePosition = 0xFFFFFF04u;

/// PakFileEntry::Flags: a patch removes the file; mounts below the package no longer show it.
inline constexpr uint32 PakFileDeleted = 1;

struct PakBlock
{
    uint64 Offset = 0;
    uint32 StoredSize = 0;
    /// True for a block stored as it is, because compressing did not make it smaller.
    uint8 Stored = 0;
    uint8 Reserved[3] = {};
};

/// A string in the string table.
struct PakString
{
    uint32 Offset = 0;
    uint32 Length = 0;
};

/// A file under a mount point, such as /Engine, in a platform layer, such as Windows, or in the root layer when Layer
/// is empty; Path is relative to the mount point or layer. Sorted by mount point, layer and path.
struct PakFileEntry
{
    PakString MountPoint;
    PakString Layer;
    PakString Path;
    uint32 Flags = 0;
    uint64 Offset = 0;
    uint64 Size = 0;
    Hash128 ContentHash;
};

/// The converted data of an object: its block and bulk in the logical stream. Sorted by Uuid. A patch writes an entry
/// whose BlockSize is zero for an object the release removed: stores below the package no longer find it.
struct PakAssetEntry
{
    Guid Uuid;
    uint64 BlockOffset = 0;
    uint64 BlockSize = 0;
    uint64 BulkOffset = 0;
    uint64 BulkSize = 0;
    /// Of block and bulk together; patches compare it with the previous release.
    Hash128 ContentHash;
    /// The Guids its block references, a range of the dependency table, so a load can start them before the block
    /// arrives; the block's own list stays what fixups use.
    uint32 DependencyIndex = 0;
    uint32 DependencyCount = 0;
};

/// A signed package has `<name>.msig` beside `<name>.mpak` (Documentation/Architecture/06 §5.3): a PakSignatureHeader,
/// then the SHA-256 of each block's stored bytes, in block order. The signature covers the package's header and
/// tables as stored, this header with Signature and SignatureSize zeroed, and the block hashes; reads check each
/// block's hash, so a package verifies without reading all of it at mount time.
inline constexpr uint32 PakSignatureMagic = 0x4749534Du;
inline constexpr uint32 PakSignatureVersion = 1;
inline constexpr usize PakBlockHashSize = 32;

struct PakSignatureHeader
{
    uint32 Magic = PakSignatureMagic;
    uint32 Version = PakSignatureVersion;
    /// The PakSignatureScheme that made Signature, zero-padded.
    char Scheme[16] = {};
    /// The key the game's PakVerifyKeyProvider hands out to verify it.
    Guid KeyId;
    uint64 BlockCount = 0;
    uint32 SignatureSize = 0;
    uint32 Reserved = 0;
    uint8 Signature[128] = {};
};

static_assert(sizeof(PakHeader) == 192);
static_assert(sizeof(PakSignatureHeader) == 184);
static_assert(sizeof(PakBlock) == 16);
static_assert(sizeof(PakFileEntry) == 64);
static_assert(sizeof(PakAssetEntry) == 72);
} // namespace Marlin
