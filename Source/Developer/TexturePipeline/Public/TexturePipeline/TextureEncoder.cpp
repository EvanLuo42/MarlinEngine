#include "TexturePipeline/TextureEncoder.h"

namespace Marlin
{
void TextureEncoderSet::Build(const ExtensionContext &context)
{
    _encoders.Build(context);
    _byFormat.Clear();
    for (TextureEncoder *encoder : _encoders)
    {
        for (const TextureFormat format : encoder->GetFormats())
        {
            _byFormat.FindOrAdd(static_cast<uint32>(format)).Add(encoder);
        }
    }
}

Span<TextureEncoder *const> TextureEncoderSet::Find(TextureFormat format) const
{
    const List<TextureEncoder *> *encoders = _byFormat.Find(static_cast<uint32>(format));
    return encoders != nullptr ? encoders->AsSpan() : Span<TextureEncoder *const>();
}
} // namespace Marlin
