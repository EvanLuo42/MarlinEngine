//! Bump allocators for short-lived data: an arena, a per-frame arena and a fixed-size pool.
//!
//! They trade individual frees for fast allocation. Destructors are not run automatically by any of them.

#pragma once

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "CoreTypes.h"
#include "Diagnostics/Sanitizer.h"
#include "Memory/Allocator.h"
#include "Memory/MemoryOps.h"
#include "Templates/Bit.h"

namespace Marlin
{
/// Allocates by bumping a pointer through blocks taken from the heap.
///
/// Everything is freed at once by `Reset` or `Rewind`, and destructors are never run. Use it for data that lives
/// until the end of a scope, frame or load.
///
/// # Thread safety
/// Not thread-safe. Use one arena per thread, or synchronize externally.
///
/// # Performance
/// Allocation is a pointer bump; a new block comes from the heap only when the current one is full.
///
/// # Examples
///
/// ```cpp
/// Arena arena;
/// int32 *values = arena.AllocateArray<int32>(4);
/// values[0] = 7;
/// arena.Reset();
/// ```
class CORE_API Arena final : public IAllocator
{
public:
    /// The default size of a new block in bytes.
    static constexpr usize DefaultBlockSize = usize{64} * 1024;

    /// A position in the arena that `Rewind` returns to.
    struct Marker
    {
        /// The block the marker points into, or null for the beginning of the arena.
        void *Block = nullptr;
        /// The byte offset in the block.
        usize Offset = 0;
    };

    /// Creates an arena whose blocks hold at least `blockSize` bytes.
    explicit Arena(usize blockSize = DefaultBlockSize);
    /// Frees every block.
    ~Arena() override;
    MARLIN_NONCOPYABLE(Arena);

    /// Returns uninitialized storage for `size` bytes aligned to `alignment`.
    ///
    /// Never returns null; running out of memory is fatal.
    ///
    /// # Preconditions
    /// `alignment` is a power of two.
    [[nodiscard]] void *Allocate(usize size, usize alignment) override;
    /// Does nothing; arena memory is released by `Reset` or `Rewind`.
    void Free(void *pointer, usize size) override;

    /// Returns uninitialized storage for `size` bytes aligned to [`DefaultAlignment`].
    [[nodiscard]] void *Allocate(usize size)
    {
        return Allocate(size, DefaultAlignment);
    }

    /// Returns uninitialized storage for `count` elements of `T`.
    ///
    /// The storage is aligned to `alignof(T)` when that is greater than 8, otherwise to 8.
    template <typename T> [[nodiscard]] T *AllocateArray(usize count)
    {
        return static_cast<T *>(Allocate(count * sizeof(T), alignof(T) > 8 ? alignof(T) : 8));
    }

    /// Constructs a T whose destructor will never run.
    template <typename T, typename... TArgs> T *New(TArgs &&...args)
    {
        return ConstructAt(AllocateArray<T>(1), Forward<TArgs>(args)...);
    }

    /// Returns a marker for the current allocation point.
    [[nodiscard]] Marker GetMarker() const;
    /// Reclaims everything allocated since `marker` was taken.
    ///
    /// A default marker reclaims the whole arena. The memory is reused by later allocations; blocks stay reserved
    /// until the arena is destroyed.
    void Rewind(Marker marker);
    /// Reclaims everything and keeps the blocks for reuse.
    void Reset();
    /// Returns the bytes consumed from the arena's blocks, including alignment padding and whole earlier blocks.
    [[nodiscard]] usize GetUsedBytes() const;

private:
    struct Block;

    Block *NewBlock(usize minimumSize);
    void PoisonUnused();

    usize _blockSize;
    Block *_first = nullptr;
    Block *_current = nullptr;
    usize _offset = 0;
};

/// The container policy for an [`Arena`], which must outlive the container.
struct ArenaAllocator
{
    /// The arena that backs the storage; it must outlive the containers that copy it.
    Arena *Source = nullptr;

    /// The storage of one container type.
    template <typename T> class ForType
    {
    public:
        /// Arena storage has no inline capacity.
        static constexpr usize InlineCapacity = 0;

        /// Creates the policy with a null source; `Allocate` asserts if the source is null.
        ForType() = default;
        explicit ForType(const ArenaAllocator &policy) : _source(policy.Source)
        {
        }

        /// Returns uninitialized storage for `capacity` elements from the arena.
        ///
        /// # Preconditions
        /// The source is not null. It is checked by an assertion in builds with assertions.
        [[nodiscard]] T *Allocate(usize capacity)
        {
            MARLIN_ASSERT(_source != nullptr);
            return _source->AllocateArray<T>(capacity);
        }

        /// Does nothing; the arena releases the storage.
        void Free(T * /*data*/, usize /*capacity*/)
        {
        }

        /// Returns false, because all storage comes from the arena.
        [[nodiscard]] constexpr bool IsInline(const T * /*data*/) const
        {
            return false;
        }

    private:
        Arena *_source = nullptr;
    };
};

/// Two bump buffers in reserved address space that swap each frame.
///
/// Data written during frame N stays valid while frame N+1 is built, so a frame can read the previous frame's
/// temporaries. `BeginFrame` switches to the other buffer and clears it.
///
/// # Thread safety
/// `Allocate` is thread-safe and lock-free except when it commits more pages.
class CORE_API FrameArena
{
public:
    /// Reserves at least `capacity` bytes of address space for each of the two buffers.
    ///
    /// The reservation is rounded up to a 1 MiB boundary.
    explicit FrameArena(usize capacity);
    /// Releases both buffers.
    ~FrameArena();
    MARLIN_NONCOPYABLE(FrameArena);

