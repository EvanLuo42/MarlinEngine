#include "AssetPipeline/AssetConverter.h"

#include "AssetPipeline/AssetCanonical.h"

#include "Asset/Asset.h"
#include "Asset/AssetBlock.h"
#include "Concurrency/ScopeLock.h"
#include "Config/Config.h"
#include "Diagnostics/AssertFormat.h"
#include "HAL/PlatformSync.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
Hash128 HashText(StringView text)
{
    return Xxh3Hash128(text.Data(), text.Length());
}

/// Writes the fields at dotted *path* among *fields*, such as `Compression.Color`, with what decides how a layer
/// applies them: each field's operation (assign, `+=`, `-=`) and its `@if`. A field assigned as a whole without the
/// rest of the path is written whole. Other parts of the layer are left out, so editing them changes nothing here.
void WriteConfigPart(Span<const MarlinTextField> fields, StringView path, StringWriter &text)
{
    StringView key = path;
    StringView rest;
    if (!path.SplitFirst('.', key, rest))
    {
        rest = {};
    }
    for (const MarlinTextField &field : fields)
    {
        if (field.IsDirective || field.Key != key)
        {
            continue;
        }
        AssetCanonical::WriteNumber(text, static_cast<uint64>(field.Operation));
        for (const MarlinTextCondition *condition = field.Condition; condition != nullptr;
             condition = condition->Parent)
        {
            AssetCanonical::WriteText(text, condition->Variable);
            AssetCanonical::WriteText(text, condition->Value);
            AssetCanonical::WriteNumber(text, condition->Equal ? 1 : 0);
        }
        AssetCanonical::WriteNumber(text, field.InElse ? 1 : 0);
        if (!rest.IsEmpty() && field.Value.Kind == MarlinTextValueKind::Block)
        {
            text.Append('{');
            WriteConfigPart(field.Value.Fields, rest, text);
            text.Append('}');
        }
        else
        {
            AssetCanonical::WriteValue(text, field.Value);
        }
    }
}

/// The dependency record naming which conversion produced the derived result *uuid*.
Hash128 DerivedParentKey(const Guid &uuid)
{
    StringBuilder<64> text;
    text.Append("Derived/");
    uuid.FormatTo(text, "N");
    return ConversionStore::DependencyKey(text.AsView());
}

/// The dependency record listing the derived results of the conversion whose result key is *key*: each one's Uuid,
/// with its result key in Section.
Hash128 DerivedListKey(const Hash128 &key)
{
    StringBuilder<64> text;
    // "2": the lists from before carried no result keys.
    text.Append("DerivedList2/").Append(FormatResultKey(key).AsView());
    return ConversionStore::DependencyKey(text.AsView());
}

/// The result key of derived result *uuid* with *block*: by its content, so a cell a level edit left as it was keeps
/// its key, and whoever loaded it has nothing to reload.
Hash128 DerivedKey(const Guid &uuid, Span<const uint8> block)
{
    StringBuilder<96> text("Derived/");
    uuid.FormatTo(text, "N");
    text.Append('/').Append(FormatResultKey(Xxh3Hash128(block.Data(), block.Count())).AsView());
    return ConversionStore::ResultKey(text.AsView());
}
} // namespace

struct ConversionContext::Document
{
    /// Objects are parsed on first use, through the document's arena: one conversion at a time.
    Mutex Lock;
    MarlinTextDocument Text;
};

struct AssetConversion::DocumentCache
{
    /// How many parsed files are kept: enough for the files a batch of conversions shares.
    static constexpr usize Capacity = 64;

    struct Entry
    {
        uint64 Size = 0;
        int64 ModifiedTime = 0;
        uint64 LastUse = 0;
        SharedPtr<ConversionContext::Document> Document;
    };

    Mutex Lock;
    HashMap<String, Entry> Entries;
    uint64 Clock = 0;
};

