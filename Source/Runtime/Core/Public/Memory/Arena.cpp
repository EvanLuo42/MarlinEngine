#include "Memory/Arena.h"

#include "HAL/PlatformMemory.h"

namespace Marlin
{
struct Arena::Block
{
    Block *Next;
    usize Size;
};

namespace
{
constexpr usize ArenaHeaderSize = AlignUp(sizeof(void *) * 2, DefaultAlignment);
constexpr usize FrameCommitGranularity = usize{1024} * 1024;

uint8 *BlockData(void *block)
{
    return static_cast<uint8 *>(block) + ArenaHeaderSize;
}
} // namespace

Arena::Arena(usize blockSize) : _blockSize(blockSize)
{
}

Arena::~Arena()
{
    Block *block = _first;
    while (block != nullptr)
    {
        Block *next = block->Next;
        Memory::Free(block);
        block = next;
    }
}

Arena::Block *Arena::NewBlock(usize minimumSize)
{
    const usize size = minimumSize > _blockSize ? minimumSize : _blockSize;
    auto *block = static_cast<Block *>(Memory::Allocate(ArenaHeaderSize + size));
    block->Next = nullptr;
    block->Size = size;
    Sanitizer::PoisonMemory(BlockData(block), size);
    return block;
}

void Arena::PoisonUnused()
{
    if constexpr (MARLIN_ASAN)
    {
        for (Block *block = _current; block != nullptr; block = block->Next)
        {
            const usize offset = block == _current ? _offset : 0;
            Sanitizer::PoisonMemory(BlockData(block) + offset, block->Size - offset);
        }
    }
}

void *Arena::Allocate(usize size, usize alignment)
{
    if (_current == nullptr)
    {
        _first = NewBlock(size + alignment);
        _current = _first;
        _offset = 0;
    }
    while (true)
    {
        const uint8 *data = BlockData(_current);
        const auto start = AlignUp(reinterpret_cast<uintptr>(data + _offset), static_cast<uintptr>(alignment));
        const usize end = (start - reinterpret_cast<uintptr>(data)) + size;
        if (end <= _current->Size)
        {
            _offset = end;
            Sanitizer::UnpoisonMemory(reinterpret_cast<void *>(start), size);
            return reinterpret_cast<void *>(start);
        }
        if (_current->Next == nullptr || _current->Next->Size < size + alignment)
        {
            Block *block = NewBlock(size + alignment);
            block->Next = _current->Next;
            _current->Next = block;
        }
        _current = _current->Next;
        _offset = 0;
    }
}

void Arena::Free(void * /*pointer*/, usize /*size*/)
{
}

Arena::Marker Arena::GetMarker() const
{
    return {.Block = _current, .Offset = _offset};
}

void Arena::Rewind(Marker marker)
{
    if (marker.Block == nullptr)
    {
        Reset();
        return;
    }
    _current = static_cast<Block *>(marker.Block);
    _offset = marker.Offset;
    PoisonUnused();
}

void Arena::Reset()
{
    _current = _first;
    _offset = 0;
    PoisonUnused();
}

usize Arena::GetUsedBytes() const
{
    usize used = 0;
    for (const Block *block = _first; block != nullptr && block != _current; block = block->Next)
    {
        used += block->Size;
    }
    return _current != nullptr ? used + _offset : 0;
}

FrameArena::FrameArena(usize capacity) : _capacity(AlignUp(capacity, FrameCommitGranularity))
{
    for (Buffer &buffer : _buffers)
    {
        buffer.Base = static_cast<uint8 *>(PlatformMemory::Reserve(_capacity));
        if (buffer.Base == nullptr)
        {
            MARLIN_FATAL("FrameArena could not reserve its address space");
        }
    }
}

FrameArena::~FrameArena()
{
    for (const Buffer &buffer : _buffers)
    {
        Sanitizer::UnpoisonMemory(buffer.Base, buffer.Committed.Load(MemoryOrder::Relaxed));
        PlatformMemory::Release(buffer.Base, _capacity);
    }
}

void *FrameArena::Allocate(usize size, usize alignment)
{
    if constexpr (MARLIN_ASAN)
    {
        alignment = alignment < Sanitizer::Granularity ? Sanitizer::Granularity : alignment;
    }
    Buffer &buffer = _buffers[_current.Load(MemoryOrder::Acquire)];
    usize offset = buffer.Offset.Load(MemoryOrder::Relaxed);
    usize start = 0;
    usize end = 0;
    do
    {
        start = AlignUp(offset, alignment);
        end = start + size;
        if (end > _capacity)
        {
            MARLIN_FATAL("FrameArena is out of space");
        }
    } while (!buffer.Offset.CompareExchangeWeak(offset, end, MemoryOrder::Relaxed));

    if (end > buffer.Committed.Load(MemoryOrder::Acquire))
    {
        const ScopeLock<SpinLock> lock(_commitLock);
        const usize committed = buffer.Committed.Load(MemoryOrder::Relaxed);
        if (end > committed)
        {
            const usize target = AlignUp(end, FrameCommitGranularity);
            if (!PlatformMemory::Commit(buffer.Base + committed, target - committed))
            {
                MARLIN_FATAL("FrameArena could not commit memory");
            }
            Sanitizer::PoisonMemory(buffer.Base + committed, target - committed);
            buffer.Committed.Store(target, MemoryOrder::Release);
        }
    }
    Sanitizer::UnpoisonMemory(buffer.Base + start, size);
    return buffer.Base + start;
}

void FrameArena::BeginFrame()
{
    const uint32 next = _current.Load(MemoryOrder::Relaxed) ^ 1;
    Sanitizer::PoisonMemory(_buffers[next].Base, _buffers[next].Committed.Load(MemoryOrder::Relaxed));
    _buffers[next].Offset.Store(0, MemoryOrder::Relaxed);
    _current.Store(next, MemoryOrder::Release);
}

usize FrameArena::GetUsedBytes() const
{
    return _buffers[_current.Load(MemoryOrder::Acquire)].Offset.Load(MemoryOrder::Relaxed);
}
} // namespace Marlin
