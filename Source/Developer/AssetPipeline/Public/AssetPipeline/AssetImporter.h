#pragma once

#include "AssetPipeline/SourceDecoder.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "MarlinText/BinaryFileLink.h"
#include "Misc/Guid.h"
#include "Reflection/ExtensionSet.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "AssetPipeline/AssetImporter.gen.h"

namespace Marlin
{
class AssetImporter;

/// One import of one source file: what the importer reads, and the source objects it produces.
class ASSETPIPELINE_API ImportContext
{
public:
    ImportContext(StringView path, const Guid &binary, const AssetImporter &importer, const ImageDecoderSet &images,
                  const SceneDecoderSet &scenes, const void *options = nullptr);
    MARLIN_NONCOPYABLE(ImportContext);

    /// The file's virtual path, such as /Project/Content/Rock.png.
    [[nodiscard]] StringView GetPath() const
    {
        return _path;
    }
    /// Lower case, without the dot.
    [[nodiscard]] StringView GetExtension() const
    {
        return _extension;
    }
    /// The file name without folder or extension, such as Rock.
    [[nodiscard]] StringView GetBaseName() const
    {
        return _baseName;
    }
    /// The link a source object writes to name the file, relative to the .masset beside it.
    [[nodiscard]] BinaryFileLink GetSourceLink() const;

    [[nodiscard]] const ImageDecoderSet &GetImageDecoders() const
    {
        return _images;
    }
    [[nodiscard]] const SceneDecoderSet &GetSceneDecoders() const
    {
        return _scenes;
    }
    /// The importer's options (AssetImporter::GetOptionsType), as this import uses them; null if it declares none.
    [[nodiscard]] const void *GetOptions() const
    {
        return _options;
    }
    template <typename T> [[nodiscard]] const T &GetOptions() const
    {
        return *static_cast<const T *>(_options);
    }

    /// Reads and decodes the file; DecodeOutline skips the geometry, for importers that list what a scene holds.
    bool Decode(DecodedScene &scene);
    bool DecodeOutline(DecodedScene &scene);

    /// Produces a source object. Its UUID comes from the importer's type, the file's GUID and *subPath*, which names
    /// the part of the file it stands for, such as a mesh of a scene; so importing again finds the same object.
    void AddObject(StringView subPath, StringView name, const TypeInfo &type, const void *object);
    template <typename T> void AddObject(StringView subPath, StringView name, const T &object)
    {
        AddObject(subPath, name, TypeOf<T>(), &object);
    }
    /// Produces a source object written out by the importer: *text* is the whole `Type { ... }` object, with the UUID
    /// GetUuid(subPath) gives, for types that are no reflected struct, such as PrefabEntity.
    void AddObjectText(StringView subPath, StringView text);
    /// The UUID AddObject gives *subPath*, so that objects can link to each other before they are added.
    [[nodiscard]] Guid GetUuid(StringView subPath) const;
    void Error(StringView message);

    struct Object
    {
        Guid Uuid;
        /// `Type { ... }` as MarlinText.
        String Text;
    };

    [[nodiscard]] Span<const Object> GetObjects() const
    {
        return _objects.AsSpan();
    }
    [[nodiscard]] bool HasFailed() const
    {
        return !_errors.IsEmpty();
    }
    [[nodiscard]] StringView GetErrors() const
    {
        return _errors;
    }

    /// The UUID AddObject gives *subPath*.
    [[nodiscard]] static Guid MakeUuid(const TypeInfo &importer, const Guid &binary, StringView subPath);

private:
    bool DecodeScene(DecodedScene &scene, bool outline);

    String _path;
    String _fileName;
    String _extension;
    String _baseName;
    Guid _binary;
    const AssetImporter &_importer;
    const ImageDecoderSet &_images;
    const SceneDecoderSet &_scenes;
    const void *_options;
    List<Object> _objects;
    String _errors;
};

/// Turns a file into source objects (Documentation/Architecture/13 §7.1): what the file holds, and the source
/// object, with default settings, that each part becomes. Heavy work belongs in converters, cached by the DDC; an
/// importer only looks. An extension point: the first importer, by priority, that accepts a file imports it.
MCLASS(Abstract, ExtensionPoint)
class ASSETPIPELINE_API AssetImporter
{
    MGENERATED_BODY()
public:
    AssetImporter() = default;
    virtual ~AssetImporter() = default;
    MARLIN_NONCOPYABLE(AssetImporter);

    [[nodiscard]] virtual bool CanImport(const ImportContext &context) const = 0;
    virtual bool Import(ImportContext &context) const = 0;

    /// The reflected type of this importer's options, which decide which objects it produces (Documentation/
    /// Architecture/06 §3.1), or null. Settings that change a conversion belong on the source objects instead.
    [[nodiscard]] virtual const TypeInfo *GetOptionsType() const
    {
        return nullptr;
    }
};

struct ImportResult
{
    bool Succeeded = false;
    /// The .masset written, such as /Project/Content/Rock.masset.
    String AssetPath;
    /// Objects added, and objects that were there already and kept as they are.
    uint32 Added = 0;
    uint32 Kept = 0;
    String Errors;
};

/// Imports source files into the .masset beside them, `Rock.png` into `Rock.masset`. Objects the file already has
/// are kept as people edited them, so importing again only adds what is new in the file: nothing is duplicated, no
/// setting is reset and no link breaks.
class ASSETPIPELINE_API AssetImport
{
public:
    AssetImport();
    MARLIN_NONCOPYABLE(AssetImport);

    /// *binary* is the file's GUID, as the asset index assigned it. *options*, a value of the importer's options type,
    /// replaces the options the .masset keeps; without it, the kept options are used, or the type's defaults the first
    /// time. The options used are kept in the .masset, as an object of their own.
    ImportResult Import(StringView path, const Guid &binary, const void *options = nullptr);

    /// The importer that takes *path*, or null.
    [[nodiscard]] const AssetImporter *FindImporter(StringView path, const Guid &binary) const;

private:
    ExtensionSet<AssetImporter> _importers;
    ImageDecoderSet _images;
    SceneDecoderSet _scenes;
};
} // namespace Marlin
