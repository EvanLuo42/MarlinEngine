#include "AssetPipeline/AssetScanner.h"

#include "AssetPipeline/AssetCanonical.h"
#include "Concurrency/Jobs.h"
#include "Config/Config.h"
#include "Engine/Engine.h"

#include "Containers/HashSet.h"
#include "HAL/PlatformFile.h"
#include "Hash/Xxh3.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Reflection/TypeOf.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
constexpr StringView FilesName = ".mfiles";

String Join(StringView a, StringView b)
{
    StringBuilder<512> path(a);
    if (!b.IsEmpty())
    {
        path.Append('/').Append(b);
    }
    return path.ToString();
}

String FormatHash(const Hash128 &hash)
{
    StringBuilder<40> text;
    constexpr char Digits[] = "0123456789abcdef";
    for (int32 shift = 60; shift >= 0; shift -= 4)
    {
        text.Append(Digits[(hash.High >> shift) & 15]);
    }
    for (int32 shift = 60; shift >= 0; shift -= 4)
    {
        text.Append(Digits[(hash.Low >> shift) & 15]);
    }
    return text.ToString();
}

bool IsAssetFile(StringView path)
{
    return path.EndsWith(".masset");
}

/// Files the index leaves alone: .mfiles itself, editor swap files and dotfiles.
bool IsIgnored(StringView name)
{
    return name.IsEmpty() || name.First() == '.' || name.EndsWith("~") || name.EndsWith(".tmp");
}

/// Gathers the links of one object, each once.
class LinkCollector
{
public:
    void Begin(AssetObjectRecord &object)
    {
        _object = &object;
        _links.Clear();
        _binaries.Clear();
    }

    void Collect(const MarlinTextValue &value)
    {
        switch (value.Kind)
        {
        case MarlinTextValueKind::ObjectLink:
            if (value.LinkGuid.IsValid() && _links.Add(value.LinkGuid))
            {
                _object->Links.Add(value.LinkGuid);
            }
            return;
        case MarlinTextValueKind::BinaryLink:
            if (value.LinkGuid.IsValid() && _binaries.Add(value.LinkGuid))
            {
                _object->BinaryLinks.Add(value.LinkGuid);
            }
            return;
        case MarlinTextValueKind::Tuple:
        case MarlinTextValueKind::Array:
            for (const MarlinTextValue &element : value.Elements)
            {
                Collect(element);
            }
            return;
        case MarlinTextValueKind::Block:
        case MarlinTextValueKind::Object:
            for (const MarlinTextField &field : value.Fields)
            {
                Collect(field.Value);
            }
            return;
        default:
            return;
        }
    }

private:
    AssetObjectRecord *_object = nullptr;
    HashSet<Guid> _links;
    HashSet<Guid> _binaries;
};
} // namespace

AssetScanner::AssetScanner(AssetIndex &index, List<String> folders) : _index(index), _folders(Move(folders))
{
    for (const String &path : _folders)
    {
        _state.AddDefaulted().Path = path;
    }
}

bool AssetScanner::ParseAsset(StringView path, StringView text, AssetFileRecord &record, StringWriter &errors)
{
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    if (!document.Parse(text, diagnostics))
    {
        errors.Append(diagnostics.ToString(path).AsView());
        return false;
    }
    record.IsAsset = true;
    LinkCollector links;
    for (usize i = 0; i < document.GetObjectCount(); ++i)
    {
        const MarlinTextObject *source = document.GetObject(i, diagnostics);
        if (source == nullptr)
        {
            continue;
        }
        AssetObjectRecord &object = record.Objects.AddDefaulted();
        object.Uuid = source->GetUuid();
        object.Name = source->GetName();
        object.Type = source->GetTypeName();
        object.Line = source->GetLine();
        object.Canonical = AssetCanonical::Hash(*source);
        links.Begin(object);
        if (const MarlinTextValue *parent = source->GetParent())
        {
            links.Collect(*parent);
        }
        for (const MarlinTextField &field : source->GetFields())
        {
            if (field.IsDirective)
            {
                continue;
            }
            links.Collect(field.Value);
            if (field.Key == "Tags" && field.Value.Kind == MarlinTextValueKind::Array)
            {
                for (const MarlinTextValue &tag : field.Value.Elements)
                {
                    if (tag.Kind == MarlinTextValueKind::Identifier || tag.Kind == MarlinTextValueKind::String)
                    {
                        object.Tags.Add(String(tag.Text));
                    }
                }
            }
        }
        if (!object.Uuid.IsValid())
        {
            errors.Append(Format("{0}({1}): {2} has no !UUID\n", path, object.Line, object.Type));
        }
    }
    if (diagnostics.HasErrors())
    {
        errors.Append(diagnostics.ToString(path).AsView());
    }
    return true;
}

