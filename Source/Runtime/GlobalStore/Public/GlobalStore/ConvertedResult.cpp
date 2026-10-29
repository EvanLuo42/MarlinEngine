#include "GlobalStore/ConvertedResult.h"

#include "Concurrency/Jobs.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
namespace
{
usize CodecLength(const char *codec)
{
    usize length = 0;
    while (length < 16 && codec[length] != '\0')
    {
        ++length;
    }
    return length;
}

/// Copies logical bytes [*offset*, +*size*) of the stream *block*, zeros, *bulk* at *bulkOffset* into *out*.
void CopyLogical(Span<const uint8> block, Span<const uint8> bulk, uint64 bulkOffset, uint64 offset, usize size,
                 uint8 *out)
{
    const uint64 end = offset + size;
    MemZero(out, size);
    if (offset < block.Count())
    {
        const uint64 last = end < block.Count() ? end : block.Count();
        MemCopy(out, block.Data() + offset, static_cast<usize>(last - offset));
    }
    if (!bulk.IsEmpty() && end > bulkOffset && offset < bulkOffset + bulk.Count())
    {
        const uint64 first = offset > bulkOffset ? offset : bulkOffset;
        const uint64 last = end < bulkOffset + bulk.Count() ? end : bulkOffset + bulk.Count();
        MemCopy(out + (first - offset), bulk.Data() + (first - bulkOffset), static_cast<usize>(last - first));
    }
}

/// One logical block as a job compresses it.
struct CompressJob
{
    Span<const uint8> Block;
    Span<const uint8> Bulk;
    uint64 BulkOffset = 0;
    uint64 Offset = 0;
    usize Size = 0;
    const CompressionCodec *Codec = nullptr;
    int32 Level = 0;
    /// The compressed bytes; empty when the block is stored as is.
    List<uint8> Compressed;
};

void RunCompress(void *argument)
{
    auto *job = static_cast<CompressJob *>(argument);
    List<uint8> logical;
    logical.ResizeUninitialized(job->Size);
    CopyLogical(job->Block, job->Bulk, job->BulkOffset, job->Offset, job->Size, logical.Data());
    if (!job->Codec->Compress(logical.AsSpan(), job->Level, job->Compressed) || job->Compressed.Count() >= job->Size)
    {
        job->Compressed.Clear();
    }
}
} // namespace

bool ConvertedResultReader::Parse(const Hash128 &key, Span<const uint8> bytes)
{
    _blocks.Clear();
    if (bytes.Count() < sizeof(ConvertedResultHeader))
    {
        return false;
    }
    MemCopy(&_header, bytes.Data(), sizeof(_header));
    const ConvertedResultHeader &header = _header;
    if (header.Magic != ConvertedResultMagic || header.Version != ConvertedResultVersion || header.Key != key ||
        header.BlockSize == 0 || bytes.Count() < GetTableEnd(header.BlockCount) ||
        header.DataOffset < GetTableEnd(header.BlockCount) ||
        uint64{header.BlockCount} * header.BlockSize < header.LogicalSize ||
        (header.BlockCount != 0 && uint64{header.BlockCount - 1} * header.BlockSize >= header.LogicalSize) ||
        header.AssetBlockOffset + header.AssetBlockSize > header.LogicalSize ||
        header.BulkOffset + header.BulkSize > header.LogicalSize)
    {
        return false;
    }
    _blocks.Resize(header.BlockCount);
    MemCopy(_blocks.Data(), bytes.Data() + sizeof(ConvertedResultHeader),
            usize{header.BlockCount} * sizeof(ConvertedResultBlock));
    for (uint64 i = 0; i < _blocks.Count(); ++i)
    {
        uint64 offset = 0;
        uint32 stored = 0;
        uint32 logical = 0;
        (void)GetBlock(i, offset, stored, logical);
        if (offset < header.DataOffset || (_blocks[i].Stored != 0 && stored != logical) || stored > logical)
        {
            _blocks.Clear();
            return false;
        }
    }
    return true;
}

StringView ConvertedResultReader::GetCodecName() const
{
    return StringView(_header.Codec, CodecLength(_header.Codec));
}

const CompressionCodec *ConvertedResultReader::FindCodec(const CompressionCodecSet &codecs) const
{
    const StringView name = GetCodecName();
    return name.IsEmpty() ? nullptr : codecs.Find(name);
}

bool ConvertedResultReader::CanDecode(const CompressionCodecSet &codecs) const
{
    return GetCodecName().IsEmpty() || FindCodec(codecs) != nullptr;
}

bool ConvertedResultReader::GetBlock(uint64 index, uint64 &offset, uint32 &storedSize, uint32 &logicalSize) const
{
    if (index >= _blocks.Count())
    {
        return false;
    }
    const uint64 start = index * _header.BlockSize;
    const uint64 remaining = _header.LogicalSize - start;
    logicalSize = static_cast<uint32>(remaining < _header.BlockSize ? remaining : _header.BlockSize);
    offset = _blocks[static_cast<usize>(index)].Offset;
    storedSize = _blocks[static_cast<usize>(index)].StoredSize;
    return true;
}