SharedPtr<ConversionContext::Document> AssetConversion::LoadDocument(const String &path, String &errors) const
{
    FileInfo info;
    const bool known = Vfs::GetInfo(path.AsView(), info) && !info.IsDirectory && info.ModifiedTime != 0;
    if (known)
    {
        const ScopeLock lock(_documents->Lock);
        if (DocumentCache::Entry *entry = _documents->Entries.Find(path);
            entry != nullptr && entry->Size == info.Size && entry->ModifiedTime == info.ModifiedTime)
        {
            entry->LastUse = ++_documents->Clock;
            return entry->Document;
        }
    }
    String text;
    SharedPtr<ConversionContext::Document> loaded(new ConversionContext::Document());
    MarlinTextDiagnostics diagnostics;
    if (!Vfs::ReadAll(path.AsView(), text) || !loaded->Text.Parse(text.AsView(), diagnostics))
    {
        errors = diagnostics.ToString(path.AsView());
        return nullptr;
    }
    if (known)
    {
        // The time is from before the read, so a change during it is seen next time.
        const ScopeLock lock(_documents->Lock);
        if (_documents->Entries.Count() >= DocumentCache::Capacity && _documents->Entries.Find(path) == nullptr)
        {
            const String *oldest = nullptr;
            uint64 oldestUse = ~uint64{0};
            for (const auto &[cached, entry] : _documents->Entries)
            {
                if (entry.LastUse < oldestUse)
                {
                    oldestUse = entry.LastUse;
                    oldest = &cached;
                }
            }
            if (oldest != nullptr)
            {
                const String evicted = *oldest;
                _documents->Entries.Remove(evicted);
            }
        }
        DocumentCache::Entry &entry = _documents->Entries.FindOrAdd(path);
        entry.Size = info.Size;
        entry.ModifiedTime = info.ModifiedTime;
        entry.LastUse = ++_documents->Clock;
        entry.Document = loaded;
    }
    return loaded;
}

const ConfigView *GetConversionConfig(const ConversionTarget &target)
{
    ConfigTarget config;
    config.Platform = target.Platform;
    config.Variables.Add({String("Target"), String(target.Kind == TargetKind::Server ? "Server" : "Game")});
    return Config::GetView(config);
}

ConversionContext::ConversionContext(IAssetSourceResolver &resolver, const ConversionTarget &target)
    : _resolver(resolver), _target(target)
{
}

ConversionContext::~ConversionContext() = default;

const MarlinTextObject *ConversionContext::LoadObject(const Guid &uuid, AssetObjectInfo *info)
{
    AssetObjectInfo found;
    if (!_resolver.FindObject(uuid, found))
    {
        Error(Format("no source object has UUID {0}", uuid));
        return nullptr;
    }
    SharedPtr<Document> *document = _documents.Find(found.Path);
    if (document == nullptr)
    {
        SharedPtr<Document> loaded;
        String errors;
        if (_conversion != nullptr)
        {
            loaded = _conversion->LoadDocument(found.Path, errors);
        }
        else
        {
            String text;
            loaded = SharedPtr<Document>(new Document());
            MarlinTextDiagnostics diagnostics;
            if (!Vfs::ReadAll(found.Path.AsView(), text) || !loaded->Text.Parse(text.AsView(), diagnostics))
            {
                errors = diagnostics.ToString(found.Path.AsView());
                loaded = nullptr;
            }
        }
        if (!loaded.IsValid())
        {
            Error(Format("{0} cannot be read: {1}", found.Path, errors));
            return nullptr;
        }
        document = &_documents.FindOrAdd(found.Path);
        *document = Move(loaded);
    }
    const MarlinTextObject *object = nullptr;
    {
        const ScopeLock lock((*document)->Lock);
        MarlinTextDiagnostics diagnostics;
        const usize index = (*document)->Text.FindObject(uuid, diagnostics);
        object = index != StringView::NotFound ? (*document)->Text.GetObject(index, diagnostics) : nullptr;
    }
    if (object == nullptr)
    {
        Stale(found.Path.AsView());
        return nullptr;
    }
    Hash128 indexed;
    if (!_verified.Contains(uuid) && _resolver.GetCanonicalHash(uuid, indexed))
    {
        if (AssetCanonical::Hash(*object) != indexed)
        {
            Stale(found.Path.AsView());
            return nullptr;
        }
        _verified.Add(uuid);
    }
    if (info != nullptr)
    {
        *info = Move(found);
    }
    return object;
}

bool ConversionContext::Begin(const Guid &uuid)
{
    _uuid = uuid;
    _source = LoadObject(uuid, &_info);
    return _source != nullptr;
}

void ConversionContext::Record(DependencyInputKind kind, const Guid &uuid, StringView section)
{
    for (const DependencyInput &input : _inputs)
    {
        if (input.Kind == kind && input.Uuid == uuid && input.Section == section)
        {
            return;
        }
    }
    DependencyInput &input = _inputs.AddDefaulted();
    input.Kind = kind;
    input.Uuid = uuid;
    input.Section = section;
}