bool AssetScanner::HashFile(StringView path, Hash128 &hash)
{
    const FileHandle file = Vfs::Open(path, FileAccess::Read);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    bool ok = PlatformFile::GetSize(file, size);
    if (ok && size == 0)
    {
        hash = Xxh3Hash128(static_cast<const uint8 *>(nullptr), 0);
    }
    else if (ok)
    {
        MappedRegion region;
        ok = PlatformFile::Map(file, FileAccess::Read, 0, static_cast<usize>(size), region);
        if (ok)
        {
            hash = Xxh3Hash128(region.Data, region.Size);
            PlatformFile::Unmap(region);
        }
    }
    PlatformFile::Close(file);
    return ok;
}

AssetScanner::FileStamp AssetScanner::StampOf(MountId layer, StringView path)
{
    FileStamp stamp;
    FileInfo info;
    if (Vfs::GetInfo(layer, path, info) && !info.IsDirectory)
    {
        stamp.Exists = true;
        stamp.Size = info.Size;
        stamp.ModifiedTime = info.ModifiedTime;
    }
    return stamp;
}

AssetScanner::Folder *AssetScanner::FolderOf(StringView path, StringView &relative)
{
    Folder *best = nullptr;
    for (Folder &folder : _state)
    {
        const StringView root = folder.Path.AsView();
        if ((path == root || (path.StartsWith(root) && path.Length() > root.Length() && path[root.Length()] == '/')) &&
            (best == nullptr || root.Length() > best->Path.Length()))
        {
            best = &folder;
        }
    }
    if (best != nullptr)
    {
        relative = path.Length() > best->Path.Length() ? path.Substring(best->Path.Length() + 1) : StringView();
    }
    return best;
}

AssetScanner::FolderLayer *AssetScanner::FindLayer(Folder &folder, MountId layer)
{
    for (FolderLayer &candidate : folder.Layers)
    {
        if (candidate.Layer == layer)
        {
            return &candidate;
        }
    }
    FolderLayer &added = folder.Layers.AddDefaulted();
    added.Layer = layer;
    return &added;
}

void AssetScanner::LoadFolder(Folder &folder)
{
    const String filesPath = Join(folder.Path, FilesName);
    Vfs::GetLayers(folder.Path.AsView(), folder.LayerIds);
    for (usize i = folder.Layers.Count(); i-- > 0;)
    {
        if (!folder.LayerIds.Contains(folder.Layers[i].Layer))
        {
            folder.Layers.RemoveAt(i);
        }
    }
    for (const MountId id : folder.LayerIds)
    {
        FolderLayer &layer = *FindLayer(folder, id);
        const FileStamp stamp = StampOf(id, filesPath.AsView());
        if (folder.Loaded && stamp == layer.Stamp)
        {
            continue;
        }
        layer.Stamp = stamp;
        layer.ByPath.Clear();
        layer.Entries.Clear();
        layer.ByHash.Clear();
        layer.Dirty = false;
        String text;
        MarlinTextDocument document;
        MarlinTextDiagnostics diagnostics;
        if (!stamp.Exists || !Vfs::ReadAll(id, filesPath.AsView(), text) || !document.Parse(text.AsView(), diagnostics))
        {
            continue;
        }
        for (usize i = 0; i < document.GetObjectCount(); ++i)
        {
            const MarlinTextObject *object = document.GetObject(i, diagnostics);
            BinaryFileEntry entry;
            if (object == nullptr || !object->GetUuid().IsValid() ||
                !MarlinTextSerializer::ReadObject(*object, TypeOf<BinaryFileEntry>(), &entry, diagnostics))
            {
                continue;
            }
            layer.ByPath.FindOrAdd(entry.Path) = object->GetUuid();
            layer.ByHash.FindOrAdd(entry.Hash).Add(object->GetUuid());
            layer.Entries.FindOrAdd(object->GetUuid()) = Move(entry);
        }
    }
    folder.Loaded = true;
}

