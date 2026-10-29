#include "TexturePipeline/TextureConverter.h"

#include "AssetPipeline/AssetCanonical.h"
#include "Concurrency/Jobs.h"
#include "Config/Config.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Math/Float16.h"
#include "Math/MathUtility.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Function.h"

namespace Marlin
{
namespace TextureColor
{
namespace
{
/// The 256 and 65536 entry tables, built on first use.
struct DecodeTables
{
    float32 From8[256];
    List<float32> From16;

    DecodeTables()
    {
        for (uint32 i = 0; i < 256; ++i)
        {
            From8[i] = SrgbToLinear(static_cast<float32>(i) / 255.0f);
        }
        From16.ResizeUninitialized(65536);
        for (uint32 i = 0; i < 65536; ++i)
        {
            From16[i] = SrgbToLinear(static_cast<float32>(i) / 65535.0f);
        }
    }
};

constexpr uint32 BucketBits = 12;
constexpr uint32 Buckets = 1u << BucketBits;

/// Encoding to 8 bits: the linear values where the rounded code steps up, and for each of Buckets equal slices of
/// [0, 1] the code at its lower end, so a lookup steps over at most a threshold or two.
struct EncodeTables
{
    float32 Thresholds[255];
    uint8 Start[Buckets];

    EncodeTables()
    {
        for (uint32 k = 0; k < 255; ++k)
        {
            Thresholds[k] = SrgbToLinear((static_cast<float32>(k) + 0.5f) / 255.0f);
        }
        uint32 code = 0;
        for (uint32 b = 0; b < Buckets; ++b)
        {
            const float32 low = static_cast<float32>(b) / static_cast<float32>(Buckets);
            while (code < 255 && Thresholds[code] <= low)
            {
                ++code;
            }
            Start[b] = static_cast<uint8>(code);
        }
    }
};

const DecodeTables &Decoding()
{
    static const DecodeTables tables;
    return tables;
}

const EncodeTables &Encoding()
{
    static const EncodeTables tables;
    return tables;
}
} // namespace

float32 SrgbToLinear(float32 value)
{
    return value <= 0.04045f ? value / 12.92f : Math::Pow((value + 0.055f) / 1.055f, 2.4f);
}

float32 LinearToSrgb(float32 value)
{
    return value <= 0.0031308f ? value * 12.92f : 1.055f * Math::Pow(value, 1.0f / 2.4f) - 0.055f;
}

float32 SrgbToLinear8(uint8 value)
{
    return Decoding().From8[value];
}

float32 SrgbToLinear16(uint16 value)
{
    return Decoding().From16[value];
}

uint8 LinearToSrgb8(float32 value)
{
    const EncodeTables &tables = Encoding();
    // Written so that NaN lands on 0, as Clamp would leave it.
    const float32 clamped = value > 0.0f ? (value < 1.0f ? value : 1.0f) : 0.0f;
    const uint32 bucket = Min(static_cast<uint32>(clamped * static_cast<float32>(Buckets)), Buckets - 1);
    uint32 code = tables.Start[bucket];
    while (code < 255 && clamped >= tables.Thresholds[code])
    {
        ++code;
    }
    return static_cast<uint8>(code);
}
} // namespace TextureColor

namespace
{
constexpr uint64 MipAlignment = 16;
/// Pixels each job of a per-pixel pass covers, in whole rows.
constexpr uint64 PixelsPerJob = 64 * 1024;

struct LinearImage
{
    uint32 Width = 0;
    uint32 Height = 0;
    List<float32> Pixels;
};

struct RowStrip
{
    const FunctionRef<void(uint32 first, uint32 end)> *Body = nullptr;
    uint32 First = 0;
    uint32 End = 0;
};

/// Runs *body* over rows [0, *height*) of an image *width* wide, split into strips across the job system; inline
/// when it is not running or the image is small.
void ForEachRows(uint32 width, uint32 height, FunctionRef<void(uint32 first, uint32 end)> body)
{
    const uint64 rowsPerJob = Max<uint64>(1, PixelsPerJob / Max<uint64>(width, 1));
    if (!JobSystem::IsRunning() || height <= rowsPerJob)
    {
        body(0, height);
        return;
    }
    List<RowStrip> strips;
    List<JobDecl> jobs;
    for (uint64 first = 0; first < height; first += rowsPerJob)
    {
        RowStrip &strip = strips.AddDefaulted();
        strip.Body = &body;
        strip.First = static_cast<uint32>(first);
        strip.End = static_cast<uint32>(Min<uint64>(first + rowsPerJob, height));
    }
    jobs.Reserve(strips.Count());
    for (RowStrip &strip : strips)
    {
        jobs.Add(JobDecl{.Entry =
                             [](void *argument) {
                                 const auto *strip = static_cast<const RowStrip *>(argument);
                                 (*strip->Body)(strip->First, strip->End);
                             },
                         .Argument = &strip});
    }
    JobCounter counter;
    Jobs::Run(jobs.AsSpan(), &counter);
    Jobs::Wait(counter);
}

LinearImage ToLinear(const DecodedImage &image, bool srgb)
{
    LinearImage linear;
    linear.Width = image.Width;
    linear.Height = image.Height;
    const usize rowValues = usize{image.Width} * 4;
    linear.Pixels.ResizeUninitialized(rowValues * image.Height);
    const uint8 *source = image.Pixels.Data();
    float32 *target = linear.Pixels.Data();
    // One loop per format, so that each is a table lookup or a multiply per channel.
    ForEachRows(image.Width, image.Height, [&](uint32 firstRow, uint32 endRow) {
        const usize first = firstRow * rowValues;
        const usize end = endRow * rowValues;
        switch (image.Format)
        {
        case DecodedPixelFormat::RGBA8:
            for (usize i = first; i < end; ++i)
            {
                target[i] = srgb && i % 4 != 3 ? TextureColor::SrgbToLinear8(source[i])
                                               : static_cast<float32>(source[i]) / 255.0f;
            }
            break;
        case DecodedPixelFormat::RGBA16:
            for (usize i = first; i < end; ++i)
            {
                uint16 raw = 0;
                MemCopy(&raw, source + i * 2, sizeof(raw));
                target[i] = srgb && i % 4 != 3 ? TextureColor::SrgbToLinear16(raw) : static_cast<float32>(raw) / 65535.0f;
            }
            break;
        case DecodedPixelFormat::RGBA32Float:
            MemCopy(target + first, source + first * 4, (end - first) * sizeof(float32));
            if (srgb)
            {
                for (usize i = first; i < end; ++i)
                {
                    target[i] = i % 4 != 3 ? TextureColor::SrgbToLinear(target[i]) : target[i];
                }
            }
            break;
        }
    });
    return linear;
}

LinearImage Halve(const LinearImage &image)
{
    LinearImage half;
    half.Width = image.Width > 1 ? image.Width / 2 : 1;
    half.Height = image.Height > 1 ? image.Height / 2 : 1;
    half.Pixels.ResizeUninitialized(usize{half.Width} * half.Height * 4);
    ForEachRows(half.Width, half.Height, [&](uint32 firstRow, uint32 endRow) {
        for (uint32 y = firstRow; y < endRow; ++y)
        {
            const uint32 y0 = y * 2 < image.Height ? y * 2 : image.Height - 1;
            const uint32 y1 = y0 + 1 < image.Height ? y0 + 1 : y0;
            const float32 *row0 = image.Pixels.Data() + usize{y0} * image.Width * 4;
            const float32 *row1 = image.Pixels.Data() + usize{y1} * image.Width * 4;
            float32 *out = half.Pixels.Data() + usize{y} * half.Width * 4;
            for (uint32 x = 0; x < half.Width; ++x)
            {
                const uint32 x0 = x * 2 < image.Width ? x * 2 : image.Width - 1;
                const uint32 x1 = x0 + 1 < image.Width ? x0 + 1 : x0;
                for (uint32 c = 0; c < 4; ++c)
                {
                    out[x * 4 + c] = (row0[x0 * 4 + c] + row0[x1 * 4 + c] + row1[x0 * 4 + c] + row1[x1 * 4 + c]) * 0.25f;
                }
            }
        }
    });
    return half;
}

/// Formats that hold color, which an sRGB source keeps in sRGB; the others hold data or HDR and stay linear.
bool IsColor(TextureFormat format)
{
    switch (format)
    {
    case TextureFormat::RGBA8:
    case TextureFormat::BC1:
    case TextureFormat::BC3:
    case TextureFormat::BC7:
    case TextureFormat::ASTC4x4:
    case TextureFormat::ASTC6x6:
    case TextureFormat::ASTC8x8:
        return true;
    default:
        return false;
    }
}

uint8 Quantize(float32 value)
{
    return static_cast<uint8>(Math::Round(Math::Clamp(value, 0.0f, 1.0f) * 255.0f));
}

/// The mip as 8-bit RGBA, sRGB-encoded if *srgb*, into *bytes*, which is reused from mip to mip.
void ToBytes(const LinearImage &image, bool srgb, List<uint8> &bytes)
{
    bytes.ResizeUninitialized(image.Pixels.Count());
    const float32 *source = image.Pixels.Data();
    uint8 *target = bytes.Data();
    const usize rowValues = usize{image.Width} * 4;
    ForEachRows(image.Width, image.Height, [&](uint32 firstRow, uint32 endRow) {
        for (usize i = firstRow * rowValues; i < endRow * rowValues; ++i)
        {
            target[i] = srgb && i % 4 != 3 ? TextureColor::LinearToSrgb8(source[i]) : Quantize(source[i]);
        }
    });
}

constexpr StringView EncoderInputPrefix = "TextureEncoder:";
/// Rows of blocks each job encodes. RECOVERY-DECISION TM-D3: 16 rows (64 texels) keeps a 4K mip at 64 jobs.
constexpr uint32 StripBlockRows = 16;

String EncoderInputName(TextureFormat format)
{
    const EnumeratorInfo *enumerator = TypeOf<TextureFormat>().FindEnumerator(static_cast<int64>(format));
    return Format("{0}{1}", EncoderInputPrefix, enumerator != nullptr ? enumerator->Name : StringView("Unknown"));
}

struct EncodeStrip
{
    const TextureEncoder *Encoder = nullptr;
    TextureEncodeInput Input;
    Span<uint8> Blocks;
    bool Succeeded = false;
    String Error;
};

void RunStrip(void *argument)
{
    auto *strip = static_cast<EncodeStrip *>(argument);
    StringBuilder<256> error;
    strip->Succeeded = strip->Encoder->Encode(strip->Input, strip->Blocks, error);
    strip->Error = error.ToString();
}

/// Encodes *input* into *blocks* in strips of StripBlockRows rows of blocks, one job each. Strips end on block rows,
/// so each strip's blocks are a contiguous part of the mip's and every encoder sees whole blocks but the last row.
bool Encode(const TextureEncoder &encoder, const TextureEncodeInput &input, Span<uint8> blocks, String &error)
{
    const uint32 blockSize = TextureFormats::GetBlockSize(input.Format);
    const usize texelBytes = TextureFormats::IsFloat(input.Format) ? 16 : 4;
    const uint32 blockRows = (input.Height + blockSize - 1) / blockSize;
    const usize rowBytes = blocks.Count() / blockRows;
    const uint32 stripCount = (blockRows + StripBlockRows - 1) / StripBlockRows;
    List<EncodeStrip> strips;
    strips.Resize(stripCount);
    List<JobDecl> jobs;
    jobs.Reserve(stripCount);
    for (uint32 index = 0; index < stripCount; ++index)
    {
        const uint32 firstBlockRow = index * StripBlockRows;
        const uint32 stripBlockRows = Min(StripBlockRows, blockRows - firstBlockRow);
        const uint32 firstRow = firstBlockRow * blockSize;
        const uint32 rows = Min(stripBlockRows * blockSize, input.Height - firstRow);
        EncodeStrip &strip = strips[index];
        strip.Encoder = &encoder;
        strip.Input = input;
        strip.Input.Height = rows;
        strip.Input.Texels =
            input.Texels.Slice(usize{firstRow} * input.Width * texelBytes, usize{rows} * input.Width * texelBytes);
        strip.Blocks = blocks.Slice(firstBlockRow * rowBytes, stripBlockRows * rowBytes);
        // Encoders such as bc7enc_rdo keep large arrays on the stack: more than a 64 KiB fiber holds.
        jobs.Add(JobDecl{.Entry = &RunStrip, .Argument = &strip, .LargeStack = true});
    }
    JobCounter counter;
    Jobs::Run(jobs.AsSpan(), &counter);
    Jobs::Wait(counter);
    for (const EncodeStrip &strip : strips)
    {
        if (!strip.Succeeded)
        {
            error = strip.Error;
            return false;
        }
    }
    return true;
}

bool EncodeMip(const LinearImage &image, const TextureCompression &compression, bool srgb,
               const TextureEncoder *encoder, List<uint8> &bulk, List<uint8> &bytes, TextureMip &mip, String &error)
{
    const TextureFormat format = compression.Format;
    mip.Width = image.Width;
    mip.Height = image.Height;
    TextureFormats::GetMipLayout(format, image.Width, image.Height, mip.RowPitch, mip.Size);
    mip.Offset = (bulk.Count() + MipAlignment - 1) & ~(MipAlignment - 1);
    bulk.Resize(static_cast<usize>(mip.Offset));
    bulk.ResizeUninitialized(static_cast<usize>(mip.Offset + mip.Size));
    uint8 *out = bulk.Data() + mip.Offset;
    const usize texels = usize{image.Width} * image.Height;

    if (encoder != nullptr)
    {
        TextureEncodeInput input;
        input.Format = format;
        input.Width = image.Width;
        input.Height = image.Height;
        input.Srgb = srgb;
        input.Quality = compression.Quality;
        input.RdoLambda = compression.RdoLambda;
        if (TextureFormats::IsFloat(format))
        {
            input.Texels = AsBytes(image.Pixels.AsSpan());
        }
        else
        {
            ToBytes(image, srgb, bytes);
            input.Texels = bytes.AsSpan();
        }
        return Encode(*encoder, input, Span<uint8>(out, static_cast<usize>(mip.Size)), error);
    }
    if (format == TextureFormat::RGBA16Float)
    {
        for (usize i = 0; i < texels * 4; ++i)
        {
            const Float16 half(Math::Clamp(image.Pixels[i], -Float16::Max, Float16::Max));
            MemCopy(out + i * 2, &half.Bits, sizeof(half.Bits));
        }
        return true;
    }
    ToBytes(image, srgb, bytes);
    const uint32 channels = format == TextureFormat::R8 ? 1 : format == TextureFormat::RG8 ? 2 : 4;
    if (channels == 4)
    {
        MemCopy(out, bytes.Data(), texels * 4);
        return true;
    }
    for (usize i = 0; i < texels; ++i)
    {
        for (uint32 c = 0; c < channels; ++c)
        {
            out[i * channels + c] = bytes[i * 4 + c];
        }
    }
    return true;
}

/// The bulk size of the whole chain from a *width* by *height* level, so it is allocated once.
uint64 ChainSize(TextureFormat format, uint32 width, uint32 height, bool mipMaps)
{
    uint64 total = 0;
    while (true)
    {
        uint32 rowPitch = 0;
        uint64 size = 0;
        TextureFormats::GetMipLayout(format, width, height, rowPitch, size);
        total = ((total + MipAlignment - 1) & ~(MipAlignment - 1)) + size;
        if (!mipMaps || (width == 1 && height == 1))
        {
            return total;
        }
        width = width > 1 ? width / 2 : 1;
        height = height > 1 ? height / 2 : 1;
    }
}
} // namespace

TextureConverter::TextureConverter()
{
    List<String> disabled;
    (void)Config::TryGet("Engine.Extensions.Disabled", disabled);
    ExtensionContext context;
    context.Disabled = disabled.AsSpan();
    _encoders.Build(context);
}

bool TextureConverter::Convert(ConversionContext &context)
{
    TextureSource source;
    TextureSettings settings;
    // Only its own kind's entry: adding or editing another kind converts no texture of this one again.
    if (!context.ReadSource(source) ||
        !context.ReadConfig("Engine.Textures", TypeOf<TextureSettings>(), &settings,
                            Format("Compression.{0}", source.Compression).AsView()))
    {
        return false;
    }
    if (!source.SourceFile.IsValid())
    {
        context.Error("SourceFile names no image");
        return false;
    }
    const TextureCompression *compression = settings.Compression.Find(source.Compression);
    if (compression == nullptr)
    {
        context.Error(Format("Engine.Textures.Compression has no {0}", source.Compression));
        return false;
    }
    const TextureEncoder *encoder = nullptr;
    if (TextureFormats::IsCompressed(compression->Format))
    {
        const String input = EncoderInputName(compression->Format);
        context.ReadConverterInput(input.AsView());
        const Span<TextureEncoder *const> encoders = _encoders.Find(compression->Format);
        if (encoders.IsEmpty())
        {
            context.Error(Format("no TextureEncoder writes {0}; link an encoder module",
                                 input.AsView().Substring(EncoderInputPrefix.Length())));
            return false;
        }
        encoder = encoders[0];
    }
    DecodedImage image;
    if (!context.Decode(source.SourceFile.Uuid, image))
    {
        return false;
    }
    const bool srgb = source.ColorSpace == TextureColorSpace::Srgb && IsColor(compression->Format);
    LinearImage level = ToLinear(image, srgb);
    image.Pixels.Clear();
    image.Pixels.ShrinkToFit();
    while (source.MaxSize != 0 && (level.Width > source.MaxSize || level.Height > source.MaxSize))
    {
        level = Halve(level);
    }

    Texture texture;
    texture.Name = FrozenString(context.GetSource().GetName());
    texture.Format = compression->Format;
    texture.ColorSpace = srgb ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
    texture.Width = level.Width;
    texture.Height = level.Height;
    List<TextureMip> mips;
    List<uint8> bulk;
    bulk.Reserve(static_cast<usize>(ChainSize(compression->Format, level.Width, level.Height, source.MipMaps)));
    List<uint8> bytes;
    while (true)
    {
        String error;
        if (!EncodeMip(level, *compression, srgb, encoder, bulk, bytes, mips.AddDefaulted(), error))
        {
            // RECOVERY-DECISION TM-D5: the message names the source object; the encoder's reason follows.
            context.Error(Format("{0} cannot be encoded: {1}", context.GetSource().GetName(), error));
            return false;
        }
        if (!source.MipMaps || (level.Width == 1 && level.Height == 1))
        {
            break;
        }
        level = Halve(level);
    }
    texture.Mips = FrozenArray<TextureMip>(mips.AsSpan());
    context.SetResult(texture);
    context.SetBulk(Move(bulk));
    return !context.HasFailed();
}

bool TextureConverter::WriteInput(StringView name, StringWriter &text) const
{
    if (!name.StartsWith(EncoderInputPrefix))
    {
        return false;
    }
    const EnumeratorInfo *format = TypeOf<TextureFormat>().FindEnumerator(name.Substring(EncoderInputPrefix.Length()));
    if (format == nullptr)
    {
        return false;
    }
    // Every encoder of the format, not only the chosen one: adding or removing a higher priority one changes the
    // result too.
    for (const TextureEncoder *encoder : _encoders.Find(static_cast<TextureFormat>(format->Value)))
    {
        AssetCanonical::WriteText(text, encoder->GetType().GetFullName());
        AssetCanonical::WriteNumber(text, encoder->GetVersion());
    }
    return true;
}
} // namespace Marlin