bool ConversionContext::ReadSource(const TypeInfo &type, void *value)
{
    MarlinTextDiagnostics diagnostics;
    MarlinTextReadOptions options;
    options.IgnoreTypeName = true;
    options.ResolveParent = [this](const MarlinTextValue &link) { return ReadDependency(link.LinkGuid); };
    const bool ok = MarlinTextSerializer::ReadObject(*_source, type, value, diagnostics, options);
    if (!ok || diagnostics.HasErrors())
    {
        Error(diagnostics.ToString(_info.Path.AsView()).AsView());
        return false;
    }
    return true;
}

const MarlinTextObject *ConversionContext::ReadDependency(const Guid &uuid)
{
    Record(DependencyInputKind::Object, uuid);
    return LoadObject(uuid, nullptr);
}

bool ConversionContext::ReadBinary(const Guid &uuid, List<uint8> &bytes)
{
    Record(DependencyInputKind::Binary, uuid);
    AssetBinaryInfo info;
    if (!_resolver.FindBinary(uuid, info) || !Vfs::ReadAll(info.Path.AsView(), bytes))
    {
        Error(Format("binary file {0} cannot be read", uuid));
        return false;
    }
    Hash128 indexed;
    if (_resolver.GetBinaryHash(uuid, indexed) && Xxh3Hash128(bytes.Data(), bytes.Count()) != indexed)
    {
        Stale(info.Path.AsView());
        return false;
    }
    return true;
}

bool ConversionContext::ReadReferencedBinary(StringView file, StringView uri, uint64 offset, uint64 size,
                                             List<uint8> &bytes)
{
    String path;
    if (!ResolveReferencedPath(file, uri, path))
    {
        Error(Format("{0} names {1}, which is outside its mount", file, uri));
        return false;
    }
    AssetBinaryInfo info;
    if (!_resolver.FindBinaryAt(path.AsView(), info))
    {
        Error(Format("{0} names {1}, which is not an indexed file", file, path));
        return false;
    }
    if (offset == 0 && size == ISourceFileReader::ToEnd)
    {
        return ReadBinary(info.Uuid, bytes);
    }
    Record(DependencyInputKind::Binary, info.Uuid);
    FileInfo now;
    if (!Vfs::GetInfo(path.AsView(), now))
    {
        Error(Format("binary file {0} cannot be read", path));
        return false;
    }
    if (now.Size != info.Size || now.ModifiedTime != info.ModifiedTime)
    {
        Stale(path.AsView());
        return false;
    }
    const uint64 end = size == ISourceFileReader::ToEnd ? now.Size : offset + size;
    if (offset > now.Size || end > now.Size || end < offset)
    {
        Error(Format("{0} has {1} bytes, fewer than the {2} to {3} read", path, now.Size, offset, end));
        return false;
    }
    const AsyncReadRequest read = Vfs::ReadAsync(path.AsView(), IoPriority::Critical, offset, end - offset);
    if (!read.Wait() || !read.Succeeded() || read.GetData().Count() != end - offset)
    {
        Error(Format("binary file {0} cannot be read", path));
        return false;
    }
    bytes.Clear();
    bytes.AddRange(read.GetData());
    return true;
}

bool ConversionContext::Decode(const Guid &uuid, DecodedImage &image)
{
    AssetBinaryInfo info;
    if (!_resolver.FindBinary(uuid, info))
    {
        Error(Format("binary file {0} cannot be found", uuid));
        return false;
    }
    StringView directory;
    StringView extension;
    if (!info.Path.AsView().SplitLast('.', directory, extension))
    {
        extension = {};
    }
    Record(DependencyInputKind::Decoder, uuid, extension);
    List<uint8> bytes;
    if (!ReadBinary(uuid, bytes))
    {
        return false;
    }
    ImageDecoder *decoder =
        _conversion != nullptr ? _conversion->GetImageDecoders().Choose(extension, bytes.AsSpan()) : nullptr;
    if (decoder == nullptr)
    {
        Error(Format("no image decoder reads {0}", info.Path));
        return false;
    }
    StringBuilder<256> error;
    if (!decoder->Decode(bytes.AsSpan(), image, error))
    {
        Error(Format("{0} cannot be decoded: {1}", info.Path, error.AsView()));
        return false;
    }
    return true;
}