void AssetScanner::SaveFolder(Folder &folder)
{
    const String filesPath = Join(folder.Path, FilesName);
    for (FolderLayer &layer : folder.Layers)
    {
        if (!layer.Dirty)
        {
            continue;
        }
        struct Row
        {
            StringView Path;
            const Guid *Uuid;
            const BinaryFileEntry *Entry;
        };
        List<Row> rows;
        for (const auto &entry : layer.Entries)
        {
            rows.Add({entry.Value.Path.AsView(), &entry.Key, &entry.Value});
        }
        rows.Sort([](const Row &a, const Row &b) { return a.Path < b.Path; });
        StringBuilder<16384> text("!Version \"1.0\"\n");
        for (const Row &row : rows)
        {
            text.Append('\n');
            MarlinTextSerializer::WriteObject(text, TypeOf<BinaryFileEntry>(), row.Entry, *row.Uuid);
        }
        // The .mfiles goes into the layer that holds the files it names, never into whichever layer is writable.
        if (Vfs::WriteAll(layer.Layer, filesPath.AsView(), text.AsView()))
        {
            layer.Dirty = false;
            // Recorded so the change event of this very write does not read the file back.
            layer.Stamp = StampOf(layer.Layer, filesPath.AsView());
        }
    }
}

void AssetScanner::BeginBatch()
{
    for (Folder &folder : _state)
    {
        for (FolderLayer &layer : folder.Layers)
        {
            layer.Seen.Clear();
        }
    }
}

void AssetScanner::EndBatch()
{
    for (Folder &folder : _state)
    {
        SaveFolder(folder);
    }
    (void)_index.Commit();
}

void AssetScanner::Walk(StringView directory, List<ScanItem> &files) const
{
    List<String> directories;
    Vfs::Enumerate(directory, [&](StringView name, const FileInfo &info) {
        if (IsIgnored(name))
        {
            return;
        }
        if (info.IsDirectory)
        {
            directories.Add(Join(directory, name));
            return;
        }
        // The listing's size and time are kept, so files need not be looked up again.
        ScanItem &file = files.AddDefaulted();
        file.Path = Join(directory, name);
        file.Info = info;
    });
    for (const String &child : directories)
    {
        Walk(child.AsView(), files);
    }
}

void AssetScanner::NeedsScan(ScanItem &item)
{
    Folder &folder = *item.Owner;
    const StringView relative = item.Path.AsView().Substring(folder.Path.Length() + 1);
    item.Asset = IsAssetFile(relative);
    item.Changed = false;
    if (!item.Asset)
    {
        // The layer the file is read from, whose .mfiles holds its Guid; with one layer there is nothing to look up.
        item.Layer = folder.LayerIds.Count() == 1 ? folder.LayerIds[0] : Vfs::FindLayer(item.Path.AsView());
        if (item.Layer == 0)
        {
            return;
        }
    }
    FolderLayer *layer = item.Asset ? nullptr : FindLayer(folder, item.Layer);
    const Guid *listed = layer != nullptr ? layer->ByPath.Find(relative) : nullptr;
    item.Listed = listed != nullptr;
    uint64 size = 0;
    int64 modified = 0;
    Hash128 indexed;
    const bool known = _index.GetFileStamp(item.Path.AsView(), size, modified, indexed);
    if (known && size == item.Info.Size && modified == item.Info.ModifiedTime && (item.Asset || item.Listed))
    {
        if (listed != nullptr)
        {
            layer->Seen.Add(*listed);
        }
        return;
    }
    item.Changed = true;
    item.Known = known && (item.Asset || item.Listed);
    item.IndexedHash = indexed;
}

