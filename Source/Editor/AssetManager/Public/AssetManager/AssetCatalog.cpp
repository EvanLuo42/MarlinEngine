#include "AssetManager/AssetCatalog.h"

#include "AssetPipeline/AssetIndexClient.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Net/NetSubsystem.h"
#include "String/Ascii.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
List<AssetTypeStyle> AssetCatalogSubsystem::Styles;

namespace
{
/// The content folders and their names in the tree.
struct ContentRoot
{
    StringView Path;
    StringView Name;
};
constexpr ContentRoot Roots[] = {{"/Project/Content", "Content"}, {"/Engine/Content", "Engine"}};

/// How often the shared mapping is checked for a newer generation, in seconds.
constexpr float64 PollInterval = 0.5;

constexpr StringView StarredPath = "/User/Editor/AssetManager.mtext";

StringView DirectoryOf(StringView path)
{
    StringView directory;
    StringView name;
    return path.SplitLast('/', directory, name) ? directory : StringView();
}

StringView NameOf(StringView path)
{
    StringView directory;
    StringView name;
    return path.SplitLast('/', directory, name) ? name : path;
}

bool LessIgnoringCase(StringView a, StringView b)
{
    const usize count = Min(a.Length(), b.Length());
    for (usize index = 0; index < count; ++index)
    {
        const char x = Ascii::ToLower(a[index]);
        const char y = Ascii::ToLower(b[index]);
        if (x != y)
        {
            return x < y;
        }
    }
    return a.Length() < b.Length();
}
} // namespace

AssetCatalogSubsystem::AssetCatalogSubsystem() = default;
AssetCatalogSubsystem::~AssetCatalogSubsystem() = default;

void AssetCatalogSubsystem::Initialize(SubsystemContext & /*context*/)
{
    _owner = Reactive::CreateOwner(nullptr);
    Reactive::RunInOwner(_owner, [this] {
        _version = CreateSignal<uint32>(0);
        _starredVersion = CreateSignal<uint32>(0);
    });
    LoadStarred();
    const EngineEnvironment &environment = Engine::GetEnvironment();
    NetSubsystem *net = Engine::GetSubsystem<NetSubsystem>();
    if (net == nullptr || !environment.HasProject())
    {
        return;
    }
    _client = MakeUnique<AssetIndexClient>(net->GetLoop());
    if (!_client->Connect(environment.ProjectRoot.AsView(), environment.ProjectFile.AsView(), true))
    {
        MARLIN_LOG(LogCore, Warning, "The Asset Manager cannot reach the asset indexer; it shows no assets");
    }
}

void AssetCatalogSubsystem::Deinitialize()
{
    if (_client != nullptr)
    {
        _client->Disconnect();
    }
    _client.Reset();
    _assets.Clear();
    _folders.Clear();
    _byUuid.Clear();
    Reactive::DisposeOwner(_owner);
    _owner = nullptr;
}

void AssetCatalogSubsystem::BeginFrame(const FrameTime &time)
{
    if (_client == nullptr || time.Elapsed < _nextPoll)
    {
        return;
    }
    _nextPoll = time.Elapsed + PollInterval;
    // Reading maps a newer generation if there is one; only then is the catalog built again.
    if (_client->ReadView([](const AssetIndexView &) {}) && _client->GetViewGeneration() != _generation)
    {
        _generation = _client->GetViewGeneration();
        Read();
    }
}

