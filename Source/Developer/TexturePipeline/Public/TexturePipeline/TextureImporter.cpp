#include "TexturePipeline/TextureImporter.h"

#include "String/Ascii.h"
#include "TexturePipeline/TextureConverter.h"

namespace Marlin
{
namespace
{
bool EndsWithIgnoringCase(StringView text, StringView suffix)
{
    if (text.Length() < suffix.Length())
    {
        return false;
    }
    const StringView tail = text.Right(suffix.Length());
    for (usize i = 0; i < suffix.Length(); ++i)
    {
        if (Ascii::ToLower(tail[i]) != Ascii::ToLower(suffix[i]))
        {
            return false;
        }
    }
    return true;
}
} // namespace

bool TextureImporter::CanImport(const ImportContext &context) const
{
    return !context.GetImageDecoders().Find(context.GetExtension()).IsEmpty();
}

bool TextureImporter::Import(ImportContext &context) const
{
    const StringView name = context.GetBaseName();
    TextureSource source;
    source.SourceFile = context.GetSourceLink();
    if (EndsWithIgnoringCase(name, "_N") || EndsWithIgnoringCase(name, "_Normal"))
    {
        source.Compression = "Normal";
    }
    else if (EndsWithIgnoringCase(name, "_M") || EndsWithIgnoringCase(name, "_Mask") ||
             EndsWithIgnoringCase(name, "_ORM"))
    {
        source.Compression = "Mask";
    }
    else if (EndsWithIgnoringCase(name, "_H") || EndsWithIgnoringCase(name, "_Height"))
    {
        source.Compression = "Grayscale";
    }
    else if (context.GetExtension() == "hdr" || context.GetExtension() == "exr")
    {
        source.Compression = "Hdr";
    }
    source.ColorSpace = source.Compression == "Color" ? TextureColorSpace::Srgb : TextureColorSpace::Linear;
    context.AddObject({}, name, source);
    return true;
}
} // namespace Marlin