void AssetScanner::Process(ScanItem &item)
{
    if (!item.Asset)
    {
        item.Read = HashFile(item.Path.AsView(), item.Hash);
        return;
    }
    // One read gives both the content hash and the text.
    List<uint8> bytes;
    if (!Vfs::ReadAll(item.Path.AsView(), bytes))
    {
        item.Errors = "it cannot be read";
        return;
    }
    item.Read = true;
    item.Hash = Xxh3Hash128(bytes.Data(), bytes.Count());
    const usize start = bytes.Count() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF ? 3 : 0;
    const StringView text(reinterpret_cast<const char *>(bytes.Data()) + start, bytes.Count() - start);
    StringBuilder<1024> errors;
    item.Parsed = ParseAsset(item.Path.AsView(), text, item.Record, errors);
    item.Errors = errors.AsView();
}

void AssetScanner::Apply(ScanItem &item, List<String> &changed, const HashSet<StringView> *present)
{
    if (item.Asset && !item.Parsed)
    {
        MARLIN_LOG(LogAssetIndex, Warning, "{0} is not indexed: {1}", item.Path, item.Errors);
        if (_index.Remove(item.Path.AsView()))
        {
            changed.Add(item.Path);
        }
        return;
    }
    if (!item.Read)
    {
        return;
    }
    if (!item.Errors.IsEmpty())
    {
        MARLIN_LOG(LogAssetIndex, Warning, "{0}", item.Errors);
    }
    AssetFileRecord &record = item.Record;
    record.Path = item.Path;
    record.ContentHash = item.Hash;
    record.Size = item.Info.Size;
    record.ModifiedTime = item.Info.ModifiedTime;
    if (!item.Asset)
    {
        const StringView relative = item.Path.AsView().Substring(item.Owner->Path.Length() + 1);
        record.BinaryGuid = AssignBinary(*item.Owner, item.Layer, relative, item.Hash, item.Info.Size, present);
    }
    // Touched, checked out again or saved unchanged: the same text gives the same record, so only the stamp is new
    // and nothing that read it is stale.
    if (!(item.Known && item.IndexedHash == item.Hash))
    {
        changed.Add(item.Path);
    }
    _index.Update(Move(record));
}

void AssetScanner::ProcessItems(List<ScanItem> &items, List<String> &changed, const HashSet<StringView> *present)
{
    uint64 found = 0;
    for (const ScanItem &item : items)
    {
        found += item.Changed ? 1 : 0;
    }
    (void)_filesFound.FetchAdd(found, MemoryOrder::Relaxed);
    Jobs::ParallelFor(static_cast<uint32>(items.Count()), 16, [&](uint32 begin, uint32 end) {
        uint64 read = 0;
        for (uint32 i = begin; i < end; ++i)
        {
            if (items[i].Changed)
            {
                Process(items[i]);
                ++read;
            }
        }
        (void)_filesRead.FetchAdd(read, MemoryOrder::Relaxed);
    });
    for (ScanItem &item : items)
    {
        if (item.Changed && (item.Asset || item.Listed))
        {
            Apply(item, changed, present);
        }
    }
    for (ScanItem &item : items)
    {
        if (item.Changed && !item.Asset && !item.Listed)
        {
            Apply(item, changed, present);
        }
    }
}

