#include "AssetPipeline/SourceDecoder.h"

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

void ImageDecoderSet::Build(const ExtensionContext &context)
{
    _decoders.Build(context);
    _byExtension.Clear();
    for (ImageDecoder *decoder : _decoders)
    {
        for (const StringView extension : decoder->GetExtensions())
        {
            _byExtension.FindOrAdd(Lower(extension)).Add(decoder);
        }
    }
}

Span<ImageDecoder *const> ImageDecoderSet::Find(StringView extension) const
{
    const List<ImageDecoder *> *decoders = _byExtension.Find(Lower(extension));
    return decoders != nullptr ? decoders->AsSpan() : Span<ImageDecoder *const>();
}

ImageDecoder *ImageDecoderSet::Choose(StringView extension, Span<const uint8> bytes) const
{
    const Span<const uint8> header = bytes.Slice(0, bytes.Count() < 64 ? bytes.Count() : 64);
    for (ImageDecoder *decoder : Find(extension))
    {
        if (decoder->CanDecode(header))
        {
            return decoder;
        }
    }
    return nullptr;
}

bool ResolveReferencedPath(StringView file, StringView uri, String &path)
{
    if (uri.IsEmpty() || uri.Contains(':') || uri.First() == '/' || uri.First() == '\\')
    {
        return false;
    }
    List<StringView> folders;
    for (const StringView part : file.Split('/'))
    {
        if (!part.IsEmpty())
        {
            folders.Add(part);
        }
    }
    if (folders.Count() < 2)
    {
        return false;
    }
    folders.RemoveAt(folders.Count() - 1);
    String slashed(uri);
    for (usize i = 0; i < slashed.Length(); ++i)
    {
        if (slashed.Data()[i] == '\\')
        {
            slashed.Data()[i] = '/';
        }
    }
    StringView last;
    for (const StringView part : slashed.AsView().Split('/'))
    {
        last = part;
        if (part.IsEmpty() || part == ".")
        {
            continue;
        }
        if (part == "..")
        {
            // The mount, the first folder, stays.
            if (folders.Count() <= 1)
            {
                return false;
            }
            folders.RemoveAt(folders.Count() - 1);
            continue;
        }
        folders.Add(part);
    }
    path.Clear();
    for (const StringView folder : folders)
    {
        path.Append('/');
        path.Append(folder);
    }
    // A uri that ends in a folder names no file.
    return !last.IsEmpty() && last != "." && last != "..";
}

void SceneDecoderSet::Build(const ExtensionContext &context)
{
    _decoders.Build(context);
    _byExtension.Clear();
    for (SceneDecoder *decoder : _decoders)
    {
        for (const StringView extension : decoder->GetExtensions())
        {
            _byExtension.FindOrAdd(Lower(extension)).Add(decoder);
        }
    }
}

Span<SceneDecoder *const> SceneDecoderSet::Find(StringView extension) const
{
    const List<SceneDecoder *> *decoders = _byExtension.Find(Lower(extension));
    return decoders != nullptr ? decoders->AsSpan() : Span<SceneDecoder *const>();
}

SceneDecoder *SceneDecoderSet::Choose(StringView extension, Span<const uint8> bytes) const
{
    const Span<const uint8> header = bytes.Slice(0, bytes.Count() < 64 ? bytes.Count() : 64);
    for (SceneDecoder *decoder : Find(extension))
    {
        if (decoder->CanDecode(header))
        {
            return decoder;
        }
    }
    return nullptr;
}
} // namespace Marlin
