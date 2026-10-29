#include "IconPipeline/IconConverter.h"

#include "IconPipeline/SvgIcon.h"
#include "MarlinText/MarlinTextDocument.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

MARLIN_DEFINE_LOG_CATEGORY(LogIconPipeline);

namespace Marlin
{
namespace
{
StringView AsText(const List<uint8> &bytes)
{
    return {reinterpret_cast<const char *>(bytes.Data()), bytes.Count()};
}
} // namespace

bool IconConverter::Convert(ConversionContext &context)
{
    IconSource source;
    if (!context.ReadSource(source))
    {
        return false;
    }
    if (!source.SourceFile.IsValid())
    {
        context.Error("SourceFile names no SVG file");
        return false;
    }
    List<uint8> bytes;
    if (!context.ReadBinary(source.SourceFile.Uuid, bytes))
    {
        return false;
    }
    Icon icon;
    SvgIconDiagnostics diagnostics;
    if (!ParseSvgIcon(AsText(bytes), icon, diagnostics))
    {
        context.Error(diagnostics.Error.AsView());
        return false;
    }
    const StringView name = context.GetSource().GetName();
    for (const String &warning : diagnostics.Warnings)
    {
        MARLIN_LOG(LogIconPipeline, Warning, "{0}: {1}", name, warning);
    }
    icon.Name = FrozenString(name);
    context.SetResult(icon);
    return true;
}

bool SvgImporter::CanImport(const ImportContext &context) const
{
    return context.GetExtension() == "svg";
}

bool SvgImporter::Import(ImportContext &context) const
{
    List<uint8> bytes;
    if (!Vfs::ReadAll(context.GetPath(), bytes))
    {
        context.Error("the SVG file cannot be read");
        return false;
    }
    if (!AsText(bytes).Contains("<svg"))
    {
        context.Error("not an SVG document");
        return false;
    }
    IconSource source;
    source.SourceFile = context.GetSourceLink();
    context.AddObject({}, context.GetBaseName(), source);
    return true;
}
} // namespace Marlin