bool ConversionContext::Decode(const Guid &uuid, DecodedScene &scene, StringView mesh)
{
    AssetBinaryInfo info;
    if (!_resolver.FindBinary(uuid, info))
    {
        Error(Format("binary file {0} cannot be found", uuid));
        return false;
    }
    StringView directory;
    StringView extension;
    if (!info.Path.AsView().SplitLast('.', directory, extension))
    {
        extension = {};
    }
    Record(DependencyInputKind::SceneDecoder, uuid, extension);
    List<uint8> bytes;
    if (!ReadBinary(uuid, bytes))
    {
        return false;
    }
    SceneDecoder *decoder =
        _conversion != nullptr ? _conversion->GetSceneDecoders().Choose(extension, bytes.AsSpan()) : nullptr;
    if (decoder == nullptr)
    {
        Error(Format("no scene decoder reads {0}", info.Path));
        return false;
    }
    // The files the scene names are inputs too, read through the index like the scene itself.
    class Files final : public ISourceFileReader
    {
    public:
        Files(ConversionContext &context, StringView path) : _context(context), _path(path)
        {
        }

        bool ReadReferencedRange(StringView uri, uint64 offset, uint64 size, List<uint8> &bytes,
                                 StringWriter &error) override
        {
            if (_context.ReadReferencedBinary(_path, uri, offset, size, bytes))
            {
                return true;
            }
            FormatTo(error, "{0} cannot be read", uri);
            return false;
        }

    private:
        ConversionContext &_context;
        StringView _path;
    } files(*this, info.Path.AsView());
    StringBuilder<256> error;
    if (!decoder->Decode(bytes.AsSpan(), files, mesh, scene, error))
    {
        Error(Format("{0} cannot be decoded: {1}", info.Path, error.AsView()));
        return false;
    }
    return true;
}

bool ConversionContext::ReadConfig(StringView section, const TypeInfo &type, void *value, StringView part)
{
    const String recorded = part.IsEmpty() ? String(section) : Format("{0}#{1}", section, part);
    Record(DependencyInputKind::Config, Guid(), recorded.AsView());
    const ConfigView *config = GetConversionConfig(_target);
    if (config == nullptr)
    {
        Error(Format("the config of {0} cannot be read", _target.Platform));
        return false;
    }
    return config->Apply(section, type, value);
}

void ConversionContext::ReadConverterInput(StringView name)
{
    Record(DependencyInputKind::Converter, Guid(), name);
}

AssetConverter *ConversionContext::GetReplaced(const AssetConverter &converter) const
{
    return _conversion != nullptr ? _conversion->GetReplaced(converter) : nullptr;
}

void ConversionContext::SetResult(const Asset &result)
{
    _block.Clear();
    if (!AssetBlock::Write(result, _uuid, _block))
    {
        Error(Format("{0} cannot be written as a block", result.GetType().GetName()));
    }
}

void ConversionContext::AddDerivedResult(const Guid &uuid, const Asset &result)
{
    DerivedResult &derived = _derived.AddDefaulted();
    derived.Uuid = uuid;
    if (!AssetBlock::Write(result, uuid, derived.Block))
    {
        Error(Format("{0} cannot be written as a block", result.GetType().GetName()));
    }
}

void ConversionContext::SetBulk(List<uint8> bulk)
{
    _bulk = Move(bulk);
}

void ConversionContext::Error(StringView message)
{
    _errors.Append(message);
    _errors.Append("\n");
}

void ConversionContext::Stale(StringView path)
{
    Error(Format("{0} changed after the asset index read it", path));
    if (!_stale.Contains(String(path)))
    {
        _stale.Add(String(path));
    }
}

bool ConversionContext::HashObject(const Guid &uuid, Hash128 &hash)
{
    const MarlinTextObject *object = LoadObject(uuid, nullptr);
    if (object == nullptr)
    {
        return false;
    }
    hash = AssetCanonical::Hash(*object);
    return true;
}

