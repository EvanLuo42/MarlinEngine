// The C interface of Include/MarlinBc7EncRdo.h, compiled into the library with bc7enc_rdo's sources.
#include "MarlinBc7EncRdo.h"

#include "rdo_bc_encoder.h"

#include <mutex>

namespace
{
// rdo_bc_encoder::init rebuilds the library's global tables on every call; one at a time keeps them from being
// written concurrently. Encoding itself only reads them.
std::mutex g_initLock;

int Level(float quality, int levels)
{
    const float clamped = quality < 0.0f ? 0.0f : quality > 1.0f ? 1.0f : quality;
    return static_cast<int>(clamped * static_cast<float>(levels - 1) + 0.5f);
}
} // namespace

extern "C" int MarlinBc7EncRdo_Encode(const MarlinBc7EncRdoInput *input, uint8_t *blocks, uint64_t blockBytes)
{
    rdo_bc::rdo_bc_params params;
    switch (input->Format)
    {
    case MarlinBc7EncRdo_BC1:
        params.m_dxgi_format = DXGI_FORMAT_BC1_UNORM;
        break;
    case MarlinBc7EncRdo_BC3:
        params.m_dxgi_format = DXGI_FORMAT_BC3_UNORM;
        break;
    case MarlinBc7EncRdo_BC4:
        params.m_dxgi_format = DXGI_FORMAT_BC4_UNORM;
        break;
    case MarlinBc7EncRdo_BC5:
        params.m_dxgi_format = DXGI_FORMAT_BC5_UNORM;
        break;
    case MarlinBc7EncRdo_BC7:
        params.m_dxgi_format = DXGI_FORMAT_BC7_UNORM;
        break;
    default:
        return 0;
    }
    params.m_bc7_uber_level = Level(input->Quality, 7);
    params.m_bc1_quality_level = Level(input->Quality, rgbcx::MAX_LEVEL + 1);
    params.m_rdo_lambda = input->RdoLambda > 0.0f ? input->RdoLambda : 0.0f;
    params.m_perceptual = input->Perceptual != 0;
    // BC1 with alpha would punch holes in opaque textures; transparency belongs in BC3 or BC7.
    params.m_use_bc1_3color_mode_for_black = false;
    params.m_rdo_multithreading = false;
    params.m_status_output = false;

    utils::image_u8 image(input->Width, input->Height);
    memcpy(image.get_pixels().data(), input->Texels, static_cast<size_t>(input->Width) * input->Height * 4);

    rdo_bc::rdo_bc_encoder encoder;
    {
        std::lock_guard<std::mutex> lock(g_initLock);
        if (!encoder.init(image, params))
        {
            return 0;
        }
    }
    if (!encoder.encode() || encoder.get_total_blocks_size_in_bytes() != blockBytes)
    {
        return 0;
    }
    memcpy(blocks, encoder.get_blocks(), blockBytes);
    return 1;
}

extern "C" int MarlinBc7EncRdo_Decode(uint32_t format, const uint8_t *blocks, uint32_t width, uint32_t height,
                                      uint8_t *texels)
{
    uint32_t blockBytes;
    switch (format)
    {
    case MarlinBc7EncRdo_BC1:
    case MarlinBc7EncRdo_BC4:
        blockBytes = 8;
        break;
    case MarlinBc7EncRdo_BC3:
    case MarlinBc7EncRdo_BC5:
    case MarlinBc7EncRdo_BC7:
        blockBytes = 16;
        break;
    default:
        return 0;
    }
    const uint32_t blocksX = (width + 3) / 4;
    const uint32_t blocksY = (height + 3) / 4;
    for (uint32_t blockY = 0; blockY < blocksY; ++blockY)
    {
        for (uint32_t blockX = 0; blockX < blocksX; ++blockX)
        {
            const uint8_t *block = blocks + (static_cast<size_t>(blockY) * blocksX + blockX) * blockBytes;
            uint8_t pixels[16 * 4];
            switch (format)
            {
            // unpack_bc1/bc3 return whether the block used the 3-color mode, not whether it decoded.
            case MarlinBc7EncRdo_BC1:
                rgbcx::unpack_bc1(block, pixels, true);
                break;
            case MarlinBc7EncRdo_BC3:
                rgbcx::unpack_bc3(block, pixels);
                break;
            case MarlinBc7EncRdo_BC4:
                for (uint32_t i = 0; i < 16; ++i)
                {
                    pixels[i * 4 + 0] = 0;
                    pixels[i * 4 + 1] = 0;
                    pixels[i * 4 + 2] = 0;
                    pixels[i * 4 + 3] = 255;
                }
                rgbcx::unpack_bc4(block, pixels, 4);
                break;
            case MarlinBc7EncRdo_BC5:
                for (uint32_t i = 0; i < 16; ++i)
                {
                    pixels[i * 4 + 2] = 0;
                    pixels[i * 4 + 3] = 255;
                }
                rgbcx::unpack_bc5(block, pixels, 0, 1, 4);
                break;
            default:
                if (!bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba *>(pixels)))
                {
                    return 0;
                }
                break;
            }
            for (uint32_t y = 0; y < 4 && blockY * 4 + y < height; ++y)
            {
                for (uint32_t x = 0; x < 4 && blockX * 4 + x < width; ++x)
                {
                    memcpy(texels + ((static_cast<size_t>(blockY) * 4 + y) * width + blockX * 4 + x) * 4,
                           pixels + (y * 4 + x) * 4, 4);
                }
            }
        }
    }
    return 1;
}
