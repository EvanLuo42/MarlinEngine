#include "TexturePipeline/PngDecoder.h"

#include "Inflate.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr uint8 Signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
constexpr uint64 MaxPixels = uint64{1} << 28; // RECOVERY-DECISION TM-D1: 16384 x 16384, the largest 2D texture.

uint32 Be32(const uint8 *bytes)
{
    return (uint32{bytes[0]} << 24) | (uint32{bytes[1]} << 16) | (uint32{bytes[2]} << 8) | uint32{bytes[3]};
}

/// The CRC-32 of ISO 3309 tables for slicing by 8: Table[0] is the byte-wise table, Table[k] advances k more bytes.
struct CrcTables
{
    uint32 Table[8][256];
};

constexpr CrcTables MakeCrcTables()
{
    CrcTables tables{};
    for (uint32 i = 0; i < 256; ++i)
    {
        uint32 crc = i;
        for (uint32 bit = 0; bit < 8; ++bit)
        {
            crc = (crc & 1u) != 0 ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
        }
        tables.Table[0][i] = crc;
    }
    for (uint32 i = 0; i < 256; ++i)
    {
        for (uint32 k = 1; k < 8; ++k)
        {
            const uint32 previous = tables.Table[k - 1][i];
            tables.Table[k][i] = (previous >> 8) ^ tables.Table[0][previous & 255u];
        }
    }
    return tables;
}

constexpr CrcTables Crc = MakeCrcTables();

/// The CRC-32 that every chunk carries over its type and data, eight bytes at a time.
uint32 Crc32(const uint8 *bytes, usize size)
{
    const auto &table = Crc.Table;
    uint32 crc = 0xFFFFFFFFu;
    for (; size >= 8; bytes += 8, size -= 8)
    {
        // Little-endian words, as every platform the engine targets stores them.
        uint32 low = 0;
        uint32 high = 0;
        MemCopy(&low, bytes, 4);
        MemCopy(&high, bytes + 4, 4);
        low ^= crc;
        crc = table[7][low & 255u] ^ table[6][(low >> 8) & 255u] ^ table[5][(low >> 16) & 255u] ^ table[4][low >> 24] ^
              table[3][high & 255u] ^ table[2][(high >> 8) & 255u] ^ table[1][(high >> 16) & 255u] ^
              table[0][high >> 24];
    }
    for (; size > 0; ++bytes, --size)
    {
        crc = table[0][(crc ^ *bytes) & 255u] ^ (crc >> 8);
    }
    return ~crc;
}

struct Header
{
    uint32 Width = 0;
    uint32 Height = 0;
    uint8 Depth = 0;
    uint8 ColorType = 0;
    uint8 Interlace = 0;
};

uint32 ChannelsOf(uint8 colorType)
{
    switch (colorType)
    {
    case 0: // Grey.
        return 1;
    case 2: // RGB.
        return 3;
    case 3: // Palette indices.
        return 1;
    case 4: // Grey and alpha.
        return 2;
    case 6: // RGBA.
        return 4;
    default:
        return 0;
    }
}

bool ValidDepth(uint8 colorType, uint8 depth)
{
    switch (colorType)
    {
    case 0:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case 3:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case 2:
    case 4:
    case 6:
        return depth == 8 || depth == 16;
    default:
        return false;
    }
}