bool ConversionContext::HashInput(const DependencyInput &input, Hash128 &hash)
{
    switch (input.Kind)
    {
    case DependencyInputKind::Object:
        return _resolver.GetCanonicalHash(input.Uuid, hash);
    case DependencyInputKind::Binary:
        return _resolver.GetBinaryHash(input.Uuid, hash);
    case DependencyInputKind::Config: {
        String memo;
        if (_conversion != nullptr && _conversion->_fixedConfig)
        {
            memo = Format("{0}/{1}/{2}", _target.Platform, static_cast<uint32>(_target.Kind), input.Section);
            if (const Hash128 *known = _conversion->_configHashes.Find(memo))
            {
                hash = *known;
                return true;
            }
        }
        const ConfigView *config = GetConversionConfig(_target);
        if (config == nullptr)
        {
            return false;
        }
        // `Engine.Textures#Compression.Color`: only that part of each layer (ReadConfig's *part*).
        StringView section = input.Section.AsView();
        StringView part;
        if (!input.Section.AsView().SplitFirst('#', section, part))
        {
            part = {};
        }
        StringBuilder<4096> text;
        config->ForEachSection(section, [&](const MarlinTextObject &layer, StringView source) {
            AssetCanonical::WriteText(text, source);
            if (part.IsEmpty())
            {
                AssetCanonical::Write(layer, text);
            }
            else
            {
                WriteConfigPart(layer.GetFields(), part, text);
            }
        });
        hash = HashText(text.AsView());
        if (!memo.IsEmpty())
        {
            _conversion->_configHashes.FindOrAdd(memo) = hash;
        }
        return true;
    }
    case DependencyInputKind::Decoder: {
        if (_conversion == nullptr)
        {
            return false;
        }
        StringBuilder<512> text;
        for (const ImageDecoder *decoder : _conversion->GetImageDecoders().Find(input.Section.AsView()))
        {
            AssetCanonical::WriteText(text, decoder->GetType().GetFullName());
            AssetCanonical::WriteNumber(text, decoder->GetVersion());
        }
        hash = HashText(text.AsView());
        return true;
    }
    case DependencyInputKind::SceneDecoder: {
        if (_conversion == nullptr)
        {
            return false;
        }
        StringBuilder<512> text;
        for (const SceneDecoder *decoder : _conversion->GetSceneDecoders().Find(input.Section.AsView()))
        {
            AssetCanonical::WriteText(text, decoder->GetType().GetFullName());
            AssetCanonical::WriteNumber(text, decoder->GetVersion());
        }
        hash = HashText(text.AsView());
        return true;
    }
    case DependencyInputKind::Converter: {
        for (const AssetConverter *link = _converter; link != nullptr && _conversion != nullptr;
             link = _conversion->GetReplaced(*link))
        {
            StringBuilder<512> text;
            if (link->WriteInput(input.Section.AsView(), text))
            {
                hash = HashText(text.AsView());
                return true;
            }
        }
        return false;
    }
    }
    return false;
}

/// Reads the source straight into the run-time type, for types marked SourceIsRuntime.
struct AssetConversion::SourceIsRuntimeConverter final : AssetConverter
{
    explicit SourceIsRuntimeConverter(const TypeInfo &type) : Type(type)
    {
    }

    [[nodiscard]] StringView GetSourceType() const override
    {
        return Type.GetName();
    }

    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return Type;
    }

    [[nodiscard]] uint32 GetVersion() const override
    {
        return Type.GetVersion();
    }

    bool Convert(ConversionContext &context) override
    {
        void *raw = Type.New();
        if (raw == nullptr)
        {
            context.Error(Format("{0} cannot be constructed", Type.GetName()));
            return false;
        }
        auto *asset = static_cast<Asset *>(Type.CastTo(raw, TypeOf<Asset>()));
        const bool ok = context.ReadSource(Type, raw);
        if (ok)
        {
            asset->Name = FrozenString(context.GetSource().GetName());
            context.SetResult(*asset);
        }
        Type.GetOperations().Delete(raw);
        return ok && !context.HasFailed();
    }

    const TypeInfo &Type;
};

AssetConversion::AssetConversion(IAssetSourceResolver &resolver, ConversionStore &store)
    : _resolver(resolver), _store(store), _documents(MakeUnique<DocumentCache>())
{
    List<String> disabled;
    (void)Config::TryGet("Engine.Extensions.Disabled", disabled);
    ExtensionContext context;
    context.Disabled = disabled.AsSpan();
    _extensions.Build(context);
    _imageDecoders.Build(context);
    _sceneDecoders.Build(context);
    HashMap<String, int64> priorities;
    for (AssetConverter *converter : _extensions)
    {
        const String sourceType(converter->GetSourceType());
        int64 priority = 0;
        (void)TryParse(converter->GetType().GetMetadata().GetOr("Priority", "0"), priority);
        AssetConverter **existing = _converters.Find(sourceType);
        if (existing == nullptr)
        {
            _converters.FindOrAdd(sourceType) = converter;
            priorities.FindOrAdd(sourceType) = priority;
            continue;
        }
        if (*priorities.Find(sourceType) == priority)
        {
            MARLIN_FATALF("{0} and {1} both convert {2} at priority {3}; give one a higher Priority, Replaces the "
                          "other, or disable one in Engine.Extensions",
                          (*existing)->GetType().GetName(), converter->GetType().GetName(), sourceType, priority);
        }
    }
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(TypeOf<Asset>(), types);
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() == TypeKind::Class && type->GetMetadata().Has("SourceIsRuntime") &&
            !_converters.Contains(String(type->GetName())))
        {
            UniquePtr<AssetConverter> converter(new SourceIsRuntimeConverter(*type));
            _converters.FindOrAdd(String(type->GetName())) = converter.Get();
            _implicit.Add(Move(converter));
        }
    }
}

