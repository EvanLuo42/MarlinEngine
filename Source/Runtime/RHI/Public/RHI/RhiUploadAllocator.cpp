#include "RHI/RhiUploadAllocator.h"

#include "Concurrency/Jobs.h"
#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiLog.h"

namespace Marlin
{
namespace
{
bool IsComplete(const RhiQueueValues &retiredAt, const RhiQueueValues &completed)
{
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        if (retiredAt.Values[queue] > completed.Values[queue])
        {
            return false;
        }
    }
    return true;
}
} // namespace

RhiUploadAllocator::RhiUploadAllocator(RhiDevice &device, uint64 chunkSize)
    : _device(device), _chunkSize(chunkSize), _blockSize(chunkSize / 8 < 64 * 1024 ? chunkSize / 8 : 64 * 1024)
{
}

RhiUploadAllocator::~RhiUploadAllocator()
{
    const auto destroy = [this](const Chunk &chunk) {
        if (chunk.Buffer.IsValid())
        {
            _device.Destroy(chunk.Buffer);
        }
    };
    destroy(_current);
    for (const Chunk &chunk : _used)
    {
        destroy(chunk);
    }
    for (const Chunk &chunk : _retired)
    {
        destroy(chunk);
    }
}

bool RhiUploadAllocator::NewChunk(uint64 size)
{
    const RhiQueueValues completed = _device.GetCompletedValues();
    for (usize i = 0; i < _retired.Count(); ++i)
    {
        if (!_retired[i].Oversized && _retired[i].Size >= size && IsComplete(_retired[i].RetiredAt, completed))
        {
            _current = _retired[i];
            _retired.RemoveAtSwap(i);
            _currentOffset = 0;
            return true;
        }
    }
    RhiBufferDesc desc;
    desc.Size = size;
    desc.Usage = RhiBufferUsage::ShaderResource | RhiBufferUsage::CopySource | RhiBufferUsage::IndexBuffer |
                 RhiBufferUsage::IndirectArguments | RhiBufferUsage::AccelerationStructureBuildInput;
    desc.Location = RhiMemoryLocation::Upload;
    desc.DebugName = "UploadChunk";
    Result<RhiBuffer, RhiError> buffer = _device.CreateBuffer(desc);
    if (buffer.IsError())
    {
        MARLIN_LOG(LogRhi, Error, "Cannot allocate {0} bytes of upload memory: {1}", size,
                   GetErrorName(buffer.Error()));
        return false;
    }
    _current = {};
    _current.Buffer = buffer.Value();
    _current.Data = _device.GetMappedData(_current.Buffer);
    _current.Address = _device.GetBufferAddress(_current.Buffer);
    _current.Size = size;
    _currentOffset = 0;
    return true;
}

RhiUploadAllocation RhiUploadAllocator::Allocate(uint64 size, uint64 alignment)
{
    if (size > _blockSize / 4)
    {
        const ScopeLock lock(_lock);
        return AllocateShared(size, alignment);
    }
    const int32 worker = Jobs::GetWorkerIndex();
    Shard &shard = _shards[worker < 0 ? 0 : 1 + static_cast<uint32>(worker) % (ShardCount - 1)];
    const ScopeLock lock(shard.Lock);
    uint64 offset = (shard.Cursor + alignment - 1) & ~(alignment - 1);
    if (!shard.Buffer.IsValid() || offset + size > shard.End)
    {
        RhiUploadAllocation block;
        {
            const ScopeLock shared(_lock);
            block = AllocateShared(_blockSize, alignment > 256 ? alignment : 256);
        }
        if (block.Data == nullptr)
        {
            return {};
        }
        shard.Buffer = block.Buffer;
        shard.ChunkData = block.Data - block.Offset;
        shard.ChunkAddress = block.Address - block.Offset;
        shard.End = block.Offset + block.Size;
        offset = block.Offset;
    }
    shard.Cursor = offset + size;
    return {shard.ChunkData + offset, shard.Buffer, offset, shard.ChunkAddress + offset, size};
}

RhiUploadAllocation RhiUploadAllocator::AllocateShared(uint64 size, uint64 alignment)
{
    if (size > _chunkSize)
    {
        Chunk previous = _current;
        const uint64 previousOffset = _currentOffset;
        if (!NewChunk(size))
        {
            return {};
        }
        Chunk oversized = _current;
        oversized.Oversized = true;
        _used.Add(oversized);
        _current = previous;
        _currentOffset = previousOffset;
        return {oversized.Data, oversized.Buffer, 0, oversized.Address, size};
    }
    uint64 offset = (_currentOffset + alignment - 1) & ~(alignment - 1);
    if (!_current.Buffer.IsValid() || offset + size > _current.Size)
    {
        if (_current.Buffer.IsValid())
        {
            _used.Add(_current);
            _current = {};
        }
        if (!NewChunk(_chunkSize))
        {
            return {};
        }
        offset = 0;
    }
    _currentOffset = offset + size;
    return {_current.Data + offset, _current.Buffer, offset, _current.Address + offset, size};
}

void RhiUploadAllocator::EndFrame()
{
    // The shards' blocks are in chunks retired below.
    for (Shard &shard : _shards)
    {
        const ScopeLock lock(shard.Lock);
        shard.Buffer = {};
        shard.Cursor = 0;
        shard.End = 0;
    }
    const ScopeLock lock(_lock);
    const RhiQueueValues submitted = _device.GetSubmittedValues();
    if (_current.Buffer.IsValid() && _currentOffset > 0)
    {
        _used.Add(_current);
        _current = {};
        _currentOffset = 0;
    }
    const RhiQueueValues completed = _device.GetCompletedValues();
    usize kept = 0;
    for (usize i = 0; i < _retired.Count(); ++i)
    {
        if (_retired[i].Oversized && IsComplete(_retired[i].RetiredAt, completed))
        {
            _device.Destroy(_retired[i].Buffer);
        }
        else
        {
            _retired[kept++] = _retired[i];
        }
    }
    _retired.Resize(kept);
    for (Chunk &chunk : _used)
    {
        chunk.RetiredAt = submitted;
        _retired.Add(chunk);
    }
    _used.Clear();
}
} // namespace Marlin