void AssetCatalogSubsystem::Read()
{
    List<CatalogAsset> assets;
    List<CatalogFolder> folders;
    HashMap<String, int32> folderByPath;
    // The folder at *path*, made with the parents it lacks the first time; its root must be there.
    const auto folderOf = [&](StringView path) -> int32 {
        List<StringView> missing;
        const int32 *found = nullptr;
        for (StringView current = path; (found = folderByPath.Find(String(current))) == nullptr;
             current = DirectoryOf(current))
        {
            if (current.IsEmpty())
            {
                return -1;
            }
            missing.Add(current);
        }
        int32 parent = *found;
        for (usize index = missing.Count(); index-- > 0;)
        {
            CatalogFolder &folder = folders.AddDefaulted();
            folder.Path = String(missing[index]);
            folder.Name = String(NameOf(missing[index]));
            folder.Parent = parent;
            folder.Depth = folders[static_cast<usize>(parent)].Depth + 1;
            const auto added = static_cast<int32>(folders.Count() - 1);
            folders[static_cast<usize>(parent)].Children.Add(added);
            folderByPath.Set(folder.Path, added);
            parent = added;
        }
        return parent;
    };
    (void)_client->ReadView([&](const AssetIndexView &view) {
        for (const ContentRoot &root : Roots)
        {
            CatalogFolder &top = folders.AddDefaulted();
            top.Path = String(root.Path);
            top.Name = String(root.Name);
            folderByPath.Set(top.Path, static_cast<int32>(folders.Count() - 1));
            view.ForEachFileBelow(root.Path, [&](StringView path) {
                const int32 folder = folderOf(DirectoryOf(path));
                AssetFileRecord record;
                if (folder < 0 || !path.EndsWith(".masset") || !view.GetRecord(path, record))
                {
                    return;
                }
                for (AssetObjectRecord &object : record.Objects)
                {
                    CatalogAsset &asset = assets.AddDefaulted();
                    asset.Uuid = object.Uuid;
                    asset.Name = Move(object.Name);
                    asset.Type = Move(object.Type);
                    asset.File = String(path);
                    asset.Folder = folders[static_cast<usize>(folder)].Path;
                    asset.Size = record.Size;
                    asset.Modified = record.ModifiedTime;
                    asset.Tags = Move(object.Tags);
                }
            });
        }
    });
    // A folder counts the assets below it; folders and assets sort by name.
    for (const CatalogAsset &asset : assets)
    {
        for (const int32 *folder = folderByPath.Find(asset.Folder); folder != nullptr && *folder >= 0;)
        {
            CatalogFolder &entry = folders[static_cast<usize>(*folder)];
            ++entry.Count;
            folder = entry.Parent >= 0 ? &entry.Parent : nullptr;
        }
    }
    for (CatalogFolder &folder : folders)
    {
        folder.Children.Sort([&folders](int32 a, int32 b) {
            return LessIgnoringCase(folders[static_cast<usize>(a)].Name, folders[static_cast<usize>(b)].Name);
        });
    }
    // Depth first, so a folder's rows follow it in a tree.
    List<CatalogFolder> ordered;
    List<int32> remap;
    remap.Resize(folders.Count(), -1);
    const auto visit = [&](auto &self, int32 index, int32 parent) -> void {
        CatalogFolder folder = folders[static_cast<usize>(index)];
        folder.Parent = parent;
        const List<int32> children = Move(folder.Children);
        folder.Children.Clear();
        const auto placed = static_cast<int32>(ordered.Count());
        remap[static_cast<usize>(index)] = placed;
        ordered.Add(Move(folder));
        for (const int32 child : children)
        {
            ordered[static_cast<usize>(placed)].Children.Add(static_cast<int32>(ordered.Count()));
            self(self, child, placed);
        }
    };
    for (usize index = 0; index < folders.Count(); ++index)
    {
        if (folders[index].Parent < 0)
        {
            visit(visit, static_cast<int32>(index), -1);
        }
    }
    assets.Sort([](const CatalogAsset &a, const CatalogAsset &b) { return LessIgnoringCase(a.Name, b.Name); });

    _assets = Move(assets);
    _folders = Move(ordered);
    _byUuid.Clear();
    for (usize index = 0; index < _assets.Count(); ++index)
    {
        _byUuid.Set(_assets[index].Uuid, index);
    }
    _version.Set(_version.Peek() + 1);
}

const List<CatalogAsset> &AssetCatalogSubsystem::GetAssets() const
{
    (void)_version.Get();
    return _assets;
}

const List<CatalogFolder> &AssetCatalogSubsystem::GetFolders() const
{
    (void)_version.Get();
    return _folders;
}

const CatalogAsset *AssetCatalogSubsystem::Find(const Guid &uuid) const
{
    (void)_version.Get();
    const usize *index = _byUuid.Find(uuid);
    return index != nullptr ? &_assets[*index] : nullptr;
}