AssetConversion::~AssetConversion() = default;

AssetConverter *AssetConversion::FindConverter(StringView sourceType) const
{
    AssetConverter *const *converter = _converters.Find(String(sourceType));
    return converter != nullptr ? *converter : nullptr;
}

AssetConverter *AssetConversion::GetReplaced(const AssetConverter &converter) const
{
    return _extensions.GetReplaced(converter);
}

List<String> AssetConversion::GetSourceTypes() const
{
    List<String> types;
    for (const auto &entry : _converters)
    {
        types.Add(entry.Key);
    }
    types.Sort();
    return types;
}

ConversionResult AssetConversion::Convert(const Guid &uuid, const ConversionTarget &target)
{
    List<ConversionResult> results;
    ConvertMany(Span<const Guid>(&uuid, 1), target, results);
    return Move(results[0]);
}

void AssetConversion::ConvertMany(Span<const Guid> uuids, const ConversionTarget &target,
                                  List<ConversionResult> &results, bool convertMisses)
{
    results.Clear();
    results.Resize(uuids.Count());
    // Phase one: the input keys, from the index alone, and one pipelined round trip for their dependency records.
    List<AssetConverter *> converters;
    converters.Resize(uuids.Count(), nullptr);
    List<Hash128> inputKeys;
    List<usize> lookedUp;
    for (usize i = 0; i < uuids.Count(); ++i)
    {
        AssetObjectInfo info;
        if (!_resolver.FindObject(uuids[i], info))
        {
            List<DependencyInput> parent;
            if (_store.GetDependencies(DerivedParentKey(uuids[i]), parent) && parent.Count() == 1)
            {
                results[i] = ConvertDerived(uuids[i], parent[0].Uuid, target);
            }
            else
            {
                results[i].Errors = Format("no source object has UUID {0}", uuids[i]);
            }
            continue;
        }
        Hash128 inputKey;
        if (FindInputKey(uuids[i], target, converters[i], inputKey, results[i].Errors))
        {
            inputKeys.Add(inputKey);
            lookedUp.Add(i);
        }
    }
    List<List<DependencyInput>> records;
    List<bool> recorded;
    (void)_store.GetDependencies(inputKeys.AsSpan(), records, recorded);

    // Phase two: the result keys those records complete with the inputs' current hashes, and one round trip for
    // their headers; a result that is there is a hit, without a source file opened. A key that misses may have a
    // redirect to the record of a conversion that read other inputs (ConversionStore::RedirectKey): those are
    // looked up in one more round trip each, a few times at most.
    List<Hash128> resultKeys;
    List<usize> completed;
    for (usize j = 0; j < lookedUp.Count(); ++j)
    {
        Hash128 key;
        if (recorded[j] && ComputeResultKey(converters[lookedUp[j]], target, inputKeys[j], records[j].AsSpan(), key))
        {
            resultKeys.Add(key);
            completed.Add(j);
        }
    }
    List<bool> hit;
    hit.Resize(lookedUp.Count(), false);
    /// The last key a record led to that missed, where a conversion that reads other inputs redirects from.
    List<Hash128> missed;
    missed.Resize(lookedUp.Count());
    List<bool> hasMissed;
    hasMissed.Resize(lookedUp.Count(), false);
    for (uint32 round = 0; !completed.IsEmpty(); ++round)
    {
        List<ConvertedResultLocation> locations;
        List<bool> located;
        (void)_store.LocateResults(resultKeys.AsSpan(), locations, located);
        List<Hash128> redirects;
        List<usize> redirected;
        for (usize k = 0; k < completed.Count(); ++k)
        {
            const usize j = completed[k];
            const usize i = lookedUp[j];
            if (!located[k])
            {
                missed[j] = resultKeys[k];
                hasMissed[j] = true;
                redirects.Add(ConversionStore::RedirectKey(resultKeys[k]));
                redirected.Add(j);
                continue;
            }
            ConversionResult &result = results[i];
            result.Succeeded = true;
            result.CacheHit = true;
            result.Key = resultKeys[k];
            result.Location = Move(locations[k]);
            result.Inputs = Move(records[j]);
            List<DependencyInput> derived;
            if (converters[i]->ProducesDerivedResults() && _store.GetDependencies(DerivedListKey(result.Key), derived))
            {
                for (const DependencyInput &entry : derived)
                {
                    result.Derived.Add(entry.Uuid);
                }
            }
            hit[j] = true;
        }
        resultKeys.Clear();
        completed.Clear();
        if (redirects.IsEmpty() || round == ConversionStore::MaxRedirects)
        {
            break;
        }
        List<List<DependencyInput>> followed;
        List<bool> found;
        (void)_store.GetDependencies(redirects.AsSpan(), followed, found);
        for (usize k = 0; k < redirected.Count(); ++k)
        {
            const usize j = redirected[k];
            Hash128 key;
            if (found[k] &&
                ComputeResultKey(converters[lookedUp[j]], target, inputKeys[j], followed[k].AsSpan(), key))
            {
                records[j] = Move(followed[k]);
                resultKeys.Add(key);
                completed.Add(j);
            }
        }
    }

    // Phase three: the misses read their sources and convert, if asked to.
    for (usize j = 0; j < lookedUp.Count(); ++j)
    {
        if (!hit[j])
        {
            if (convertMisses)
            {
                results[lookedUp[j]] = Run(uuids[lookedUp[j]], target, converters[lookedUp[j]], inputKeys[j],
                                           hasMissed[j] ? &missed[j] : nullptr);
            }
            else
            {
                results[lookedUp[j]].Missing = true;
            }
        }
    }
}

