#include "AssetPipeline/AssetImporter.h"

#include "Config/Config.h"
#include "Containers/HashSet.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/UniquePtr.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
/// The sub-path of the object that keeps an importer's options; a colon starts no name a scene gives its parts.
constexpr StringView OptionsSubPath = ":Options";
} // namespace

ImportContext::ImportContext(StringView path, const Guid &binary, const AssetImporter &importer,
                             const ImageDecoderSet &images, const SceneDecoderSet &scenes, const void *options)
    : _path(path), _binary(binary), _importer(importer), _images(images), _scenes(scenes), _options(options)
{
    StringView directory;
    StringView fileName;
    if (!path.SplitLast('/', directory, fileName))
    {
        fileName = path;
    }
    _fileName = fileName;
    StringView baseName;
    StringView extension;
    if (!fileName.SplitLast('.', baseName, extension))
    {
        baseName = fileName;
        extension = {};
    }
    _baseName = baseName;
    _extension = extension;
    for (usize i = 0; i < _extension.Length(); ++i)
    {
        _extension.Data()[i] = Ascii::ToLower(_extension.Data()[i]);
    }
}

BinaryFileLink ImportContext::GetSourceLink() const
{
    BinaryFileLink link;
    link.Path = _fileName;
    link.Uuid = _binary;
    return link;
}

bool ImportContext::Decode(DecodedScene &scene)
{
    return DecodeScene(scene, false);
}

bool ImportContext::DecodeOutline(DecodedScene &scene)
{
    return DecodeScene(scene, true);
}

bool ImportContext::DecodeScene(DecodedScene &scene, bool outline)
{
    List<uint8> bytes;
    if (!Vfs::ReadAll(_path.AsView(), bytes))
    {
        Error(Format("{0} cannot be read", _path));
        return false;
    }
    SceneDecoder *decoder = _scenes.Choose(_extension.AsView(), bytes.AsSpan());
    // An import records no inputs, so the files the scene names are read straight from the VFS.
    class Files final : public ISourceFileReader
    {
    public:
        explicit Files(StringView path) : _path(path)
        {
        }

        bool ReadReferencedRange(StringView uri, uint64 offset, uint64 size, List<uint8> &bytes,
                                 StringWriter &error) override
        {
            String path;
            if (!ResolveReferencedPath(_path, uri, path))
            {
                FormatTo(error, "{0} is outside the file's mount", uri);
                return false;
            }
            const AsyncReadRequest read = Vfs::ReadAsync(path.AsView(), IoPriority::Critical, offset, size);
            if (!read.Wait() || !read.Succeeded() || (size != ToEnd && read.GetData().Count() != size))
            {
                FormatTo(error, "{0} cannot be read", path);
                return false;
            }
            bytes.Clear();
            bytes.AddRange(read.GetData());
            return true;
        }

    private:
        StringView _path;
    } files(_path.AsView());
    StringBuilder<256> error;
    if (decoder == nullptr || !(outline ? decoder->DecodeOutline(bytes.AsSpan(), files, scene, error)
                                        : decoder->Decode(bytes.AsSpan(), files, {}, scene, error)))
    {
        Error(decoder == nullptr ? Format("no scene decoder reads {0}", _path)
                                 : Format("{0} cannot be decoded: {1}", _path, error.AsView()));
        return false;
    }
    return true;
}

void ImportContext::AddObject(StringView subPath, StringView name, const TypeInfo &type, const void *object)
{
    Object &added = _objects.AddDefaulted();
    added.Uuid = MakeUuid(_importer.GetType(), _binary, subPath);
    StringBuilder<4096> text;
    MarlinTextSerializer::WriteObject(text, type, object, added.Uuid, name);
    added.Text = text.ToString();
}

void ImportContext::AddObjectText(StringView subPath, StringView text)
{
    Object &added = _objects.AddDefaulted();
    added.Uuid = GetUuid(subPath);
    added.Text = text;
}

Guid ImportContext::GetUuid(StringView subPath) const
{
    return MakeUuid(_importer.GetType(), _binary, subPath);
}

void ImportContext::Error(StringView message)
{
    _errors.Append(message);
    _errors.Append("\n");
}

Guid ImportContext::MakeUuid(const TypeInfo &importer, const Guid &binary, StringView subPath)
{
    // A name-based UUID in the file's own namespace: the same importer, file and part always give the same object,
    // and two files never share one. The importer's name is hashed so that renaming a part's path stays short.
    const StringView importerName = importer.GetFullName();
    const Hash128 importerHash = Xxh3Hash128(importerName.Data(), importerName.Length());
    StringBuilder<256> name;
    name.Append(Format("{0:x}{1:x}/", importerHash.High, importerHash.Low).AsView());
    name.Append(subPath);
    return Guid::FromName(binary, name.AsView());
}

AssetImport::AssetImport()
{
    List<String> disabled;
    (void)Config::TryGet("Engine.Extensions.Disabled", disabled);
    ExtensionContext context;
    context.Disabled = disabled.AsSpan();
    _importers.Build(context);
    _images.Build(context);
    _scenes.Build(context);
}

const AssetImporter *AssetImport::FindImporter(StringView path, const Guid &binary) const
{
    for (const AssetImporter *importer : _importers)
    {
        const ImportContext context(path, binary, *importer, _images, _scenes);
        if (importer->CanImport(context))
        {
            return importer;
        }
    }
    return nullptr;
}

