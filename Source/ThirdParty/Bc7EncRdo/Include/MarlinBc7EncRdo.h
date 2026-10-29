#pragma once

/* A C interface over bc7enc_rdo's rdo_bc_encoder, so the engine never compiles the library's STL headers.
   Built into the library by Build/BuildWindows.sh from Build/MarlinBc7EncRdo.cpp. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MarlinBc7EncRdoFormat
{
    MarlinBc7EncRdo_BC1 = 1,
    MarlinBc7EncRdo_BC3 = 3,
    MarlinBc7EncRdo_BC4 = 4,
    MarlinBc7EncRdo_BC5 = 5,
    MarlinBc7EncRdo_BC7 = 7,
};

struct MarlinBc7EncRdoInput
{
    uint32_t Format;
    uint32_t Width;
    uint32_t Height;
    /* Width * Height texels, RGBA8, rows top to bottom. */
    const uint8_t *Texels;
    /* 0 is the fastest search, 1 the most thorough. */
    float Quality;
    /* Rate-distortion lambda: 0 leaves the blocks as encoded; higher trades quality for blocks that compress
       better afterwards. */
    float RdoLambda;
    /* Weights errors by how visible they are, for color; off for data such as normals. */
    int Perceptual;
};

/* Encodes the image into rows of 4x4 blocks, top to bottom, into blocks, which holds blockBytes bytes; 1 on success.
   Safe to call from several threads at once. */
int MarlinBc7EncRdo_Encode(const struct MarlinBc7EncRdoInput *input, uint8_t *blocks, uint64_t blockBytes);

/* Decodes width * height texels of the given MarlinBc7EncRdoFormat from rows of 4x4 blocks into texels, RGBA8; 1 on
   success. BC4 fills red and BC5 red and green, with the other channels 0 and alpha 255. */
int MarlinBc7EncRdo_Decode(uint32_t format, const uint8_t *blocks, uint32_t width, uint32_t height, uint8_t *texels);

#ifdef __cplusplus
}
#endif
