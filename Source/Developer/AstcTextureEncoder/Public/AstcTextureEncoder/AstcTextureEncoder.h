#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "TexturePipeline/TextureEncoder.h"
#include "AstcTextureEncoder/AstcTextureEncoder.gen.h"

struct astcenc_context;

namespace Marlin
{
/// ASTC LDR in 4x4, 6x6 and 8x8 blocks through Arm's astcenc; Quality maps onto astcenc's 0-100 search effort and sRGB
/// color uses its sRGB profile. Contexts are costly to create, so each one is kept for the next mip with the same
/// settings.
MCLASS()
class ASTCTEXTUREENCODER_API AstcTextureEncoder final : public TextureEncoder
{
    MGENERATED_BODY()
public:
    AstcTextureEncoder() = default;
    ~AstcTextureEncoder() override;

    [[nodiscard]] Span<const TextureFormat> GetFormats() const override;
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const override;

    /// Decodes a *width* by *height* mip of ASTC *blocks* into RGBA8 *texels*, as a GPU would sample it; for tests and
    /// previews. False if the format is not ASTC or the sizes do not match.
    static bool Decode(TextureFormat format, Span<const uint8> blocks, uint32 width, uint32 height, bool srgb,
                       Span<uint8> texels);

private:
    /// An idle context and the block size, profile and effort it was created for.
    struct PooledContext
    {
        uint32 Key = 0;
        astcenc_context *Context = nullptr;
    };

    mutable SpinLock _lock;
    mutable List<PooledContext> _idle;
};
} // namespace Marlin