ImportResult AssetImport::Import(StringView path, const Guid &binary, const void *options)
{
    ImportResult result;
    const AssetImporter *importer = FindImporter(path, binary);
    if (importer == nullptr)
    {
        result.Errors = Format("no importer takes {0}", path);
        return result;
    }
    StringView directory;
    StringView fileName;
    if (!path.SplitLast('/', directory, fileName))
    {
        directory = {};
        fileName = path;
    }
    StringView baseName;
    StringView extension;
    if (!fileName.SplitLast('.', baseName, extension))
    {
        baseName = fileName;
    }
    result.AssetPath = Format("{0}/{1}.masset", directory, baseName);

    // The .masset goes beside its source file in the VFS layer that file comes from, such as a platform layer,
    // not into whichever layer happens to be writable.
    const MountId layer = Vfs::FindLayer(path);
    if (layer == 0)
    {
        result.Errors = Format("{0} cannot be read", path);
        return result;
    }
    String text;
    if (!Vfs::ReadAll(layer, result.AssetPath.AsView(), text))
    {
        text = "!Version \"1.0\"\n";
    }
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    if (!document.Parse(text.AsView(), diagnostics))
    {
        result.Errors = Format("{0} cannot be read: {1}", result.AssetPath, diagnostics.ToString(result.AssetPath));
        return result;
    }

    // The options: those given, else those the .masset keeps, else the type's defaults.
    const TypeInfo *optionsType = importer->GetOptionsType();
    const Guid optionsUuid = ImportContext::MakeUuid(importer->GetType(), binary, OptionsSubPath);
    const usize optionsIndex =
        optionsType != nullptr ? document.FindObject(optionsUuid, diagnostics) : MarlinTextDocument::NotFound;
    UniquePtr<ReflectedValue> keptOptions;
    const bool optionsGiven = options != nullptr;
    if (optionsType != nullptr && !optionsGiven)
    {
        keptOptions = MakeUnique<ReflectedValue>(*optionsType);
        const MarlinTextObject *kept =
            optionsIndex != MarlinTextDocument::NotFound ? document.GetObject(optionsIndex, diagnostics) : nullptr;
        if (!keptOptions->IsValid() || (optionsIndex != MarlinTextDocument::NotFound &&
                                        (kept == nullptr || !MarlinTextSerializer::ReadObject(
                                                                *kept, *optionsType, keptOptions->Get(), diagnostics))))
        {
            result.Errors = Format("the import options in {0} cannot be read: {1}", result.AssetPath,
                                   diagnostics.ToString(result.AssetPath));
            return result;
        }
        options = keptOptions->Get();
    }

    ImportContext context(path, binary, *importer, _images, _scenes, optionsType != nullptr ? options : nullptr);
    const bool imported = importer->Import(context);
    if (!imported || context.HasFailed() || context.GetObjects().IsEmpty())
    {
        result.Errors = context.HasFailed() ? String(context.GetErrors()) : Format("{0} imported nothing", path);
        return result;
    }

    // Options given for this import replace the kept ones in place; the first import adds them.
    bool optionsEdited = false;
    if (optionsType != nullptr && optionsGiven && optionsIndex != MarlinTextDocument::NotFound)
    {
        const String before(document.GetText());
        if (!MarlinTextSerializer::UpdateObject(document, optionsIndex, *optionsType, options, diagnostics))
        {
            result.Errors =
                Format("{0} cannot be updated: {1}", result.AssetPath, diagnostics.ToString(result.AssetPath));
            return result;
        }
        optionsEdited = document.GetText() != before.AsView();
    }
    // Each object of the file is parsed once for its UUID, and the new ones are appended in one edit: adding them one
    // by one would parse the whole document again for each.
    HashSet<Guid> existing;
    for (usize i = 0; i < document.GetObjectCount(); ++i)
    {
        if (const MarlinTextObject *object = document.GetObject(i, diagnostics))
        {
            existing.Add(object->GetUuid());
        }
    }
    StringBuilder<4096> added;
    if (optionsType != nullptr && optionsIndex == MarlinTextDocument::NotFound)
    {
        const String name = Format("{0}_ImportOptions", baseName);
        StringBuilder<256> object;
        MarlinTextSerializer::WriteObject(object, *optionsType, options, optionsUuid, name.AsView());
        added.Append(object.AsView().Trim()).Append('\n');
    }
    for (const ImportContext::Object &object : context.GetObjects())
    {
        if (!existing.Add(object.Uuid))
        {
            ++result.Kept;
            continue;
        }
        added.Append(added.IsEmpty() ? "" : "\n").Append(object.Text.AsView().Trim()).Append('\n');
        ++result.Added;
    }
    if (!added.IsEmpty())
    {
        const StringView current = document.GetText();
        StringBuilder<4096> edited(current);
        if (!current.IsEmpty())
        {
            edited.Append(current.EndsWith('\n') ? "\n" : "\n\n");
        }
        edited.Append(added.AsView());
        if (!document.Parse(edited.AsView(), diagnostics))
        {
            result.Errors = Format("{0} cannot be read: {1}", result.AssetPath, diagnostics.ToString(result.AssetPath));
            return result;
        }
    }
    if ((optionsEdited || !added.IsEmpty()) && !Vfs::WriteAll(layer, result.AssetPath.AsView(), document.GetText()))
    {
        result.Errors = Format("{0} cannot be written", result.AssetPath);
        return result;
    }
    result.Succeeded = true;
    return result;
}
} // namespace Marlin