ConversionResult AssetConversion::ConvertObject(const Guid &uuid, const ConversionTarget &target, bool useCache)
{
    if (useCache)
    {
        return Convert(uuid, target);
    }
    ConversionResult result;
    AssetConverter *converter = nullptr;
    Hash128 inputKey;
    if (!FindInputKey(uuid, target, converter, inputKey, result.Errors))
    {
        return result;
    }
    return Run(uuid, target, converter, inputKey);
}

ConversionResult AssetConversion::ConvertDerived(const Guid &uuid, const Guid &parent, const ConversionTarget &target)
{
    for (const bool useCache : {true, false})
    {
        ConversionResult result = ConvertObject(parent, target, useCache);
        if (!result.Succeeded)
        {
            return result;
        }
        List<DependencyInput> derived;
        Hash128 key;
        bool listed = false;
        if (_store.GetDependencies(DerivedListKey(result.Key), derived))
        {
            for (const DependencyInput &entry : derived)
            {
                listed = listed || (entry.Uuid == uuid && ParseResultKey(entry.Section.AsView(), key));
            }
        }
        if (listed && _store.LocateResult(key, result.Location))
        {
            result.Key = key;
            result.Inputs.Insert(0, DependencyInput{DependencyInputKind::Object, parent, {}});
            result.Derived.Clear();
            return result;
        }
    }
    ConversionResult missing;
    missing.Errors = Format("converting {0} no longer produces {1}", parent, uuid);
    return missing;
}

bool AssetConversion::GetInputKey(const Guid &uuid, const ConversionTarget &target, Hash128 &inputKey) const
{
    AssetConverter *converter = nullptr;
    String error;
    return FindInputKey(uuid, target, converter, inputKey, error);
}

bool AssetConversion::FindInputKey(const Guid &uuid, const ConversionTarget &target, AssetConverter *&converter,
                                   Hash128 &inputKey, String &error) const
{
    AssetObjectInfo info;
    Hash128 sourceHash;
    if (!_resolver.FindObject(uuid, info))
    {
        error = Format("no source object has UUID {0}", uuid);
        return false;
    }
    if (!_resolver.GetCanonicalHash(uuid, sourceHash))
    {
        error = Format("the asset index has no canonical hash for {0} ({1})", uuid, info.Path);
        return false;
    }
    converter = FindConverter(info.Type.AsView());
    if (converter == nullptr)
    {
        error = Format("no converter turns {0} into run-time data", info.Type);
        return false;
    }

    StringBuilder<1024> head;
    for (const AssetConverter *link = converter; link != nullptr; link = GetReplaced(*link))
    {
        AssetCanonical::WriteText(head, link->GetType().GetFullName());
        AssetCanonical::WriteNumber(head, link->GetVersion());
        head.Append('/');
    }
    AssetCanonical::WriteText(head, converter->GetSourceType());
    AssetCanonical::WriteNumber(head, AssetBlock::GetLayoutHash(converter->GetRuntimeType()));
    head.Append('/');
    AssetCanonical::WriteNumber(head, AssetBlockVersion);
    head.Append('/');
    AssetCanonical::WriteText(head, target.Platform);
    AssetCanonical::WriteNumber(head, static_cast<uint64>(target.Kind));
    uuid.FormatTo(head, "N");
    head.Append(FormatResultKey(sourceHash).AsView());
    inputKey = ConversionStore::DependencyKey(head.AsView());
    return true;
}

