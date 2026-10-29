#pragma once

#include "Containers/List.h"
#include "ScriptVM/ScriptDomain.h"

namespace Marlin
{
/// The bits of ScriptObject::Gc below the hash ordinal (MarlinScript §9.1).
namespace ScriptGcBits
{
inline constexpr uint64 Marked = 1 << 0;
/// The object moved: its Type field holds the new address.
inline constexpr uint64 Forwarded = 1 << 1;
inline constexpr uint64 AgeShift = 2;
inline constexpr uint64 AgeMask = 0xF << AgeShift;
/// Never freed or moved, such as a module's string literals and TypeInfo objects.
inline constexpr uint64 Permanent = 1 << 6;
/// In the old generation.
inline constexpr uint64 Old = 1 << 7;
} // namespace ScriptGcBits

/// The memory of a domain's heap (MarlinScript §9.2): one reservation holding the nursery, two survivor spaces and
/// the old generation. The old generation is made of blocks, each holding cells of one size class or part of a
/// large object; cards record old objects that may reference young ones.
class ScriptHeap
{
public:
    static constexpr usize BlockBytes = usize{64} << 10;
    static constexpr usize LargeObjectBytes = usize{8} << 10;
    static constexpr usize CardBytes = 512;

    explicit ScriptHeap(const ScriptDomainOptions &options);
    ~ScriptHeap();
    MARLIN_NONCOPYABLE(ScriptHeap);

    /// Zeroed memory in the nursery, or in the old generation with *old* set when the nursery is full or the object
    /// is large. Null when the heap is exhausted.
    uint8 *Allocate(usize bytes, bool &old);
    uint8 *AllocateOld(usize bytes);

    [[nodiscard]] bool Contains(const void *address) const
    {
        const uint8 *at = static_cast<const uint8 *>(address);
        return at >= _base && at < _base + _reserved;
    }

    [[nodiscard]] bool IsYoung(const void *address) const
    {
        const uint8 *at = static_cast<const uint8 *>(address);
        return at >= _edenBegin && at < _oldBase;
    }

    [[nodiscard]] bool IsOld(const void *address) const
    {
        const uint8 *at = static_cast<const uint8 *>(address);
        return at >= _oldBase && at < _oldEnd;
    }

    /// Records that the old object field at *address* may now reference a young object.
    void MarkCard(const void *address)
    {
        const usize card = static_cast<usize>(static_cast<const uint8 *>(address) - _oldBase) / CardBytes;
        if (card < _cards.Count())
        {
            _cards[card] = 1;
        }
    }

    /// Whether *bytes* more do not fit in the nursery.
    [[nodiscard]] bool IsNurseryFull(usize bytes) const
    {
        return _edenTop + bytes > _edenEnd;
    }

    [[nodiscard]] usize GetUsedBytes() const;

private:
    friend class ScriptCollector;

    struct BlockInfo
    {
        /// 0 free, 1 cells of SizeClass, 2 the first block of a large object of Run blocks, 3 a later one.
        uint8 Kind = 0;
        uint8 SizeClass = 0;
        uint32 Run = 0;
    };

    [[nodiscard]] static uint32 SizeClassOf(usize bytes);
    uint8 *AllocateBlocks(uint32 count);
    void FreeBlocks(usize first, uint32 count);
    void SetStart(const uint8 *object);

    /// Records that a young object starts at *object*, in the nursery or a survivor space.
    void SetYoungStart(const uint8 *object)
    {
        const usize bit = static_cast<usize>(object - _edenBegin) / 16;
        _youngStarts[bit / 64] |= uint64{1} << (bit % 64);
    }

    /// Forgets the young object starts from *begin* to *end*, once the objects there are evacuated.
    void ClearYoungStarts(const uint8 *begin, const uint8 *end);
    /// The young object holding *address*, which lies between *begin*, where the objects of its space start, and
    /// the end of the last of them.
    [[nodiscard]] uint8 *FindYoungStart(const uint8 *address, const uint8 *begin) const;

    uint8 *_base = nullptr;
    usize _reserved = 0;
    uint8 *_edenBegin = nullptr;
    uint8 *_edenTop = nullptr;
    uint8 *_edenEnd = nullptr;
    uint8 *_survivor[2] = {};
    /// The end of the objects in the survivor space in use.
    uint8 *_survivorTop = nullptr;
    usize _survivorBytes = 0;
    uint32 _survivorIndex = 0;
    uint8 *_oldBase = nullptr;
    uint8 *_oldEnd = nullptr;
    usize _blockHighWater = 0;
    /// No block below this one is free.
    usize _freeBlockHint = 0;
    List<BlockInfo> _blocks;
    List<uint8> _cards;
    /// A bit per 16 bytes of the old generation, set where a live cell or large object starts.
    List<uint64> _starts;
    /// A bit per 16 bytes of the young generation, set where an object starts in the nursery or the survivor space
    /// in use, so a managed pointer into a young object finds its start without walking the space.
    List<uint64> _youngStarts;
    /// The first free cell of each size class; a free cell holds the next one.
    List<uint8 *> _freeCells;
    usize _oldBytes = 0;
};
} // namespace Marlin
