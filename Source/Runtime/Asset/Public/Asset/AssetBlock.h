#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "Reflection/TypeInfo.h"

namespace Marlin
{
class Asset;
class StringWriter;

/// "MABK" read as a little-endian uint32.
inline constexpr uint32 AssetBlockMagic = 0x4B42414Du;
inline constexpr uint32 AssetBlockVersion = 1;

/// The start of a converted asset (Documentation/Architecture/06 §4.1). Offsets are from the start of the block.
struct AssetBlockHeader
{
    uint32 Magic = AssetBlockMagic;
    uint32 Version = AssetBlockVersion;
    Guid Object;
    /// AssetBlock::GetTypeHash of the run-time type.
    uint64 TypeHash = 0;
    /// AssetBlock::GetLayoutHash of the run-time type when it was converted.
    uint64 LayoutHash = 0;
    uint32 BlockSize = 0;
    /// The alignment the block needs in memory.
    uint32 Alignment = 0;
    uint32 DependencyOffset = 0;
    uint32 DependencyCount = 0;
    uint32 ImageOffset = 0;
    uint32 ImageSize = 0;
    uint32 DataOffset = 0;
    uint32 DataSize = 0;
    uint32 FixupOffset = 0;
    uint32 FixupCount = 0;
    /// The size of the bulk data stored apart from the block, zero if there is none.
    uint64 BulkSize = 0;
};

/// What a fixup patches; stored in the low bits of its offset, which pointers keep 8-aligned.
enum class AssetFixupKind : uint8
{
    /// An offset into the block becomes a pointer.
    Pointer = 0,
    /// A dependency index becomes that dependency's slot.
    AssetRef = 1,
};

inline constexpr uint32 AssetFixupKindMask = 7;

/// Writes and checks blocks. Converters write them; the asset table reads them.
namespace AssetBlock
{
/// Identifies a run-time type in blocks: a hash of its full name.
[[nodiscard]] ASSET_API uint64 GetTypeHash(const TypeInfo &type);

/// A hash of what a block's image depends on: field names, types, offsets and sizes, nested types and enumerators.
[[nodiscard]] ASSET_API uint64 GetLayoutHash(const TypeInfo &type);

/// True if every field other than Transient ones can live in a memory image; otherwise writes one line per
/// offending field to *problems*, if given.
ASSET_API bool CheckImageSafe(const TypeInfo &type, StringWriter *problems = nullptr);

/// Writes *asset* as the block of *guid*: its image-safe fields with their arrays, strings and references.
/// *bulkSize* is recorded for the bulk data the caller stores apart.
ASSET_API bool Write(const Asset &asset, const Guid &guid, List<uint8> &block, uint64 bulkSize = 0);

/// Checks the header and that every part lies inside *block*; on failure writes why to *error*.
ASSET_API bool Validate(Span<const uint8> block, StringWriter &error);
} // namespace AssetBlock
} // namespace Marlin