int32 AssetCatalogSubsystem::FindFolder(StringView path) const
{
    for (usize index = 0; index < _folders.Count(); ++index)
    {
        if (_folders[index].Path == path)
        {
            return static_cast<int32>(index);
        }
    }
    return -1;
}

void AssetCatalogSubsystem::GetReferencers(const Guid &uuid, List<CatalogAsset> &results) const
{
    results.Clear();
    if (_client == nullptr)
    {
        return;
    }
    List<AssetObjectInfo> referencers;
    if (!_client->GetReferencers(uuid, referencers))
    {
        return;
    }
    for (const AssetObjectInfo &info : referencers)
    {
        if (const usize *index = _byUuid.Find(info.Uuid))
        {
            results.Add(_assets[*index]);
        }
        else
        {
            CatalogAsset &asset = results.AddDefaulted();
            asset.Uuid = info.Uuid;
            asset.Name = info.Name;
            asset.Type = info.Type;
            asset.File = info.Path;
        }
    }
}

bool AssetCatalogSubsystem::IsConnected() const
{
    return _client != nullptr && _client->IsConnected();
}

void AssetCatalogSubsystem::Rescan()
{
    if (_client != nullptr)
    {
        (void)_client->Rescan();
    }
}

bool AssetCatalogSubsystem::IsStarred(const Guid &uuid) const
{
    (void)_starredVersion.Get();
    return _starred.Contains(uuid);
}

void AssetCatalogSubsystem::SetStarred(const Guid &uuid, bool starred)
{
    if (starred == _starred.Contains(uuid))
    {
        return;
    }
    if (starred)
    {
        _starred.Add(uuid);
    }
    else
    {
        (void)_starred.Remove(uuid);
    }
    _starredVersion.Set(_starredVersion.Peek() + 1);
    SaveStarred();
}

uint32 AssetCatalogSubsystem::GetStarredCount() const
{
    (void)_starredVersion.Get();
    return static_cast<uint32>(_starred.Count());
}

const AssetTypeStyle &AssetCatalogSubsystem::GetStyle(StringView type)
{
    for (const AssetTypeStyle &style : Styles)
    {
        if (style.Type == type)
        {
            return style;
        }
    }
    // Made once per unknown type, so the reference stays valid.
    static List<UniquePtr<AssetTypeStyle>> fallbacks;
    for (const UniquePtr<AssetTypeStyle> &fallback : fallbacks)
    {
        if (fallback->Type == type)
        {
            return *fallback;
        }
    }
    UniquePtr<AssetTypeStyle> fallback = MakeUnique<AssetTypeStyle>();
    fallback->Type = String(type);
    fallback->Label = String(type.EndsWith("Source") ? type.Left(type.Length() - 6) : type);
    fallback->Icon = "File";
    fallback->Color = "AssetType.Other";
    fallbacks.Add(Move(fallback));
    return *fallbacks.Last();
}

void AssetCatalogSubsystem::LoadStarred()
{
    String text;
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    const MarlinTextObject *object = nullptr;
    AssetManagerUserState state;
    if (Vfs::IsMounted("/User") && Vfs::ReadAll(StarredPath, text) && document.Parse(text.AsView(), diagnostics) &&
        document.GetObjectCount() == 1 && (object = document.GetObject(0, diagnostics)) != nullptr &&
        MarlinTextSerializer::ReadObject(*object, TypeOf<AssetManagerUserState>(), &state, diagnostics))
    {
        _starred = Move(state.Starred);
    }
}

void AssetCatalogSubsystem::SaveStarred() const
{
    if (!Vfs::IsMounted("/User"))
    {
        return;
    }
    AssetManagerUserState state;
    state.Starred = _starred;
    StringBuilder<1024> text;
    MarlinTextSerializer::WriteObject(text, TypeOf<AssetManagerUserState>(), &state);
    text.Append('\n');
    (void)Vfs::CreateDirectory("/User/Editor");
    if (!Vfs::WriteAll(StarredPath, text.AsView()))
    {
        MARLIN_LOG(LogCore, Warning, "The starred assets cannot be saved to {0}", StarredPath);
    }
}
} // namespace Marlin