uint8 Paeth(uint8 a, uint8 b, uint8 c)
{
    const int32 p = int32{a} + int32{b} - int32{c};
    const int32 pa = p > a ? p - a : a - p;
    const int32 pb = p > b ? p - b : b - p;
    const int32 pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/// Undoes, in place, the per-row filters of *height* rows of *rowBytes* bytes, each led by its filter type byte.
/// *pixelBytes* is the distance to the byte of the same channel in the pixel to the left, at least 1. In place works
/// because a row only reads itself to the left and the row above, both already unfiltered. One loop per filter, so
/// the common ones vectorize.
bool Unfilter(uint8 *rows, uint32 height, usize rowBytes, uint32 pixelBytes)
{
    const usize stride = rowBytes + 1;
    const usize lead = pixelBytes < rowBytes ? pixelBytes : rowBytes;
    for (uint32 y = 0; y < height; ++y)
    {
        uint8 *row = rows + usize{y} * stride + 1;
        const uint8 *prior = y > 0 ? row - stride : nullptr;
        switch (row[-1])
        {
        case 0:
            break;
        case 1:
            for (usize i = pixelBytes; i < rowBytes; ++i)
            {
                row[i] = static_cast<uint8>(row[i] + row[i - pixelBytes]);
            }
            break;
        case 2:
            if (prior != nullptr)
            {
                for (usize i = 0; i < rowBytes; ++i)
                {
                    row[i] = static_cast<uint8>(row[i] + prior[i]);
                }
            }
            break;
        case 3:
            if (prior == nullptr)
            {
                for (usize i = pixelBytes; i < rowBytes; ++i)
                {
                    row[i] = static_cast<uint8>(row[i] + row[i - pixelBytes] / 2);
                }
                break;
            }
            for (usize i = 0; i < lead; ++i)
            {
                row[i] = static_cast<uint8>(row[i] + prior[i] / 2);
            }
            for (usize i = pixelBytes; i < rowBytes; ++i)
            {
                row[i] = static_cast<uint8>(row[i] + (uint32{row[i - pixelBytes]} + prior[i]) / 2);
            }
            break;
        case 4:
            // Without a row above, Paeth picks the left byte; on the first pixel, the byte above.
            if (prior == nullptr)
            {
                for (usize i = pixelBytes; i < rowBytes; ++i)
                {
                    row[i] = static_cast<uint8>(row[i] + row[i - pixelBytes]);
                }
                break;
            }
            for (usize i = 0; i < lead; ++i)
            {
                row[i] = static_cast<uint8>(row[i] + prior[i]);
            }
            for (usize i = pixelBytes; i < rowBytes; ++i)
            {
                row[i] = static_cast<uint8>(row[i] + Paeth(row[i - pixelBytes], prior[i], prior[i - pixelBytes]));
            }
            break;
        default:
            return false;
        }
    }
    return true;
}

struct Palette
{
    uint8 Rgba[256][4];
    uint32 Count;
};

struct Transparency
{
    bool Present;
    uint16 Key[3];
};

/// Writes the unfiltered rows of one pass, *width* by *height* pixels placed every *dx* and *dy* from (x0, y0), into
/// *image* as four channels: 8 bits for depths up to 8, scaled to the full range, and 16 bits for depth 16. Row *y*
/// starts at *rows* + *y* × *stride*.
void Expand(const Header &header, const uint8 *rows, usize stride, uint32 width, uint32 height, uint32 x0, uint32 y0,
            uint32 dx, uint32 dy, const Palette &palette, const Transparency &transparency, DecodedImage &image)
{
    const uint32 channels = ChannelsOf(header.ColorType);
    const uint32 depth = header.Depth;
    const uint32 maximum = (1u << depth) - 1;
    const uint32 pixelBytes = image.GetPixelBytes();
    // The common cases of a whole image of 8-bit RGBA or RGB: rows copy straight across.
    if (depth == 8 && dx == 1 && dy == 1 && x0 == 0 && y0 == 0 &&
        (header.ColorType == 6 || (header.ColorType == 2 && !transparency.Present)))
    {
        for (uint32 y = 0; y < height; ++y)
        {
            const uint8 *row = rows + usize{y} * stride;
            uint8 *out = image.Pixels.Data() + usize{y} * image.Width * 4;
            if (header.ColorType == 6)
            {
                MemCopy(out, row, usize{width} * 4);
                continue;
            }
            for (uint32 x = 0; x < width; ++x)
            {
                out[x * 4] = row[x * 3];
                out[x * 4 + 1] = row[x * 3 + 1];
                out[x * 4 + 2] = row[x * 3 + 2];
                out[x * 4 + 3] = 255;
            }
        }
        return;
    }
    const auto sample = [depth, maximum](const uint8 *row, uint32 index) -> uint32 {
        if (depth == 16)
        {
            return (uint32{row[usize{index} * 2]} << 8) | uint32{row[usize{index} * 2 + 1]};
        }
        if (depth == 8)
        {
            return row[index];
        }
        const usize bit = usize{index} * depth;
        return (uint32{row[bit / 8]} >> (8 - depth - bit % 8)) & maximum;
    };
    for (uint32 y = 0; y < height; ++y)
    {
        const uint8 *row = rows + usize{y} * stride;
        for (uint32 x = 0; x < width; ++x)
        {
            uint32 value[4] = {0, 0, 0, maximum};
            const uint32 first = x * channels;
            bool scale = depth < 8;
            switch (header.ColorType)
            {
            case 3: {
                const uint32 index = sample(row, x);
                const uint8 *entry = palette.Rgba[index < palette.Count ? index : 0];
                for (uint32 c = 0; c < 4; ++c)
                {
                    value[c] = entry[c];
                }
                scale = false;
                break;
            }
            case 0:
            case 4:
                value[0] = value[1] = value[2] = sample(row, first);
                if (header.ColorType == 4)
                {
                    value[3] = sample(row, first + 1);
                }
                else if (transparency.Present && value[0] == transparency.Key[0])
                {
                    value[3] = 0;
                }
                break;
            default:
                for (uint32 c = 0; c < channels; ++c)
                {
                    value[c] = sample(row, first + c);
                }
                if (header.ColorType == 2 && transparency.Present && value[0] == transparency.Key[0] &&
                    value[1] == transparency.Key[1] && value[2] == transparency.Key[2])
                {
                    value[3] = 0;
                }
                break;
            }
            const usize px = x0 + usize{x} * dx;
            const usize py = y0 + usize{y} * dy;
            uint8 *out = image.Pixels.Data() + (py * image.Width + px) * pixelBytes;
            for (uint32 c = 0; c < 4; ++c)
            {
                if (depth == 16)
                {
                    const auto wide = static_cast<uint16>(value[c]);
                    MemCopy(out + c * 2, &wide, sizeof(wide));
                }
                else
                {
                    out[c] = static_cast<uint8>(scale ? value[c] * 255 / maximum : value[c]);
                }
            }
        }
    }
}

struct Pass
{
    uint32 X0, Y0, DX, DY;
};

constexpr Pass Adam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4},
                           {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
constexpr Pass Whole[1] = {{0, 0, 1, 1}};
} // namespace

