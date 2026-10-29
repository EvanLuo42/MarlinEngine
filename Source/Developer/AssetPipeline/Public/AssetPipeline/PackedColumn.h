#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Memory/MemoryOps.h"

namespace Marlin
{
/// A column of unsigned integers, bit-packed in pages of PackedColumnPageRows: each page stores its minimum and
/// the fewest bits that cover the rest, so sorted ids, small counts and ascending row numbers take a few bits per
/// row. Pages wider than 56 bits store plain 64-bit values. Random access costs one or two unaligned loads.
///
/// Layout: uint32 row count, uint32 page count, then per page { uint64 base, uint32 data offset, uint32 bits },
/// then the data, padded with 8 bytes so every load stays inside.
inline constexpr uint32 PackedColumnPageShift = 12;
inline constexpr uint32 PackedColumnPageRows = 1u << PackedColumnPageShift;

class ASSETPIPELINE_API PackedColumnView
{
public:
    PackedColumnView() = default;

    /// *data* points at a column written by PackedColumnWriter; false if it does not fit in *size* bytes.
    bool Open(const uint8 *data, usize size);

    [[nodiscard]] uint32 Count() const
    {
        return _count;
    }

    [[nodiscard]] uint64 operator[](uint32 row) const
    {
        const Page &page = _pages[row >> PackedColumnPageShift];
        const uint32 index = row & (PackedColumnPageRows - 1);
        const uint8 *data = _data + page.Offset;
        if (page.Bits == 64)
        {
            uint64 value = 0;
            MemCopy(&value, data + static_cast<usize>(index) * 8, 8);
            return value;
        }
        if (page.Bits == 0)
        {
            return page.Base;
        }
        const uint64 bit = static_cast<uint64>(index) * page.Bits;
        uint64 word = 0;
        MemCopy(&word, data + (bit >> 3), 8);
        return page.Base + ((word >> (bit & 7)) & ((uint64{1} << page.Bits) - 1));
    }

    /// The bytes the column occupies, for statistics.
    [[nodiscard]] usize GetSize() const
    {
        return _size;
    }

private:
    struct Page
    {
        uint64 Base;
        uint32 Offset;
        uint32 Bits;
    };

    const Page *_pages = nullptr;
    const uint8 *_data = nullptr;
    uint32 _count = 0;
    usize _size = 0;
};

namespace PackedColumnWriter
{
/// Appends *values* to *output* as a packed column, aligned to 8 bytes.
ASSETPIPELINE_API void Write(Span<const uint64> values, List<uint8> &output);
} // namespace PackedColumnWriter
} // namespace Marlin