bool ConvertedResultReader::IsStored(uint64 index) const
{
    return index < _blocks.Count() && _blocks[static_cast<usize>(index)].Stored != 0;
}

bool ConvertedResultReader::DecompressBlock(const CompressionCodec *codec, uint64 index, Span<const uint8> stored,
                                            Span<uint8> output) const
{
    if (IsStored(index))
    {
        if (stored.Count() != output.Count())
        {
            return false;
        }
        MemCopy(output.Data(), stored.Data(), stored.Count());
        return true;
    }
    return codec != nullptr && codec->Decompress(stored, output);
}

bool ConvertedResultReader::Decode(const CompressionCodecSet &codecs, const Hash128 &key, Span<const uint8> value,
                                   List<uint8> &logical)
{
    ConvertedResultReader reader;
    if (!reader.Parse(key, value) || !reader.CanDecode(codecs))
    {
        return false;
    }
    const CompressionCodec *codec = reader.FindCodec(codecs);
    logical.ResizeUninitialized(static_cast<usize>(reader._header.LogicalSize));
    for (uint64 i = 0; i < reader._blocks.Count(); ++i)
    {
        uint64 offset = 0;
        uint32 stored = 0;
        uint32 size = 0;
        (void)reader.GetBlock(i, offset, stored, size);
        if (offset + stored > value.Count() ||
            !reader.DecompressBlock(codec, i, value.Slice(static_cast<usize>(offset), stored),
                                    logical.AsSpan().Slice(static_cast<usize>(i * reader._header.BlockSize), size)))
        {
            logical.Clear();
            return false;
        }
    }
    return true;
}

namespace ConvertedResultWriter
{
void Write(const Hash128 &key, Span<const uint8> block, Span<const uint8> bulk, const CompressionCodec *codec,
           int32 level, List<uint8> &value, ConvertedResultHeader *written, usize prefix)
{
    ConvertedResultHeader header;
    header.Key = key;
    header.AssetBlockSize = block.Count();
    if (!bulk.IsEmpty())
    {
        header.BulkOffset = (block.Count() + ConvertedResultBulkAlignment - 1) & ~(ConvertedResultBulkAlignment - 1);
        header.BulkSize = bulk.Count();
    }
    header.LogicalSize = bulk.IsEmpty() ? block.Count() : header.BulkOffset + bulk.Count();
    header.BlockCount = static_cast<uint32>((header.LogicalSize + header.BlockSize - 1) / header.BlockSize);
    header.DataOffset = ConvertedResultReader::GetTableEnd(header.BlockCount);

    List<CompressJob> compress;
    compress.Resize(header.BlockCount);
    for (uint32 i = 0; i < header.BlockCount; ++i)
    {
        CompressJob &job = compress[i];
        job.Block = block;
        job.Bulk = bulk;
        job.BulkOffset = header.BulkOffset;
        job.Offset = uint64{i} * header.BlockSize;
        const uint64 remaining = header.LogicalSize - job.Offset;
        job.Size = static_cast<usize>(remaining < header.BlockSize ? remaining : header.BlockSize);
        job.Codec = codec;
        job.Level = level;
    }
    if (codec != nullptr)
    {
        List<JobDecl> jobs;
        jobs.Reserve(compress.Count());
        for (CompressJob &job : compress)
        {
            jobs.Add(JobDecl{.Entry = &RunCompress, .Argument = &job});
        }
        JobCounter counter;
        Jobs::Run(jobs.AsSpan(), &counter);
        Jobs::Wait(counter);
    }

    bool anyCompressed = false;
    uint64 size = header.DataOffset;
    for (const CompressJob &job : compress)
    {
        size += job.Compressed.IsEmpty() ? job.Size : job.Compressed.Count();
        anyCompressed = anyCompressed || !job.Compressed.IsEmpty();
    }
    if (anyCompressed)
    {
        const StringView name = codec->GetName();
        MemCopy(header.Codec, name.Data(), name.Length() < sizeof(header.Codec) ? name.Length() : sizeof(header.Codec));
    }
    value.Clear();
    value.ResizeUninitialized(prefix + static_cast<usize>(size));
    uint8 *base = value.Data() + prefix;
    MemCopy(base, &header, sizeof(header));
    uint64 at = header.DataOffset;
    for (uint32 i = 0; i < header.BlockCount; ++i)
    {
        const CompressJob &job = compress[i];
        ConvertedResultBlock entry;
        entry.Offset = at;
        if (job.Compressed.IsEmpty())
        {
            entry.StoredSize = static_cast<uint32>(job.Size);
            entry.Stored = 1;
            CopyLogical(block, bulk, header.BulkOffset, job.Offset, job.Size, base + at);
        }
        else
        {
            entry.StoredSize = static_cast<uint32>(job.Compressed.Count());
            MemCopy(base + at, job.Compressed.Data(), job.Compressed.Count());
        }
        MemCopy(base + sizeof(header) + usize{i} * sizeof(ConvertedResultBlock), &entry, sizeof(entry));
        at += entry.StoredSize;
    }
    if (written != nullptr)
    {
        *written = header;
    }
}
} // namespace ConvertedResultWriter
} // namespace Marlin