bool AssetConversion::ComputeResultKey(const AssetConverter *converter, const ConversionTarget &target,
                                       const Hash128 &inputKey, Span<const DependencyInput> inputs, Hash128 &key)
{
    ConversionContext context(_resolver, target);
    context._conversion = this;
    context._converter = converter;
    return ComputeResultKey(context, inputKey, inputs, key);
}

bool AssetConversion::ComputeResultKey(ConversionContext &context, const Hash128 &inputKey,
                                       Span<const DependencyInput> inputs, Hash128 &key)
{
    StringBuilder<1024> text;
    text.Append(FormatResultKey(inputKey).AsView());
    for (const DependencyInput &input : inputs)
    {
        Hash128 hash;
        if (!context.HashInput(input, hash))
        {
            return false;
        }
        AssetCanonical::WriteNumber(text, static_cast<uint64>(input.Kind));
        input.Uuid.FormatTo(text, "N");
        AssetCanonical::WriteText(text, input.Section);
        text.Append(FormatResultKey(hash).AsView());
    }
    key = ConversionStore::ResultKey(text.AsView());
    return true;
}

ConversionResult AssetConversion::Run(const Guid &uuid, const ConversionTarget &target, AssetConverter *converter,
                                      const Hash128 &inputKey, const Hash128 *missed)
{
    ConversionResult result;
    ConversionContext context(_resolver, target);
    context._conversion = this;
    context._converter = converter;
    if (!context.Begin(uuid))
    {
        result.Errors = context.GetErrors();
        result.Stale.AddRange(context.GetStalePaths());
        return result;
    }
    const bool converted = converter->Convert(context) && !context.HasFailed() && !context.GetBlock().IsEmpty();
    if (!converted)
    {
        result.Errors = context.HasFailed() ? String(context.GetErrors()) : String("the converter wrote no block");
        result.Stale.AddRange(context.GetStalePaths());
        return result;
    }
    // The block header learns the bulk's size in place: the block is the context's, and only written from here on.
    List<uint8> &block = context._block;
    AssetBlockHeader header;
    MemCopy(&header, block.Data(), sizeof(header));
    header.BulkSize = context.GetBulk().Count();
    MemCopy(block.Data(), &header, sizeof(header));
    // The result goes in before the record that leads to it, so a record found always has its result. The record
    // under the input key is the first conversion's; one that read other inputs than the record that led here
    // redirects from the key that record gave.
    if (!ComputeResultKey(context, inputKey, context.GetInputs(), result.Key) ||
        !_store.PutResult(result.Key, block.AsSpan(), context.GetBulk(), result.Location) ||
        (missed != nullptr && *missed != result.Key &&
         !_store.PutDependencies(ConversionStore::RedirectKey(*missed), context.GetInputs())) ||
        !_store.PutDependencies(inputKey, context.GetInputs()))
    {
        result.Errors = context.HasFailed() ? String(context.GetErrors()) : String("GlobalStore cannot be written");
        return result;
    }
    List<DependencyInput> derived;
    for (const ConversionContext::DerivedResult &extra : context.GetDerivedResults())
    {
        const DependencyInput parent{DependencyInputKind::Object, uuid, {}};
        const Hash128 key = DerivedKey(extra.Uuid, extra.Block.AsSpan());
        ConvertedResultLocation location;
        if (!_store.PutResult(key, extra.Block.AsSpan(), {}, location) ||
            !_store.PutDependencies(DerivedParentKey(extra.Uuid), Span<const DependencyInput>(&parent, 1)))
        {
            result.Errors = String("GlobalStore cannot be written");
            return result;
        }
        derived.Add({DependencyInputKind::Object, extra.Uuid, FormatResultKey(key)});
        result.Derived.Add(extra.Uuid);
    }
    if (!derived.IsEmpty() && !_store.PutDependencies(DerivedListKey(result.Key), derived.AsSpan()))
    {
        result.Errors = String("GlobalStore cannot be written");
        return result;
    }
    result.Succeeded = true;
    result.Inputs.AddRange(context.GetInputs());
    return result;
}
} // namespace Marlin
