#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/ConversionStore.h"
#include "AssetPipeline/SourceDecoder.h"
#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Memory/SharedPtr.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "Reflection/ExtensionSet.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "AssetPipeline/AssetConverter.gen.h"

namespace Marlin
{
class Asset;
class AssetConversion;
class AssetConverter;
class ConfigView;
class MarlinTextDocument;
class MarlinTextObject;
class StringWriter;

/// The config a conversion for *target* reads (Documentation/Architecture/06 §3.2): the target platform's layers,
/// `@if Platform` the target's, `Target` Game or Server by its kind, and Configuration undefined, since converted data
/// is shared by every build configuration. Null if this build knows no such platform.
[[nodiscard]] ASSETPIPELINE_API const ConfigView *GetConversionConfig(const ConversionTarget &target);

/// Finds source objects and binary files for conversions: the asset index, through its daemon or in-process.
class ASSETPIPELINE_API IAssetSourceResolver
{
public:
    IAssetSourceResolver() = default;
    virtual ~IAssetSourceResolver() = default;
    MARLIN_NONCOPYABLE(IAssetSourceResolver);

    virtual bool FindObject(const Guid &uuid, AssetObjectInfo &info) = 0;
    virtual bool FindBinary(const Guid &uuid, AssetBinaryInfo &info) = 0;
    /// The binary file at a virtual path, such as one a source file names (ISourceFileReader).
    virtual bool FindBinaryAt(StringView path, AssetBinaryInfo &info) = 0;
    /// What the index stored at scan time, so a result key never needs the source file.
    virtual bool GetCanonicalHash(const Guid &uuid, Hash128 &hash) = 0;
    virtual bool GetBinaryHash(const Guid &uuid, Hash128 &hash) = 0;
};

/// Resolves through an AssetIndex in this process, such as the indexer's own.
class ASSETPIPELINE_API IndexSourceResolver final : public IAssetSourceResolver
{
public:
    explicit IndexSourceResolver(const AssetIndex &index) : _index(&index)
    {
    }

    bool FindObject(const Guid &uuid, AssetObjectInfo &info) override
    {
        return _index->Find(uuid, info);
    }

    bool FindBinary(const Guid &uuid, AssetBinaryInfo &info) override
    {
        return _index->FindBinary(uuid, info);
    }

    bool FindBinaryAt(StringView path, AssetBinaryInfo &info) override
    {
        return _index->FindBinaryAt(path, info);
    }

    bool GetCanonicalHash(const Guid &uuid, Hash128 &hash) override
    {
        return _index->GetCanonicalHash(uuid, hash);
    }

    bool GetBinaryHash(const Guid &uuid, Hash128 &hash) override
    {
        return _index->GetBinaryHash(uuid, hash);
    }

private:
    const AssetIndex *_index;
};

/// One conversion: the source object, and every other input the converter reads, recorded so that the result key
/// covers exactly what the result depends on.
class ASSETPIPELINE_API ConversionContext
{
public:
    ConversionContext(IAssetSourceResolver &resolver, const ConversionTarget &target);
    ~ConversionContext();
    MARLIN_NONCOPYABLE(ConversionContext);

    /// Loads the source object; false with an error if it cannot be found or parsed.
    bool Begin(const Guid &uuid);

    [[nodiscard]] const Guid &GetGuid() const
    {
        return _uuid;
    }

    [[nodiscard]] const MarlinTextObject &GetSource() const
    {
        return *_source;
    }

    [[nodiscard]] const AssetObjectInfo &GetSourceInfo() const
    {
        return _info;
    }

    [[nodiscard]] const ConversionTarget &GetTarget() const
    {
        return _target;
    }

    /// Reads the source object into a reflected value, the objects it overrides first.
    bool ReadSource(const TypeInfo &type, void *value);
    template <typename T> bool ReadSource(T &value)
    {
        return ReadSource(TypeOf<T>(), &value);
    }

    /// Another source object the result depends on, such as a material's shader.
    const MarlinTextObject *ReadDependency(const Guid &uuid);
    /// A binary source file the result depends on, such as a texture's PNG.
    bool ReadBinary(const Guid &uuid, List<uint8> &bytes);
    /// A binary file that the binary file at *file* names by *uri*, relative to its folder (ResolveReferencedPath),
    /// such as a glTF's separate buffer; recorded like ReadBinary. Scene decoders read these through
    /// ISourceFileReader. The whole file (*size* ISourceFileReader::ToEnd from 0) is checked against the index's
    /// hash as ReadBinary checks it; a part of it, which is too costly to hash on every read, against the size and
    /// modification time the index scanned.
    bool ReadReferencedBinary(StringView file, StringView uri, uint64 offset, uint64 size, List<uint8> &bytes);
    /// Decodes an image source file through the ImageDecoder its extension and header select (13 §7.1); the decoders
    /// are recorded as an input along with the file.
    bool Decode(const Guid &uuid, DecodedImage &image);
    /// Decodes a scene source file through the SceneDecoder its extension and header select, recorded the same way;
    /// given a *mesh* name, only that mesh needs its primitives (SceneDecoder::Decode).
    bool Decode(const Guid &uuid, DecodedScene &scene, StringView mesh = {});
    /// A config section the result depends on, such as Engine.Textures, merged over every layer. With *part*, a
    /// dotted path into it such as `Compression.Color`, the result depends on that part alone: the converter promises
    /// to use nothing else of the section, and edits elsewhere in it do not convert it again.
    bool ReadConfig(StringView section, const TypeInfo &type, void *value, StringView part = {});
    /// An input only the converter knows how to hash, such as the encoders it picks; the key check asks the converter's
    /// WriteInput for *name* whenever it checks the key.
    void ReadConverterInput(StringView name);

