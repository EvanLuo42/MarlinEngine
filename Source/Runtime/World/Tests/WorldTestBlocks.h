#pragma once

#include "Asset/AssetBlock.h"
#include "Asset/Assets.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformFile.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "Testing/Testing.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
/// Serves blocks written to files under /Temp, as a package catalog would serve them from a package.
class WorldTestBlockStore final : public IAssetStore
{
public:
    explicit WorldTestBlockStore(HashMap<Guid, String> files) : _files(Move(files))
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        const String *path = _files.Find(guid);
        AssetLocation location;
        done(path != nullptr && Vfs::OpenRegion(path->AsView(), location.Block) ? &location : nullptr);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "World test blocks";
    }

private:
    HashMap<Guid, String> _files;
};

/// Converted assets in /Temp/<directory>, which it creates empty and deletes again, served to the asset table it
/// starts and shuts down.
class WorldTestBlocks
{
public:
    explicit WorldTestBlocks(StringView directory) : _virtual(Format("/Temp/{0}", directory))
    {
        Assets::Initialize();
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath(_virtual.AsView(), physical));
        // Only ever a folder of the test's own name under /Temp.
        MARLIN_REQUIRE(!directory.IsEmpty() && physical.AsView().EndsWith(directory));
        _physical = physical.AsView();
        (void)PlatformFile::DeleteDirectory(_physical.CStr(), true);
        MARLIN_REQUIRE(PlatformFile::CreateDirectory(_physical.CStr()));
    }

    ~WorldTestBlocks()
    {
        Assets::RemoveStore(_store);
        Assets::Shutdown();
        (void)PlatformFile::DeleteDirectory(_physical.CStr(), true);
    }

    MARLIN_NONCOPYABLE(WorldTestBlocks);

    [[nodiscard]] static Guid MakeGuid(uint32 value)
    {
        return {0x3E57B10Cu, 0, 0, value};
    }

    /// Converts *asset* into the block of *guid*.
    void Write(const Asset &asset, const Guid &guid)
    {
        List<uint8> block;
        MARLIN_REQUIRE(AssetBlock::Write(asset, guid, block));
        StringBuilder<256> path;
        FormatTo(path, "{0}/", _virtual);
        guid.FormatTo(path, "N");
        path.Append(".mablk");
        MARLIN_REQUIRE(
            Vfs::WriteAll(path.AsView(), StringView(reinterpret_cast<const char *>(block.Data()), block.Count())));
        _files.Set(guid, String(path.AsView()));
    }

    /// Starts serving the blocks written so far.
    void Serve()
    {
        _store = Assets::AddStore(MakeUnique<WorldTestBlockStore>(_files), 0);
    }

private:
    String _virtual;
    String _physical;
    HashMap<Guid, String> _files;
    AssetStoreId _store = 0;
};
} // namespace Marlin::Tests
