#include "AssetPipeline/PackedColumn.h"

namespace Marlin
{
namespace
{
struct PageHeader
{
    uint64 Base;
    uint32 Offset;
    uint32 Bits;
};

static_assert(sizeof(PageHeader) == 16);

uint32 BitsFor(uint64 range)
{
    return range == 0 ? 0 : 64 - static_cast<uint32>(__builtin_clzll(range));
}
} // namespace

bool PackedColumnView::Open(const uint8 *data, usize size)
{
    if (size < 8)
    {
        return false;
    }
    uint32 pages = 0;
    MemCopy(&_count, data, 4);
    MemCopy(&pages, data + 4, 4);
    const usize expected = (static_cast<usize>(_count) + PackedColumnPageRows - 1) >> PackedColumnPageShift;
    if (pages != expected || size < 8 + static_cast<usize>(pages) * sizeof(PageHeader))
    {
        return false;
    }
    _pages = reinterpret_cast<const Page *>(data + 8);
    _data = data + 8 + static_cast<usize>(pages) * sizeof(PageHeader);
    _size = size;
    return true;
}

namespace PackedColumnWriter
{
void Write(Span<const uint64> values, List<uint8> &output)
{
    while ((output.Count() & 7) != 0)
    {
        output.Add(0);
    }
    const auto count = static_cast<uint32>(values.Count());
    const uint32 pages = (count + PackedColumnPageRows - 1) >> PackedColumnPageShift;
    List<PageHeader> headers;
    headers.Resize(pages);
    List<uint8> data;
    for (uint32 page = 0; page < pages; ++page)
    {
        const uint32 begin = page << PackedColumnPageShift;
        const uint32 end = begin + PackedColumnPageRows < count ? begin + PackedColumnPageRows : count;
        uint64 minimum = ~uint64{0};
        uint64 maximum = 0;
        for (uint32 i = begin; i < end; ++i)
        {
            minimum = values[i] < minimum ? values[i] : minimum;
            maximum = values[i] > maximum ? values[i] : maximum;
        }
        uint32 bits = BitsFor(maximum - minimum);
        while ((data.Count() & 7) != 0)
        {
            data.Add(0);
        }
        headers[page].Offset = static_cast<uint32>(data.Count());
        if (bits > 56)
        {
            bits = 64;
            minimum = 0;
            const usize offset = data.Count();
            data.Resize(offset + static_cast<usize>(end - begin) * 8);
            MemCopy(data.Data() + offset, values.Data() + begin, static_cast<usize>(end - begin) * 8);
        }
        else if (bits != 0)
        {
            const usize offset = data.Count();
            const usize bytes = (static_cast<usize>(end - begin) * bits + 7) >> 3;
            data.Resize(offset + bytes + 8);
            for (uint32 i = begin; i < end; ++i)
            {
                const uint64 bit = static_cast<uint64>(i - begin) * bits;
                uint8 *target = data.Data() + offset + (bit >> 3);
                uint64 word = 0;
                MemCopy(&word, target, 8);
                word |= (values[i] - minimum) << (bit & 7);
                MemCopy(target, &word, 8);
            }
            data.Resize(offset + bytes);
        }
        headers[page].Base = minimum;
        headers[page].Bits = bits;
    }
    data.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>("\0\0\0\0\0\0\0\0"), 8));
    const usize start = output.Count();
    output.Resize(start + 8 + headers.Count() * sizeof(PageHeader));
    MemCopy(output.Data() + start, &count, 4);
    MemCopy(output.Data() + start + 4, &pages, 4);
    MemCopy(output.Data() + start + 8, headers.Data(), headers.Count() * sizeof(PageHeader));
    output.AddRange(data.AsSpan());
}
} // namespace PackedColumnWriter
} // namespace Marlin