    /// The converter *converter* replaces, for a converter that wraps another (`MCLASS(Replaces = "Other")`).
    [[nodiscard]] AssetConverter *GetReplaced(const AssetConverter &converter) const;

    /// The run-time object; written as the block. A later call replaces an earlier one, so a wrapping converter can
    /// let the wrapped one convert and then write its own result.
    void SetResult(const Asset &result);
    /// Another result of the same conversion, such as one streaming cell of a level, under its own *uuid*: one no
    /// source object has, normally derived from the source's. It shares the main result's inputs; converting *uuid*
    /// converts the source object, or finds it cached, and returns this block.
    void AddDerivedResult(const Guid &uuid, const Asset &result);
    void SetBulk(List<uint8> bulk);
    void Error(StringView message);

    [[nodiscard]] bool HasFailed() const
    {
        return !_errors.IsEmpty();
    }

    [[nodiscard]] StringView GetErrors() const
    {
        return _errors;
    }

    /// Source files whose contents differ from what the index scanned, so that the key would not cover what was
    /// read; the conversion has failed, and should be tried again once the index has caught up.
    [[nodiscard]] Span<const String> GetStalePaths() const
    {
        return _stale.AsSpan();
    }

    [[nodiscard]] Span<const DependencyInput> GetInputs() const
    {
        return _inputs.AsSpan();
    }

    [[nodiscard]] Span<const uint8> GetBlock() const
    {
        return _block.AsSpan();
    }

    [[nodiscard]] Span<const uint8> GetBulk() const
    {
        return _bulk.AsSpan();
    }

    struct DerivedResult
    {
        Guid Uuid;
        List<uint8> Block;
    };

    [[nodiscard]] Span<const DerivedResult> GetDerivedResults() const
    {
        return _derived.AsSpan();
    }

    /// The canonical hash of *uuid*'s source object: what a dependency contributes to a key.
    bool HashObject(const Guid &uuid, Hash128 &hash);
    /// The hash of an input as it is now.
    bool HashInput(const DependencyInput &input, Hash128 &hash);

private:
    friend class AssetConversion;
    struct Document;

    /// Loads *uuid*'s source object, checking it against the index's canonical hash once.
    const MarlinTextObject *LoadObject(const Guid &uuid, AssetObjectInfo *info);
    void Record(DependencyInputKind kind, const Guid &uuid, StringView section = {});
    void Stale(StringView path);

    IAssetSourceResolver &_resolver;
    const AssetConversion *_conversion = nullptr;
    const AssetConverter *_converter = nullptr;
    ConversionTarget _target;
    Guid _uuid;
    AssetObjectInfo _info;
    const MarlinTextObject *_source = nullptr;
    /// Shared with the AssetConversion's cache, which keeps them for the next conversions.
    HashMap<String, SharedPtr<Document>> _documents;
    List<DependencyInput> _inputs;
    List<uint8> _block;
    List<uint8> _bulk;
    List<DerivedResult> _derived;
    String _errors;
    List<String> _stale;
    HashSet<Guid> _verified;
};

/// Turns source objects of one type into run-time blocks: an extension point (Documentation/Architecture/13 §3),
/// so every module adds converters the same way, choose between them with `MCLASS(Priority = N)` and
/// wrap one with `MCLASS(Replaces = "Other")`. Converters run in MarlinConverter workers and must be stateless.
/// Bump GetVersion whenever the output changes for the same input: the version is part of the result key.
MCLASS(Abstract, ExtensionPoint)
class ASSETPIPELINE_API AssetConverter
{
    MGENERATED_BODY()
public:
    AssetConverter() = default;
    virtual ~AssetConverter() = default;
    MARLIN_NONCOPYABLE(AssetConverter);

    /// The source type name as written in .masset files, such as TextureSource.
    [[nodiscard]] virtual StringView GetSourceType() const = 0;
    [[nodiscard]] virtual const TypeInfo &GetRuntimeType() const = 0;
    [[nodiscard]] virtual uint32 GetVersion() const = 0;
    virtual bool Convert(ConversionContext &context) = 0;

    /// True if Convert may call ConversionContext::AddDerivedResult; only then does a cache hit look up the derived
    /// results.
    [[nodiscard]] virtual bool ProducesDerivedResults() const
    {
        return false;
    }

