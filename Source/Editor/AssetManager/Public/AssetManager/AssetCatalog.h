#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "EditorCore/EditorSubsystem.h"
#include "EditorUI/Signal.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "AssetManager/AssetCatalog.gen.h"

namespace Marlin
{
class AssetIndexClient;

/// How the Asset Manager shows assets of a source type (Config `Editor.AssetTypes`).
MSTRUCT()
struct AssetTypeStyle
{
    MGENERATED_BODY()

    /// The source type name, such as TextureSource.
    MPROPERTY()
    String Type;
    /// What the user sees, such as Texture.
    MPROPERTY()
    String Label;
    /// Theme icon and color tokens.
    MPROPERTY()
    String Icon;
    MPROPERTY()
    String Color;
};

/// What the Asset Manager keeps per project, in `/User/Editor/AssetManager.mtext`.
MSTRUCT()
struct AssetManagerUserState
{
    MGENERATED_BODY()

    MPROPERTY()
    List<Guid> Starred;
};

/// An object of a .masset file, as the Asset Manager lists it.
struct CatalogAsset
{
    Guid Uuid;
    String Name;
    String Type;
    /// The .masset holding it, and the virtual directory that holds that, such as /Project/Content/Rocks.
    String File;
    String Folder;
    /// Of the .masset.
    uint64 Size = 0;
    /// DateTime ticks.
    int64 Modified = 0;
    List<String> Tags;
};

/// A directory of the content folders.
struct CatalogFolder
{
    /// The virtual directory, such as /Project/Content/Rocks.
    String Path;
    String Name;
    /// Indices into the folder list; -1 for a root.
    int32 Parent = -1;
    List<int32> Children;
    uint32 Depth = 0;
    /// Assets in it and below it.
    uint32 Count = 0;
};

/// The assets of the project and the engine as the asset index has them (Documentation/Architecture/06 §2, 09 §5),
/// for the Asset Manager: it connects to the project's MarlinAssetIndexer, reads its shared mapping and reads it again
/// whenever the index changes. Also keeps the user's starred assets, in `/User/Editor/AssetManager.mtext`.
MCLASS(Config = "Editor.AssetTypes")
class ASSETMANAGER_API AssetCatalogSubsystem final : public EditorSubsystem
{
    MGENERATED_BODY()
public:
    /// How each source type shows; types not listed show their name without "Source", a file icon and a gray.
    MPROPERTY()
    static List<AssetTypeStyle> Styles;

    AssetCatalogSubsystem();
    ~AssetCatalogSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// Changes whenever the assets or folders are read again.
    [[nodiscard]] uint32 GetVersion() const
    {
        return _version.Get();
    }

    /// The assets, by name within each folder; follows changes.
    [[nodiscard]] const List<CatalogAsset> &GetAssets() const;
    /// The content folders: each root (the project's, then the engine's) followed by the folders below it, depth
    /// first by name; follows changes.
    [[nodiscard]] const List<CatalogFolder> &GetFolders() const;
    /// Null if *uuid* is no asset; follows changes.
    [[nodiscard]] const CatalogAsset *Find(const Guid &uuid) const;
    /// The index of the folder at *path*, or -1.
    [[nodiscard]] int32 FindFolder(StringView path) const;

    /// The objects linking to *uuid*.
    void GetReferencers(const Guid &uuid, List<CatalogAsset> &results) const;

    /// Whether the indexer answered.
    [[nodiscard]] bool IsConnected() const;
    /// Asks the indexer to scan the content folders again.
    void Rescan();

    [[nodiscard]] bool IsStarred(const Guid &uuid) const;
    void SetStarred(const Guid &uuid, bool starred);
    [[nodiscard]] uint32 GetStarredCount() const;

    [[nodiscard]] static const AssetTypeStyle &GetStyle(StringView type);

private:
    void Read();
    void LoadStarred();
    void SaveStarred() const;

    UniquePtr<AssetIndexClient> _client;
    ReactiveOwner *_owner = nullptr;
    Signal<uint32> _version;
    Signal<uint32> _starredVersion;
    List<CatalogAsset> _assets;
    List<CatalogFolder> _folders;
    HashMap<Guid, usize> _byUuid;
    List<Guid> _starred;
    uint64 _generation = ~uint64{0};
    float64 _nextPoll = 0;
};
} // namespace Marlin
