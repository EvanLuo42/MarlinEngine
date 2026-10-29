#pragma once

#include "Compression/CompressionCodec.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "String/StringView.h"

namespace Marlin
{
/// A converted result as GlobalStore holds it (Documentation/Architecture/06 §3.3), the value under its result key:
///
///     ConvertedResultHeader                  fixed, at value offset 0
///     ConvertedResultBlock[BlockCount]       at value offset sizeof(ConvertedResultHeader)
///     the stored blocks, one after another   at value offset DataOffset
///
/// The *logical* stream the stored blocks decompress to holds the asset block at [AssetBlockOffset, +AssetBlockSize)
/// and the bulk at [BulkOffset, +BulkSize); the gap between them is zeros. ConvertedResultLocation (in
/// GlobalStoreAssetStore/AssetLocate.h, Asset.Locate's answer) gives the same logical positions. The logical stream is cut
/// into blocks of BlockSize bytes (the last may be shorter), each compressed on its own with Codec; a block that does
/// not shrink is stored as it is. A reader Gets the header and table once, then only the stored blocks a logical range
/// covers, which is an IRegionCompression over the value as in a package (02 §5.5). A process that does not link
/// Codec treats the result as missing and converts again. Everything is little-endian.
inline constexpr uint32 ConvertedResultMagic = 0x5243534Du; // "MSCR"
inline constexpr uint32 ConvertedResultVersion = 1;
/// Logical bytes per compressed block, as packages use (PakDefaultBlockSize).
inline constexpr uint32 ConvertedResultBlockSize = 64 << 10;
/// The bulk starts at this alignment in the logical stream.
inline constexpr uint64 ConvertedResultBulkAlignment = 16;

struct ConvertedResultHeader
{
    uint32 Magic = ConvertedResultMagic;
    uint32 Version = ConvertedResultVersion;
    /// The result key the value is stored under, so a misplaced value is caught.
    Hash128 Key;
    /// The asset block (Runtime/Asset AssetBlock) in the logical stream; AssetBlockOffset is 0 in version 1.
    uint64 AssetBlockOffset = 0;
    uint64 AssetBlockSize = 0;
    /// The bulk in the logical stream; both zero without bulk.
    uint64 BulkOffset = 0;
    uint64 BulkSize = 0;
    /// The size of the logical stream: the end of the bulk, or of the asset block without bulk.
    uint64 LogicalSize = 0;
    /// The CompressionCodec the compressed blocks use, zero padded; all zero when every block is stored as is.
    char Codec[16] = {};
    /// Logical bytes per block, ConvertedResultBlockSize when written; every block but the last holds this many.
    uint32 BlockSize = ConvertedResultBlockSize;
    uint32 BlockCount = 0;
    /// Where the stored blocks start in the value, after the block table.
    uint64 DataOffset = 0;
};

/// One entry of the block table.
struct ConvertedResultBlock
{
    /// Where the block's stored bytes are, from the start of the value.
    uint64 Offset = 0;
    uint32 StoredSize = 0;
    /// 1 for a block stored as it is, because compressing it did not make it smaller.
    uint8 Stored = 0;
    uint8 Reserved[3] = {};
};

static_assert(sizeof(ConvertedResultHeader) == 96);
static_assert(sizeof(ConvertedResultBlock) == 16);

/// Reads the header and block table of a converted result; what an IRegionCompression over one is built from.
class GLOBALSTORE_API ConvertedResultReader
{
public:
    /// Bytes to Get from offset 0 so that Parse sees the header and table of a result whose header says
    /// *blockCount*; Get sizeof(ConvertedResultHeader) first to learn it.
    [[nodiscard]] static uint64 GetTableEnd(uint32 blockCount)
    {
        return sizeof(ConvertedResultHeader) + uint64{blockCount} * sizeof(ConvertedResultBlock);
    }

    /// Parses *bytes*, the start of the value (at least GetTableEnd bytes) whose key is *key*; false if it is not a
    /// converted result of this version for that key, or its table does not describe a whole logical stream.
    bool Parse(const Hash128 &key, Span<const uint8> bytes);

    [[nodiscard]] const ConvertedResultHeader &GetHeader() const
    {
        return _header;
    }
    [[nodiscard]] StringView GetCodecName() const;
    /// The codec, from *codecs*; null if the header names none (every block stored) or *codecs* lacks it, in which
    /// case CanDecode is false.
    [[nodiscard]] const CompressionCodec *FindCodec(const CompressionCodecSet &codecs) const;
    [[nodiscard]] bool CanDecode(const CompressionCodecSet &codecs) const;

    /// Where logical block *index* is stored, its stored size and its logical size; false past the last block.
    bool GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const;
    [[nodiscard]] bool IsStored(uint64 index) const;
    /// Decompresses block *index* from *stored* into *output*, its logical size; a stored block is copied.
    bool DecompressBlock(const CompressionCodec *codec, uint64 index, Span<const uint8> stored,
                         Span<uint8> output) const;

    /// The whole logical stream of *value*, a complete converted result: what the Packager reads.
    static bool Decode(const CompressionCodecSet &codecs, const Hash128 &key, Span<const uint8> value,
                       List<uint8> &logical);

private:
    ConvertedResultHeader _header;
    List<ConvertedResultBlock> _blocks;
};

/// Writes converted results; the pipeline's side of the format.
namespace ConvertedResultWriter
{
/// The value of the result *key* holding *block* and *bulk*: each logical block compressed with *codec* at *level*
/// (null stores them as is), in parallel on the job system when it runs. *header*, if given, receives the header.
/// The value starts *prefix* bytes into *value*, room the caller fills, such as GlobalStoreClient::PutHeaderSize.
GLOBALSTORE_API void Write(const Hash128 &key, Span<const uint8> block, Span<const uint8> bulk,
                           const CompressionCodec *codec, int32 level, List<uint8> &value,
                           ConvertedResultHeader *header = nullptr, usize prefix = 0);
} // namespace ConvertedResultWriter
} // namespace Marlin
