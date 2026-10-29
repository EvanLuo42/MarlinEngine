#include "Compression/CompressionCodec.h"

#include "String/Ascii.h"

namespace Marlin
{
namespace
{
String Lower(StringView text)
{
    String lower(text);
    for (usize i = 0; i < lower.Length(); ++i)
    {
        lower.Data()[i] = Ascii::ToLower(lower.Data()[i]);
    }
    return lower;
}
} // namespace

void CompressionCodecSet::Build(const ExtensionContext &context)
{
    _codecs.Build(context);
    _byName.Clear();
    for (CompressionCodec *codec : _codecs)
    {
        const String key = Lower(codec->GetName());
        if (!_byName.Contains(key))
        {
            _byName.FindOrAdd(key) = codec;
        }
    }
}

const CompressionCodec *CompressionCodecSet::Find(StringView name) const
{
    CompressionCodec *const *codec = _byName.Find(Lower(name));
    return codec != nullptr ? *codec : nullptr;
}

} // namespace Marlin
