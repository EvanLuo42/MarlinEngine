#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Reflection/ExtensionSet.h"
#include "Texture/Texture.h"
#include "TexturePipeline/TextureEncoder.gen.h"

namespace Marlin
{
class StringWriter;

/// What an encoder encodes: a whole mip, or a strip of whole rows of blocks of one, since the converter splits large
/// mips into strips and encodes them on several jobs at once.
struct TextureEncodeInput
{
    TextureFormat Format = TextureFormat::BC7;
    uint32 Width = 0;
    uint32 Height = 0;
    /// Width * Height texels, rows top to bottom: RGBA8, or RGBA32Float for formats TextureFormats::IsFloat names.
    Span<const uint8> Texels;
    bool Srgb = false;
    /// 0 for the fastest encoding, 1 for the best.
    float32 Quality = 0.5f;
    /// Rate-distortion lambda for encoders that support it; 0 is off. Higher values trade quality for blocks that
    /// compress better in the package.
    float32 RdoLambda = 0;
};

/// Encodes texels into one or more block-compressed formats (Documentation/Architecture/06 §3.4): an extension point,
/// so each encoder library is a module of its own and a fork adds or swaps one with `MCLASS(Priority = N)` or
/// `MCLASS(Replaces = "Other")`. The converter takes the first encoder of a format and records every encoder of it as
/// a DDC input, so upgrading one reconverts only the textures of its formats. Bump GetVersion whenever the blocks
/// change for the same texels. Encode is called from several threads at once.
MCLASS(Abstract, ExtensionPoint)
class TEXTUREPIPELINE_API TextureEncoder
{
    MGENERATED_BODY()
public:
    TextureEncoder() = default;
    virtual ~TextureEncoder() = default;
    MARLIN_NONCOPYABLE(TextureEncoder);

    [[nodiscard]] virtual Span<const TextureFormat> GetFormats() const = 0;
    [[nodiscard]] virtual uint32 GetVersion() const = 0;
    /// Writes the blocks of *input* into *blocks*, rows of blocks top to bottom, as TextureFormats::GetMipLayout lays
    /// them out; false with a reason in *error* if it cannot.
    virtual bool Encode(const TextureEncodeInput &input, Span<uint8> blocks, StringWriter &error) const = 0;
};

/// The texture encoders linked into the process, by format in priority order. Immutable once built.
class TEXTUREPIPELINE_API TextureEncoderSet
{
public:
    TextureEncoderSet() = default;
    MARLIN_NONCOPYABLE(TextureEncoderSet);

    void Build(const ExtensionContext &context);

    /// The encoders that write *format*, highest priority first; empty if none.
    [[nodiscard]] Span<TextureEncoder *const> Find(TextureFormat format) const;

private:
    ExtensionSet<TextureEncoder> _encoders;
    HashMap<uint32, List<TextureEncoder *>> _byFormat;
};
} // namespace Marlin