Guid AssetScanner::AssignBinary(Folder &folder, MountId layer, StringView relative, const Hash128 &hash, uint64 size,
                                const HashSet<StringView> *present)
{
    FolderLayer &files = *FindLayer(folder, layer);
    const String hashText = FormatHash(hash);
    if (const Guid *existing = files.ByPath.Find(relative))
    {
        const Guid guid = *existing;
        BinaryFileEntry &entry = *files.Entries.Find(guid);
        if (entry.Hash != hashText || entry.Size != size)
        {
            if (entry.Hash != hashText)
            {
                files.ByHash.FindOrAdd(hashText).Add(guid);
            }
            entry.Hash = hashText;
            entry.Size = size;
            files.Dirty = true;
        }
        files.Seen.Add(guid);
        return guid;
    }
    // RECOVERY-DECISION AP-D1: a file that overrides one of a lower layer, such as a platform layer's version of a
    // texture, takes that file's Guid, so links to it keep resolving whichever layer wins.
    Guid guid;
    for (const FolderLayer &other : folder.Layers)
    {
        const Guid *shadowed = &other != &files ? other.ByPath.Find(relative) : nullptr;
        if (shadowed != nullptr && !guid.IsValid())
        {
            guid = *shadowed;
        }
    }
    // A rename: an entry with the same contents whose file is gone from this layer. Only entries with that hash are
    // looked at, and those this scan already found in place are passed over without asking the file system.
    if (!guid.IsValid())
    {
        if (const List<Guid> *candidates = files.ByHash.Find(hashText.AsView()))
        {
            StringBuilder<512> path;
            for (const Guid &candidate : *candidates)
            {
                BinaryFileEntry *entry = files.Entries.Find(candidate);
                if (entry == nullptr || entry->Hash != hashText || files.Seen.Contains(candidate))
                {
                    continue;
                }
                path.Clear();
                path.Append(folder.Path.AsView()).Append('/').Append(entry->Path.AsView());
                FileInfo info;
                const bool gone = (present != nullptr && !present->Contains(path.AsView())) ||
                                  !Vfs::GetInfo(layer, path.AsView(), info);
                if (gone)
                {
                    files.ByPath.Remove(entry->Path);
                    entry->Path = relative;
                    files.ByPath.FindOrAdd(entry->Path) = candidate;
                    files.Seen.Add(candidate);
                    files.Dirty = true;
                    return candidate;
                }
            }
        }
    }
    if (!guid.IsValid())
    {
        guid = Guid::NewGuid();
    }
    BinaryFileEntry &entry = files.Entries.FindOrAdd(guid);
    entry.Path = relative;
    entry.Hash = hashText;
    entry.Size = size;
    files.ByPath.FindOrAdd(entry.Path) = guid;
    files.ByHash.FindOrAdd(hashText).Add(guid);
    files.Seen.Add(guid);
    files.Dirty = true;
    return guid;
}

void AssetScanner::RemoveGone(StringView path, Folder *folder, const HashSet<StringView> *present,
                              List<String> &changed)
{
    List<String> gone;
    _index.ForEachFileBelow(path, [&](StringView indexed) {
        StringView relative;
        if ((folder == nullptr || FolderOf(indexed, relative) == folder) &&
            (present == nullptr || !present->Contains(indexed)))
        {
            gone.Add(String(indexed));
        }
    });
    for (String &file : gone)
    {
        if (_index.Remove(file.AsView()))
        {
            changed.Add(Move(file));
        }
    }
}

void AssetScanner::ScanFolder(Folder &folder, List<String> &changed)
{
    LoadFolder(folder);
    List<ScanItem> files;
    Walk(folder.Path.AsView(), files);
    // Views into the paths of *files*, which stay where they are from here on.
    HashSet<StringView> present;
    for (ScanItem &file : files)
    {
        present.Add(file.Path.AsView());
        file.Owner = &folder;
        NeedsScan(file);
    }
    ProcessItems(files, changed, &present);
    RemoveGone(folder.Path.AsView(), &folder, &present, changed);
    // Entries whose file is gone from their own layer; a file only shadowed by a higher layer keeps its entry.
    StringBuilder<512> path;
    for (FolderLayer &layer : folder.Layers)
    {
        List<Guid> stale;
        for (const auto &entry : layer.Entries)
        {
            if (layer.Seen.Contains(entry.Key))
            {
                continue;
            }
            path.Clear();
            path.Append(folder.Path.AsView()).Append('/').Append(entry.Value.Path.AsView());
            FileInfo info;
            if (!present.Contains(path.AsView()) || !Vfs::GetInfo(layer.Layer, path.AsView(), info))
            {
                stale.Add(entry.Key);
            }
        }
        for (const Guid &guid : stale)
        {
            layer.ByPath.Remove(layer.Entries.Find(guid)->Path);
            layer.Entries.Remove(guid);
            layer.Dirty = true;
        }
    }
}