    /// Writes what the input *name*, recorded with ReadConverterInput, is now, such as the type and version of each
    /// encoder that could be chosen; false if this converter recorded no such input.
    virtual bool WriteInput(StringView name, StringWriter &text) const
    {
        (void)name;
        (void)text;
        return false;
    }
};

/// The result of AssetConversion::Convert.
struct ConversionResult
{
    bool Succeeded = false;
    bool CacheHit = false;
    Hash128 Key;
    ConvertedResultLocation Location;
    String Errors;
    /// What the result depends on besides its source.
    List<DependencyInput> Inputs;
    /// The UUIDs of the derived results the conversion also produced, which packaging converts as well.
    List<Guid> Derived;
    /// When it failed because source files changed after the index scanned them: their paths. Nothing was stored.
    List<String> Stale;
    /// From ConvertMany without convertMisses: the result is not in GlobalStore and was not converted.
    bool Missing = false;
};

/// Converts by Guid through GlobalStore (06 §3.2): computes the key, finds the result there, or runs the converter and
/// stores it. Types marked `MCLASS(SourceIsRuntime)` convert by reading the source straight into the run-time type.
class ASSETPIPELINE_API AssetConversion
{
public:
    AssetConversion(IAssetSourceResolver &resolver, ConversionStore &store);
    ~AssetConversion();
    MARLIN_NONCOPYABLE(AssetConversion);

    /// A cache hit is decided from the index alone; only a miss reads the source. A *uuid* no source object has is
    /// looked up as a derived result of an earlier conversion.
    ConversionResult Convert(const Guid &uuid, const ConversionTarget &target);
    /// Convert for many objects at once: their dependency records, and then their results, are each looked up in one
    /// pipelined round trip, so a batch of hits costs two; the misses then convert one after another.
    /// Without *convertMisses*, a miss is only marked Missing.
    void ConvertMany(Span<const Guid> uuids, const ConversionTarget &target, List<ConversionResult> &results,
                     bool convertMisses = true);

    /// The converter for a source type name, or null.
    [[nodiscard]] AssetConverter *FindConverter(StringView sourceType) const;
    /// The converter *converter* replaces, if it wraps one.
    [[nodiscard]] AssetConverter *GetReplaced(const AssetConverter &converter) const;

    /// Every source type some converter turns into run-time data.
    [[nodiscard]] List<String> GetSourceTypes() const;

    /// The key of the dependency record of converting *uuid*'s source object for *target*, what it read: the first
    /// half of a lookup. False if no converter takes the object.
    bool GetInputKey(const Guid &uuid, const ConversionTarget &target, Hash128 &inputKey) const;

    /// Promises that config is not reloaded while this conversion lives, as in a one-shot warming run, so the hash
    /// of each config section a key covers is computed once instead of on every lookup.
    void AssumeFixedConfig()
    {
        _fixedConfig = true;
    }

    [[nodiscard]] const ImageDecoderSet &GetImageDecoders() const
    {
        return _imageDecoders;
    }

    [[nodiscard]] const SceneDecoderSet &GetSceneDecoders() const
    {
        return _sceneDecoders;
    }

private:
    friend class ConversionContext;
    struct SourceIsRuntimeConverter;
    struct DocumentCache;

    /// The parsed source file *path*, shared between the conversions of this AssetConversion while the file keeps
    /// its size and modification time, so a file of many objects is not read and parsed once for each of them. Null
    /// with *errors* if it cannot be read.
    SharedPtr<ConversionContext::Document> LoadDocument(const String &path, String &errors) const;

    ConversionResult ConvertObject(const Guid &uuid, const ConversionTarget &target, bool useCache);
    ConversionResult ConvertDerived(const Guid &uuid, const Guid &parent, const ConversionTarget &target);
    /// The converter of *uuid* and its input key; false with *error* set if there is none.
    bool FindInputKey(const Guid &uuid, const ConversionTarget &target, AssetConverter *&converter, Hash128 &inputKey,
                      String &error) const;
    /// The result key *inputs*, a dependency record, complete *inputKey* to with their current hashes.
    bool ComputeResultKey(const AssetConverter *converter, const ConversionTarget &target, const Hash128 &inputKey,
                          Span<const DependencyInput> inputs, Hash128 &key);
    bool ComputeResultKey(ConversionContext &context, const Hash128 &inputKey, Span<const DependencyInput> inputs,
                          Hash128 &key);
    /// Reads the source, converts and stores the result and its dependency record; *missed* is the last result key
    /// a dependency record led to, which redirects to this record when its inputs differ.
    ConversionResult Run(const Guid &uuid, const ConversionTarget &target, AssetConverter *converter,
                         const Hash128 &inputKey, const Hash128 *missed = nullptr);

    IAssetSourceResolver &_resolver;
    ConversionStore &_store;
    ExtensionSet<AssetConverter> _extensions;
    ImageDecoderSet _imageDecoders;
    SceneDecoderSet _sceneDecoders;
    List<UniquePtr<AssetConverter>> _implicit;
    HashMap<String, AssetConverter *> _converters;
    bool _fixedConfig = false;
    /// With _fixedConfig, the hash of each config section by target and section.
    mutable HashMap<String, Hash128> _configHashes;
    UniquePtr<DocumentCache> _documents;
};

} // namespace Marlin