    /// Returns uninitialized storage for `size` bytes aligned to `alignment`.
    ///
    /// Never returns null; running out of space is fatal.
    ///
    /// # Preconditions
    /// `alignment` is a power of two.
    [[nodiscard]] void *Allocate(usize size, usize alignment = DefaultAlignment);

    /// Returns uninitialized storage for `count` elements of `T`.
    ///
    /// The storage is aligned to `alignof(T)` when that is greater than 8, otherwise to 8.
    template <typename T> [[nodiscard]] T *AllocateArray(usize count)
    {
        return static_cast<T *>(Allocate(count * sizeof(T), alignof(T) > 8 ? alignof(T) : 8));
    }

    /// Switches to the other buffer and clears it.
    ///
    /// The buffer being switched to last held data from two frames ago, so nothing may still use it.
    void BeginFrame();
    /// Returns the bytes allocated in the current buffer.
    [[nodiscard]] usize GetUsedBytes() const;

private:
    struct Buffer
    {
        uint8 *Base = nullptr;
        Atomic<usize> Offset{0};
        Atomic<usize> Committed{0};
    };

    Buffer _buffers[2];
    Atomic<uint32> _current{0};
    usize _capacity;
    SpinLock _commitLock;
};

/// Fixed-size blocks for one type, from chunks taken from the heap.
///
/// Freed blocks are reused first. The destructor frees the chunks without running the destructors of live objects.
///
/// # Thread safety
/// Not thread-safe.
///
/// # Performance
/// Allocation and free are O(1); a new chunk is allocated only when the free list is empty.
template <typename T> class PoolAllocator
{
public:
    /// Creates a pool that allocates `blocksPerChunk` blocks per chunk.
    ///
    /// # Preconditions
    /// `blocksPerChunk` is greater than zero. It is not checked.
    explicit PoolAllocator(usize blocksPerChunk = 256) : _blocksPerChunk(blocksPerChunk)
    {
    }

    /// Frees every chunk; the destructors of live objects are not run.
    ~PoolAllocator()
    {
        Chunk *chunk = _chunks;
        while (chunk != nullptr)
        {
            Chunk *next = chunk->Next;
            Memory::Free(chunk);
            chunk = next;
        }
    }

    MARLIN_NONCOPYABLE(PoolAllocator);

    /// Returns uninitialized storage for one `T`, allocating a chunk when the free list is empty.
    [[nodiscard]] T *Allocate()
    {
        if (_free == nullptr)
        {
            AddChunk();
        }
        FreeBlock *block = _free;
        Sanitizer::UnpoisonMemory(block, sizeof(FreeBlock));
        _free = block->Next;
        ++_count;
        return reinterpret_cast<T *>(block);
    }

    /// Returns a block to the pool; null is ignored.
    ///
    /// # Preconditions
    /// `pointer` was returned by `Allocate` and has not been freed yet.
    void Free(T *pointer)
    {
        if (pointer != nullptr)
        {
            auto *block = reinterpret_cast<FreeBlock *>(pointer);
            block->Next = _free;
            Sanitizer::PoisonMemory(block, sizeof(FreeBlock));
            _free = block;
            --_count;
        }
    }

    /// Constructs a `T` in a block from the pool.
    template <typename... TArgs> T *New(TArgs &&...args)
    {
        return ConstructAt(Allocate(), Forward<TArgs>(args)...);
    }

    /// Destroys a `T` and returns its block to the pool; null is ignored.
    void Delete(T *pointer)
    {
        if (pointer != nullptr)
        {
            DestroyAt(pointer);
            Free(pointer);
        }
    }

    /// Returns the number of blocks handed out and not freed.
    [[nodiscard]] usize GetCount() const
    {
        return _count;
    }

private:
    union FreeBlock {
        FreeBlock *Next;
        alignas(T) uint8 Storage[sizeof(T)];
    };

    struct Chunk
    {
        Chunk *Next;
    };

    static constexpr usize BlocksOffset = AlignUp(sizeof(Chunk), alignof(FreeBlock));

    void AddChunk()
    {
        auto *chunk = static_cast<Chunk *>(Memory::Allocate(BlocksOffset + (_blocksPerChunk * sizeof(FreeBlock)),
                                                            Private::ElementAlignment<FreeBlock>()));
        chunk->Next = _chunks;
        _chunks = chunk;
        auto *blocks = reinterpret_cast<FreeBlock *>(reinterpret_cast<uint8 *>(chunk) + BlocksOffset);
        for (usize i = _blocksPerChunk; i-- > 0;)
        {
            blocks[i].Next = _free;
            _free = blocks + i;
        }
        Sanitizer::PoisonMemory(blocks, _blocksPerChunk * sizeof(FreeBlock));
    }

    usize _blocksPerChunk;
    Chunk *_chunks = nullptr;
    FreeBlock *_free = nullptr;
    usize _count = 0;
};
} // namespace Marlin
