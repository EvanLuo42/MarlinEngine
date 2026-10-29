#include "AstcTextureEncoder/AstcTextureEncoder.h"

#include "Concurrency/ScopeLock.h"
#include "Math/MathUtility.h"
#include "String/StringBuilder.h"
#include "astcenc.h"

namespace Marlin
{
namespace
{
constexpr astcenc_swizzle Rgba = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};

/// A single-threaded context for *block* x *block* texels at the astcenc effort *quality* (0-100); null if astcenc
/// refuses the settings or runs out of memory.
astcenc_context *CreateContext(uint32 block, bool srgb, float32 quality, uint32 flags)
{
    astcenc_config config;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR, block, block, 1, quality, flags, &config) !=
        ASTCENC_SUCCESS)
    {
        return nullptr;
    }
    astcenc_context *context = nullptr;
    return astcenc_context_alloc(&config, 1, &context, nullptr) == ASTCENC_SUCCESS ? context : nullptr;
}

/// Texels along each side of a block of *format*, or 0 if it is not an ASTC format.
uint32 AstcBlockSize(TextureFormat format)
{
    const bool astc =
        format == TextureFormat::ASTC4x4 || format == TextureFormat::ASTC6x6 || format == TextureFormat::ASTC8x8;
    return astc && TextureFormats::IsCompressed(format) && !TextureFormats::IsFloat(format)
               ? TextureFormats::GetBlockSize(format)
               : 0;
}
} // namespace

AstcTextureEncoder::~AstcTextureEncoder()
{
    for (const PooledContext &pooled : _idle)
    {
        astcenc_context_free(pooled.Context);
    }
}

Span<const TextureFormat> AstcTextureEncoder::GetFormats() const
{
    static constexpr TextureFormat Formats[] = {TextureFormat::ASTC4x4, TextureFormat::ASTC6x6, TextureFormat::ASTC8x8};
    return Formats;
}

bool AstcTextureEncoder::Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const
{
    const uint32 block = AstcBlockSize(input.Format);
    if (block == 0 || input.Texels.Count() != usize{input.Width} * input.Height * 4)
    {
        error.Append("astcenc takes RGBA8 texels of ASTC");
        return false;
    }
    // RECOVERY-DECISION ENC-D1: Quality 0-1 maps linearly onto astcenc's effort 0-100, rounded to whole steps so
    // contexts are shared between nearby settings; no extra flags (no normal-map or perceptual mode), since the input
    // does not say what kind of texture it is.
    const uint32 effort = static_cast<uint32>(Math::Clamp(input.Quality, 0.0f, 1.0f) * 100.0f + 0.5f);
    const uint32 key = block | (input.Srgb ? 1u << 4 : 0u) | effort << 5;

    astcenc_context *context = nullptr;
    {
        ScopeLock<SpinLock> lock(_lock);
        for (usize i = 0; i < _idle.Count(); ++i)
        {
            if (_idle[i].Key == key)
            {
                context = _idle[i].Context;
                _idle.RemoveAtSwap(i);
                break;
            }
        }
    }
    if (context == nullptr)
    {
        context = CreateContext(block, input.Srgb, static_cast<float32>(effort), 0);
        if (context == nullptr)
        {
            error.Append("astcenc cannot create a context");
            return false;
        }
    }

    void *slice = const_cast<uint8 *>(input.Texels.Data());
    astcenc_image image{input.Width, input.Height, 1, ASTCENC_TYPE_U8, &slice};
    const astcenc_error status = astcenc_compress_image(context, &image, &Rgba, blocks.Data(), blocks.Count(), 0);
    (void)astcenc_compress_reset(context);
    {
        ScopeLock<SpinLock> lock(_lock);
        _idle.Add(PooledContext{.Key = key, .Context = context});
    }
    if (status != ASTCENC_SUCCESS)
    {
        error.Append(astcenc_get_error_string(status));
        return false;
    }
    return true;
}

bool AstcTextureEncoder::Decode(TextureFormat format, Span<const uint8> blocks, uint32 width, uint32 height, bool srgb,
                                Span<uint8> texels)
{
    const uint32 block = AstcBlockSize(format);
    if (block == 0 || texels.Count() != usize{width} * height * 4)
    {
        return false;
    }
    astcenc_context *context = CreateContext(block, srgb, 0.0f, ASTCENC_FLG_DECOMPRESS_ONLY);
    if (context == nullptr)
    {
        return false;
    }
    void *slice = texels.Data();
    astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, &slice};
    const astcenc_error status = astcenc_decompress_image(context, blocks.Data(), blocks.Count(), &image, &Rgba, 0);
    astcenc_context_free(context);
    return status == ASTCENC_SUCCESS;
}
} // namespace Marlin