void AssetScanner::ScanBelow(Folder &folder, StringView path, List<ScanItem> &items, List<String> &changed)
{
    FileInfo info;
    if (!Vfs::GetInfo(path, info))
    {
        RemoveGone(path, nullptr, nullptr, changed);
        return;
    }
    if (!info.IsDirectory)
    {
        ScanItem item;
        item.Owner = &folder;
        item.Path = path;
        item.Info = info;
        NeedsScan(item);
        if (item.Changed)
        {
            items.Add(Move(item));
        }
        return;
    }
    List<ScanItem> files;
    Walk(path, files);
    HashSet<StringView> present;
    for (const ScanItem &file : files)
    {
        present.Add(file.Path.AsView());
    }
    RemoveGone(path, nullptr, &present, changed);
    for (ScanItem &file : files)
    {
        file.Owner = &folder;
        NeedsScan(file);
        if (file.Changed)
        {
            items.Add(Move(file));
        }
    }
}

void AssetScanner::ScanAll(List<String> &changed)
{
    BeginBatch();
    for (Folder &folder : _state)
    {
        ScanFolder(folder, changed);
    }
    EndBatch();
}

void AssetScanner::ScanPath(StringView path, List<String> &changed)
{
    const String paths[] = {String(path)};
    ScanPaths(paths, changed);
}

void AssetScanner::ScanPaths(Span<const String> paths, List<String> &changed)
{
    BeginBatch();
    List<ScanItem> items;
    List<Folder *> loaded;
    for (const String &entry : paths)
    {
        const StringView path = entry.AsView();
        StringView relative;
        Folder *folder = FolderOf(path, relative);
        if (folder == nullptr)
        {
            continue;
        }
        StringView parent;
        StringView name;
        if (!path.SplitLast('/', parent, name))
        {
            name = path;
        }
        if (relative.IsEmpty())
        {
            ScanFolder(*folder, changed);
            continue;
        }
        // Loading compares stamps, once per folder and batch, so an event for a .mfiles the scanner wrote itself
        // reads nothing.
        if (!loaded.Contains(folder))
        {
            LoadFolder(*folder);
            loaded.Add(folder);
        }
        if (name == FilesName || IsIgnored(name))
        {
            continue;
        }
        ScanBelow(*folder, path, items, changed);
    }
    ProcessItems(items, changed, nullptr);
    EndBatch();
}
} // namespace Marlin

namespace Marlin
{
List<String> GetProjectContentFolders()
{
    List<String> subfolders;
    if (!Config::TryGet("Engine.AssetIndex.Folders", subfolders) || subfolders.IsEmpty())
    {
        subfolders.Add("Content");
    }
    List<String> roots;
    roots.Add("/Engine");
    roots.Add("/Project");
    List<String> folders;
    for (const String &root : roots)
    {
        for (const String &subfolder : subfolders)
        {
            folders.Add(Format("{0}/{1}", root, subfolder));
        }
    }
    return folders;
}

bool LoadProjectIndex(AssetIndex &index)
{
    const String directory = Format("{0}/Intermediate/AssetIndex", Engine::GetEnvironment().ProjectRoot);
    index.SetSnapshotDirectory(directory.AsView());
    const bool loaded = index.LoadLatest();
    // Read only: the snapshots there are the indexer's, which this process must neither write nor delete.
    index.SetSnapshotDirectory({});
    if (loaded)
    {
        (void)index.ReplayLog(Format("{0}/Index.log", directory).AsView());
    }
    return loaded;
}

List<String> GetPhysicalFolders(StringView folder)
{
    List<String> folders;
    List<MountId> layers;
    Vfs::GetLayers(folder, layers);
    for (const MountId layer : layers)
    {
        FileInfo info;
        StringBuilder<1024> physical;
        if (Vfs::GetInfo(layer, folder, info) && info.IsDirectory && Vfs::GetPhysicalPath(layer, folder, physical) &&
            !physical.IsEmpty())
        {
            folders.Add(physical.ToString());
        }
    }
    StringBuilder<1024> lowest;
    if (folders.IsEmpty() && !layers.IsEmpty() && Vfs::GetPhysicalPath(layers.Last(), folder, lowest) &&
        !lowest.IsEmpty() && PlatformFile::CreateDirectory(lowest.CStr()))
    {
        folders.Add(lowest.ToString());
    }
    return folders;
}
} // namespace Marlin
