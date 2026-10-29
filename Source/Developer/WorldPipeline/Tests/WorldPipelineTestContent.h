#pragma once

#include "Asset/Assets.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetScanner.h"
#include "AssetPipeline/ConversionStore.h"
#include "Concurrency/SpinLock.h"
#include "HAL/PlatformFile.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Testing/Testing.h"
#include "Vfs/Vfs.h"

namespace Marlin::Tests
{
/// Serves assets by converting them when they are asked for, as the indexer does for the editor: a level's cells come
/// from the conversion of the level. Conversions run one at a time, on whichever thread asks, and each result is
/// written out of GlobalStore into a file below *folder* to be read from.
class ConvertingStore final : public IAssetStore
{
public:
    ConvertingStore(AssetConversion &conversion, ConversionStore &store, StringView folder)
        : _conversion(&conversion), _store(&store), _folder(folder)
    {
    }

    void Locate(const Guid &guid, AssetLocateCallback done) override
    {
        ConversionResult result;
        String path;
        bool exported = false;
        {
            const ScopeLock<SpinLock> lock(_lock);
            ConversionTarget target;
            target.Platform = "Windows";
            result = _conversion->Convert(guid, target);
            path = Format("{0}/{1}.bin", _folder, result.Location.Key);
            exported = result.Succeeded && _store->ExportResult(result.Key, path.AsView());
        }
        FileRegion file;
        if (!exported || !Vfs::OpenRegion(path.AsView(), file))
        {
            done(nullptr);
            return;
        }
        AssetLocation location;
        location.Block = file;
        location.Block.Offset = file.Offset + result.Location.BlockOffset;
        location.Block.Size = result.Location.BlockSize;
        location.Bulk = file;
        location.Bulk.Offset = file.Offset + result.Location.BulkOffset;
        location.Bulk.Size = result.Location.BulkSize;
        done(&location);
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "World pipeline test content";
    }

private:
    AssetConversion *_conversion;
    ConversionStore *_store;
    String _folder;
    SpinLock _lock;
};

/// Source content in /Temp/<name>, mounted as /<name>: sources go in /<name>/Content (and the *extra* folders, which
/// are scanned too), the DDC in /<name>/Cache, and the asset table it starts serves them converted on demand. The
/// folder is created and deleted through the VFS, inside the /Temp mount, and only ever the fixture's own.
class ContentFixture
{
public:
    explicit ContentFixture(StringView name, Span<const StringView> extra = {})
        : _root(Format("/{0}", name)), _temp(Format("/Temp/{0}", name))
    {
        MARLIN_REQUIRE(!name.IsEmpty() && !name.Contains('/'));
        (void)Vfs::Delete(_temp.AsView());
        MARLIN_REQUIRE(Vfs::CreateDirectory(_temp.AsView()));
        MARLIN_REQUIRE(Vfs::CreateDirectory(Format("{0}/Content", _temp).AsView()));
        StringBuilder<512> physical;
        MARLIN_REQUIRE(Vfs::GetPhysicalPath(_temp.AsView(), physical) && !physical.IsEmpty());
        _physical = physical.AsView();
        _mount = Vfs::Mount(_root.AsView(), MakeUnique<DirectoryMount>(_physical.AsView(), true), 0);
        List<String> folders;
        folders.Add(Format("{0}/Content", _root));
        for (const StringView folder : extra)
        {
            folders.Add(String(folder));
        }
        _scanner = MakeUnique<AssetScanner>(_index, Move(folders));
        _resolver = MakeUnique<IndexSourceResolver>(_index);
        _globalStore =
            MakeUnique<ConversionStore>(ConversionStoreDesc::InProcess(Format("{0}/GlobalStore", _root).AsView()));
        MARLIN_REQUIRE(Vfs::CreateDirectory(Format("{0}/Results", _root).AsView()));
        _conversion = MakeUnique<AssetConversion>(*_resolver, *_globalStore);
        Assets::Initialize();
        _store = Assets::AddStore(
            MakeUnique<ConvertingStore>(*_conversion, *_globalStore, Format("{0}/Results", _root).AsView()), 0);
    }

    ~ContentFixture()
    {
        Assets::RemoveStore(_store);
        Assets::Shutdown();
        _conversion.Reset();
        _globalStore.Reset();
        _resolver.Reset();
        _scanner.Reset();
        if (_mount != 0)
        {
            (void)Vfs::Unmount(_mount);
        }
        // Only the fixture's own folder: "/Temp/" and a name without separators.
        const StringView temp = _temp.AsView();
        if (temp.StartsWith("/Temp/") && temp.Length() > 6 && !temp.Substring(6).Contains('/'))
        {
            (void)Vfs::Delete(temp);
        }
    }

    MARLIN_NONCOPYABLE(ContentFixture);

    /// Writes the source file *file* into the content folder; Scan indexes it.
    void Write(StringView file, StringView text)
    {
        MARLIN_REQUIRE(Vfs::WriteAll(Format("{0}/Content/{1}", _root, file).AsView(), text));
    }

    /// Converts *uuid* as the store would. Call it before anything loads: the store may convert at the same time.
    ConversionResult Convert(const Guid &uuid)
    {
        ConversionTarget target;
        target.Platform = "Windows";
        return _conversion->Convert(uuid, target);
    }

    /// Indexes what was written since the last scan.
    void Scan()
    {
        List<String> changed;
        _scanner->ScanAll(changed);
    }

private:
    String _root;
    String _temp;
    String _physical;
    MountId _mount = 0;
    AssetIndex _index;
    UniquePtr<AssetScanner> _scanner;
    UniquePtr<IndexSourceResolver> _resolver;
    UniquePtr<ConversionStore> _globalStore;
    UniquePtr<AssetConversion> _conversion;
    AssetStoreId _store = 0;
};
} // namespace Marlin::Tests