Span<const StringView> PngDecoder::GetExtensions() const
{
    static constexpr StringView Extensions[] = {"png"};
    return Extensions;
}

bool PngDecoder::CanDecode(Span<const uint8> header) const
{
    return header.Count() >= sizeof(Signature) && MemCompare(header.Data(), Signature, sizeof(Signature)) == 0;
}

bool PngDecoder::Decode(Span<const uint8> bytes, DecodedImage &image, StringWriter &error) const
{
    if (!CanDecode(bytes))
    {
        error.Append("not a PNG file");
        return false;
    }
    Header header;
    bool hasHeader = false;
    Palette palette{};
    Transparency transparency{};
    // The image data: the one IDAT chunk in place, or several joined.
    List<uint8> compressed;
    Span<const uint8> firstData;
    uint32 dataChunks = 0;
    usize offset = sizeof(Signature);
    bool ended = false;
    while (!ended)
    {
        if (bytes.Count() - offset < 8)
        {
            error.Append("the file ends inside a chunk");
            return false;
        }
        const uint32 length = Be32(bytes.Data() + offset);
        const uint8 *type = bytes.Data() + offset + 4;
        const StringView name(reinterpret_cast<const char *>(type), 4);
        if (bytes.Count() - offset - 8 < usize{length} + 4)
        {
            error.Append("a chunk runs past the end of the file");
            return false;
        }
        const uint8 *data = type + 4;
        if (Crc32(type, usize{length} + 4) != Be32(data + length))
        {
            FormatTo(error, "chunk {0} is corrupt", name);
            return false;
        }
        offset += usize{length} + 12;
        if (!hasHeader && name != "IHDR")
        {
            error.Append("the file has no IHDR");
            return false;
        }
        if (name == "IHDR")
        {
            if (length != 13)
            {
                error.Append("IHDR has the wrong size");
                return false;
            }
            header.Width = Be32(data);
            header.Height = Be32(data + 4);
            header.Depth = data[8];
            header.ColorType = data[9];
            header.Interlace = data[12];
            if (hasHeader || header.Width == 0 || header.Height == 0 ||
                uint64{header.Width} * header.Height > MaxPixels || !ValidDepth(header.ColorType, header.Depth) ||
                data[10] != 0 || data[11] != 0 || header.Interlace > 1)
            {
                error.Append("IHDR describes an unsupported image");
                return false;
            }
            hasHeader = true;
        }
        else if (name == "PLTE")
        {
            if (length == 0 || length % 3 != 0 || length / 3 > 256)
            {
                FormatTo(error, "chunk {0} is corrupt", name);
                return false;
            }
            palette.Count = length / 3;
            for (uint32 i = 0; i < palette.Count; ++i)
            {
                MemCopy(palette.Rgba[i], data + i * 3, 3);
                palette.Rgba[i][3] = 255;
            }
        }
        else if (name == "tRNS")
        {
            if (header.ColorType == 3)
            {
                for (uint32 i = 0; i < length && i < 256; ++i)
                {
                    palette.Rgba[i][3] = data[i];
                }
            }
            else if ((header.ColorType == 0 && length >= 2) || (header.ColorType == 2 && length >= 6))
            {
                for (uint32 c = 0; c < (header.ColorType == 0 ? 1u : 3u); ++c)
                {
                    transparency.Key[c] = static_cast<uint16>((uint32{data[c * 2]} << 8) | data[c * 2 + 1]);
                }
                transparency.Present = true;
            }
        }
        else if (name == "IDAT")
        {
            if (dataChunks == 1)
            {
                compressed.AddRange(firstData);
            }
            if (dataChunks == 0)
            {
                firstData = Span<const uint8>(data, length);
            }
            else
            {
                compressed.AddRange(Span<const uint8>(data, length));
            }
            ++dataChunks;
        }
        else if (name == "IEND")
        {
            ended = true;
        }
        else if ((type[0] & 0x20u) == 0)
        {
            FormatTo(error, "critical chunk {0} is not supported", name);
            return false;
        }
    }
    if (header.ColorType == 3 && palette.Count == 0)
    {
        error.Append("a palette image has no PLTE");
        return false;
    }

    const uint32 bitsPerPixel = ChannelsOf(header.ColorType) * header.Depth;
    const uint32 filterBytes = bitsPerPixel >= 8 ? bitsPerPixel / 8 : 1;
    const Span<const Pass> passes = header.Interlace == 1 ? Span<const Pass>(Adam7) : Span<const Pass>(Whole);
    const auto passWidth = [&header](const Pass &pass) {
        return header.Width > pass.X0 ? (header.Width - pass.X0 + pass.DX - 1) / pass.DX : 0u;
    };
    const auto passHeight = [&header](const Pass &pass) {
        return header.Height > pass.Y0 ? (header.Height - pass.Y0 + pass.DY - 1) / pass.DY : 0u;
    };
    const auto rowBytesOf = [bitsPerPixel](uint32 width) { return (usize{width} * bitsPerPixel + 7) / 8; };
    usize expected = 0;
    for (const Pass &pass : passes)
    {
        const uint32 width = passWidth(pass);
        const uint32 height = passHeight(pass);
        if (width != 0 && height != 0)
        {
            expected += usize{height} * (rowBytesOf(width) + 1);
        }
    }
    List<uint8> raw;
    if (!Inflate::Zlib(dataChunks > 1 ? compressed.AsSpan() : firstData, raw, expected))
    {
        error.Append("the image data is corrupt");
        return false;
    }

    image.Width = header.Width;
    image.Height = header.Height;
    image.Format = header.Depth == 16 ? DecodedPixelFormat::RGBA16 : DecodedPixelFormat::RGBA8;
    image.Pixels.Clear();
    // Every pixel is written, by the one pass or by the seven of Adam7.
    image.Pixels.ResizeUninitialized(usize{header.Width} * header.Height * image.GetPixelBytes());
    usize consumed = 0;
    for (const Pass &pass : passes)
    {
        const uint32 width = passWidth(pass);
        const uint32 height = passHeight(pass);
        if (width == 0 || height == 0)
        {
            continue;
        }
        const usize rowBytes = rowBytesOf(width);
        if (!Unfilter(raw.Data() + consumed, height, rowBytes, filterBytes))
        {
            error.Append("a row has an unknown filter");
            return false;
        }
        Expand(header, raw.Data() + consumed + 1, rowBytes + 1, width, height, pass.X0, pass.Y0, pass.DX, pass.DY,
               palette, transparency, image);
        consumed += usize{height} * (rowBytes + 1);
    }
    return true;
}
} // namespace Marlin
