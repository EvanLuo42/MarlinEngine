#include "FontPipeline/FontConverter.h"

#include "MarlinText/MarlinTextDocument.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
uint32 ReadBigEndian32(Span<const uint8> data, usize offset)
{
    return (static_cast<uint32>(data[offset]) << 24) | (static_cast<uint32>(data[offset + 1]) << 16) |
           (static_cast<uint32>(data[offset + 2]) << 8) | static_cast<uint32>(data[offset + 3]);
}
} // namespace

uint32 GetSfntFaceCount(Span<const uint8> data)
{
    if (data.Count() < 12)
    {
        return 0;
    }
    const uint32 tag = ReadBigEndian32(data, 0);
    if (tag == 0x00010000u || tag == 0x4F54544Fu || tag == 0x74727565u)
    {
        return 1;
    }
    if (tag == 0x74746366u)
    {
        return ReadBigEndian32(data, 8);
    }
    return 0;
}

bool FontConverter::Convert(ConversionContext &context)
{
    FontSource source;
    if (!context.ReadSource(source))
    {
        return false;
    }
    if (!source.SourceFile.IsValid())
    {
        context.Error("SourceFile names no font");
        return false;
    }
    List<uint8> bytes;
    if (!context.ReadBinary(source.SourceFile.Uuid, bytes))
    {
        return false;
    }
    const uint32 faces = GetSfntFaceCount(bytes.AsSpan());
    if (faces == 0)
    {
        context.Error("SourceFile is not an OpenType or TrueType font");
        return false;
    }
    if (source.FaceIndex >= faces)
    {
        context.Error(Format("FaceIndex {0} is past the collection's {1} faces", source.FaceIndex, faces));
        return false;
    }
    Font font;
    font.Name = FrozenString(context.GetSource().GetName());
    font.Data = FrozenArray<uint8>(bytes.AsSpan());
    font.FaceIndex = source.FaceIndex;
    context.SetResult(font);
    return true;
}

bool FontImporter::CanImport(const ImportContext &context) const
{
    const StringView extension = context.GetExtension();
    return extension == "ttf" || extension == "otf" || extension == "ttc" || extension == "otc";
}

bool FontImporter::Import(ImportContext &context) const
{
    List<uint8> bytes;
    if (!Vfs::ReadAll(context.GetPath(), bytes))
    {
        context.Error("the font file cannot be read");
        return false;
    }
    const uint32 faces = GetSfntFaceCount(bytes.AsSpan());
    if (faces == 0)
    {
        context.Error("not an OpenType or TrueType font");
        return false;
    }
    for (uint32 face = 0; face < faces; ++face)
    {
        FontSource source;
        source.SourceFile = context.GetSourceLink();
        source.FaceIndex = face;
        if (faces == 1)
        {
            context.AddObject({}, context.GetBaseName(), source);
        }
        else
        {
            const String subPath = Format("{0}", face);
            context.AddObject(subPath.AsView(), Format("{0}_{1}", context.GetBaseName(), face).AsView(), source);
        }
    }
    return true;
}
} // namespace Marlin
