#include "Packaging/Packager.h"

#include "Asset/AssetBlock.h"
#include "AssetPipeline/AssetConverter.h"
#include "Compression/CompressionCodec.h"
#include "Concurrency/Atomic.h"
#include "Concurrency/Jobs.h"
#include "Config/Config.h"
#include "Containers/HashSet.h"
#include "HAL/PlatformCpu.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformThread.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "Memory/UniquePtr.h"
#include "Packaging/PakWriter.h"
#include "ScriptPipeline/ScriptModuleConversion.h"
#include "ScriptVM/ScriptModuleAsset.h"
#include "Shader/GlobalShader.h"
#include "ShaderPipeline/GlobalShaderConversion.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/PakFile.h"
#include "Vfs/PakSignature.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
struct Item
{
    Guid Uuid;
    String Path;
    String PackageClass;
    /// The entry point of a global shader; empty for an object.
    String Shader;
    /// The package of a script module; empty for an object.
    String Script;
    ConversionResult Result;
};

/// Converts Items[Next, End) on several threads; each thread has its own conversion, as converter workers do, and
/// all share the one ConversionStore.
struct Conversions
{
    const PackageDesc *Desc = nullptr;
    ConversionStore *Store = nullptr;
    List<Item> *Items = nullptr;
    Atomic<usize> Next{0};
    usize End = 0;

    /// Items a thread takes at once: the objects among them are looked up with one ConvertMany, two round trips for
    /// the lot, as converter workers do.
    static constexpr usize Batch = 16;

    void Run()
    {
        IndexSourceResolver resolver(*Desc->Index);
        AssetConversion conversion(resolver, *Store);
        // One build: config is read once, so each section's hash is computed once.
        conversion.AssumeFixedConfig();
        GlobalShaderConversion shaders(*Store);
        ScriptModuleConversion scripts(*Store);
        while (true)
        {
            const usize first = Next.FetchAdd(Batch, MemoryOrder::Relaxed);
            if (first >= End)
            {
                return;
            }
            const usize last = first + Batch < End ? first + Batch : End;
            List<Guid> uuids;
            List<usize> objects;
            for (usize index = first; index < last; ++index)
            {
                const Item &item = (*Items)[index];
                if (item.Script.IsEmpty() && item.Shader.IsEmpty())
                {
                    uuids.Add(item.Uuid);
                    objects.Add(index);
                }
            }
            List<ConversionResult> results;
            conversion.ConvertMany(uuids.AsSpan(), Desc->Target, results);
            for (usize k = 0; k < objects.Count(); ++k)
            {
                (*Items)[objects[k]].Result = Move(results[k]);
            }
            for (usize index = first; index < last; ++index)
            {
                ConvertOther((*Items)[index], shaders, scripts);
            }
        }
    }

    /// Converts *item* if it is a shader or a script module.
    void ConvertOther(Item &item, GlobalShaderConversion &shaders, ScriptModuleConversion &scripts)
    {
        if (!item.Script.IsEmpty())
        {
            ScriptModuleConversionResult script =
                scripts.Convert(item.Script.AsView(), Desc->Target, Desc->ScriptOptions);
            item.Result.Succeeded = script.Succeeded;
            item.Result.CacheHit = script.CacheHit;
            item.Result.Key = script.Key;
            item.Result.Location = script.Location;
            item.Result.Errors = Move(script.Errors);
            return;
        }
        if (item.Shader.IsEmpty())
        {
            return;
        }
        GlobalShaderConversionResult shader = shaders.Convert(item.Shader.AsView(), Desc->Target, Desc->ShaderOptions);
        item.Result.Succeeded = shader.Succeeded;
        item.Result.CacheHit = shader.CacheHit;
        item.Result.Key = shader.Key;
        item.Result.Location = shader.Location;
        item.Result.Errors = Move(shader.Errors);
    }
};

/// The text of the native file *path*; false if it cannot be read.
bool ReadNativeText(const String &path, String &text)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    bool ok = PlatformFile::GetSize(file, size) && size < 4096;
    if (ok)
    {
        text.Resize(static_cast<usize>(size));
        ok = PlatformFile::Read(file, text.Data(), text.Length(), read) && read == text.Length();
    }
    PlatformFile::Close(file);
    return ok;
}

bool WriteNativeText(const String &path, StringView text)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

String FileKey(StringView mountPoint, StringView layer, StringView path)
{
    return Format("{0}|{1}|{2}", mountPoint, layer, path);
}

void CollectFiles(const String &directory, const String &relative, List<String> &files)
{
    DirectoryIterator iterator;
    DirectoryEntry entry;
    const String native = relative.IsEmpty() ? directory : Format("{0}/{1}", directory, relative);
    if (!iterator.Open(native.CStr()))
    {
        return;
    }
    List<String> folders;
    while (iterator.Next(entry))
    {
        const String path = relative.IsEmpty() ? String(entry.Name) : Format("{0}/{1}", relative, entry.Name);
        if (entry.Info.IsDirectory)
        {
            folders.Add(path);
        }
        else
        {
            files.Add(path);
        }
    }
    for (const String &folder : folders)
    {
        CollectFiles(directory, folder, files);
    }
}

/// Whether *path* is an absolute native path below a root, such as C:/Builds/Paks or /home/builds/Paks: never empty,
/// relative or a root itself.
bool IsAbsoluteFolder(StringView path)
{
    const bool drive = path.Length() >= 3 && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
    const bool rooted = !path.IsEmpty() && (path[0] == '/' || path[0] == '\\');
    const usize rootLength = drive ? 3 : 1;
    return (drive || rooted) && path.Length() > rootLength;
}

/// What the release a patch builds on has of an object or file: the hash of its content, or that it was removed.
struct Released
{
    Hash128 ContentHash;
    bool Deleted = false;
    /// For files: where the entry lives, to write a deletion entry for it.
    String MountPoint;
    String Layer;
    String Path;
};

/// Writes `<name>.msig` beside the package at *path* (Documentation/Architecture/06 §5.3): *hashes*, the hash of each
/// block as stored, which PakWriter worked out while compressing, and *scheme*'s signature over them and the header and
/// tables, through a temporary file like the package.
bool SignPackage(const String &path, const PakSignatureScheme &scheme, Span<const uint8> privateKey, const Guid &keyId,
                 const List<uint8> &hashes, StringWriter &errors)
{
    List<uint8> signedBytes;
    bool ok = PakSigning::ReadSignedBytes(path.AsView(), signedBytes);
    PakSignatureHeader signature;
    const StringView name = scheme.GetName();
    MemCopy(signature.Scheme, name.Data(), name.Length());
    signature.KeyId = keyId;
    signature.BlockCount = hashes.Count() / PakBlockHashSize;
    uint8 digest[PlatformCrypto::Sha256Size];
    List<uint8> bytes;
    ok = ok && PakSigning::ComputeDigest(signedBytes.AsSpan(), signature, hashes.AsSpan(), digest) &&
         scheme.Sign(privateKey, digest, bytes) && bytes.Count() <= sizeof(signature.Signature);
    if (!ok)
    {
        errors.Append(Format("{0} cannot be signed with {1}\n", path, name));
        return false;
    }
    MemCopy(signature.Signature, bytes.Data(), bytes.Count());
    signature.SignatureSize = static_cast<uint32>(bytes.Count());

    const String target = PakSigning::SignaturePath(path.AsView());
    const String temporary = Format("{0}.tmp", target);
    const FileHandle output = PlatformFile::Open(temporary.CStr(), FileAccess::Write, FileMode::CreateAlways);
    ok = output.IsValid() && PlatformFile::Write(output, &signature, sizeof(signature)) &&
         PlatformFile::Write(output, hashes.Data(), hashes.Count());
    if (output.IsValid())
    {
        ok = PlatformFile::Flush(output) && ok;
        PlatformFile::Close(output);
    }
    if (!ok || !PlatformFile::Move(temporary.CStr(), target.CStr(), true))
    {
        if (output.IsValid())
        {
            (void)PlatformFile::Delete(temporary.CStr());
        }
        errors.Append(Format("{0} cannot be written\n", target));
        return false;
    }
    return true;
}
} // namespace

bool BuildPackages(const PackageDesc &desc, PackageResult &result, StringWriter &errors)
{
    result = {};
    // RECOVERY-DECISION SP-D2: the packager only writes package files (through a temporary file and a rename); it
    // never deletes or cleans a directory, so the output folder must be an absolute native path below a root.
    if (desc.Index == nullptr || !IsAbsoluteFolder(desc.OutputDirectory.AsView()))
    {
        errors.Append(Format("{0} cannot be written\n", desc.OutputDirectory));
        return false;
    }
    const AssetIndex &index = *desc.Index;
    HashSet<Guid> wanted;
    List<Guid> order;
    const auto want = [&](const Guid &uuid) {
        if (wanted.Add(uuid))
        {
            order.Add(uuid);
            List<Guid> objects;
            List<Guid> binaries;
            index.GetDependencies(uuid, objects, binaries);
            for (const Guid &dependency : objects)
            {
                if (wanted.Add(dependency))
                {
                    order.Add(dependency);
                }
            }
        }
    };
    for (const Guid &root : desc.Roots)
    {
        want(root);
    }
    for (const String &folder : desc.RootFolders)
    {
        AssetQuery query;
        query.PathPrefix = folder;
        query.Limit = ~0u;
        List<AssetObjectInfo> objects;
        index.Query(query, objects);
        for (const AssetObjectInfo &object : objects)
        {
            want(object.Uuid);
        }
    }

    // Results go into GlobalStore and the packages are read back from it; nothing is cached on this machine's disk.
    UniquePtr<ConversionStore> ownStore;
    ConversionStore *store = desc.Store;
    if (store == nullptr)
    {
        ownStore = MakeUnique<ConversionStore>();
        store = ownStore.Get();
    }
    if (store->GetClient() == nullptr)
    {
        errors.Append("no GlobalStore can be reached for the conversions\n");
        return false;
    }
    List<Item> items;
    {
        IndexSourceResolver resolver(index);
        const AssetConversion probe(resolver, *store);
        for (const Guid &uuid : order)
        {
            AssetObjectInfo info;
            const AssetConverter *converter =
                index.Find(uuid, info) ? probe.FindConverter(info.Type.AsView()) : nullptr;
            if (converter == nullptr)
            {
                continue;
            }
            Item &item = items.AddDefaulted();
            item.Uuid = uuid;
            item.Path = info.Path;
            item.PackageClass = converter->GetRuntimeType().GetMetadata().GetOr("PackageClass", "Package");
        }
    }
    if (desc.GlobalShaders)
    {
        List<String> entryPoints;
        String listErrors;
        if (!GetGlobalShaderEntryPoints(entryPoints, listErrors))
        {
            errors.Append(Format("the global shaders cannot be listed:\n{0}\n", listErrors));
            return false;
        }
        for (const String &entryPoint : entryPoints)
        {
            const Guid uuid = GetGlobalShaderGuid(entryPoint.AsView());
            if (wanted.Add(uuid))
            {
                Item &item = items.AddDefaulted();
                item.Uuid = uuid;
                item.PackageClass = "Package";
                item.Shader = entryPoint;
            }
        }
    }
    if (desc.ScriptModules)
    {
        List<String> packages;
        GetScriptPackages(packages);
        for (const String &package : packages)
        {
            const Guid uuid = GetScriptModuleGuid(package.AsView());
            if (wanted.Add(uuid))
            {
                Item &item = items.AddDefaulted();
                item.Uuid = uuid;
                item.PackageClass = "Package";
                item.Script = package;
            }
        }
    }

    // Conversions run in passes: the derived results a pass reports, such as the cells of a level, are converted by
    // the next one, until no new ones appear.
    Conversions conversions;
    conversions.Desc = &desc;
    conversions.Items = &items;
    conversions.Store = store;
    const uint32 logical = PlatformCpu::GetTopology().LogicalCount;
    const uint32 maxThreads = desc.Threads != 0 ? desc.Threads : (logical != 0 ? logical : 1);
    usize start = 0;
    while (start < items.Count())
    {
        conversions.Next.Store(start);
        conversions.End = items.Count();
        const usize count = conversions.End - start;
        const uint32 threadCount = count < maxThreads ? static_cast<uint32>(count) : maxThreads;
        List<ThreadHandle> threads;
        for (uint32 i = 0; i < threadCount; ++i)
        {
            ThreadDesc thread;
            thread.Name = "Marlin Packaging";
            thread.Entry = [](void *argument) { static_cast<Conversions *>(argument)->Run(); };
            thread.Argument = &conversions;
            thread.StackSize = usize{4} << 20;
            threads.Add(PlatformThread::Create(thread));
        }
        for (const ThreadHandle &thread : threads)
        {
            PlatformThread::Join(thread);
        }
        const usize end = conversions.End;
        for (usize i = start; i < end; ++i)
        {
            for (const Guid &derived : items[i].Result.Derived)
            {
                if (wanted.Add(derived))
                {
                    // RECOVERY-DECISION SP-D3: a derived result goes into the chunk and format of its source object.
                    Item &item = items.AddDefaulted();
                    item.Uuid = derived;
                    item.Path = items[i].Path;
                    item.PackageClass = items[i].PackageClass;
                }
            }
        }
        start = end;
    }
    bool ok = true;
    for (const Item &item : items)
    {
        if (!item.Result.Succeeded)
        {
            const String &name = !item.Script.IsEmpty()   ? item.Script
                                 : !item.Shader.IsEmpty() ? item.Shader
                                                          : item.Path;
            errors.Append(Format("{0} ({1}): {2}\n", item.Uuid, name, item.Result.Errors));
            ok = false;
        }
    }
    if (!ok)
    {
        return false;
    }

    CompressionCodecSet codecs;
    codecs.Build(ExtensionContext{});
    PakCrypto crypto;
    crypto.Build(ExtensionContext{});
    PakEncryption encryption;
    const PackageEncryption &encryptionSettings = desc.Settings.Encryption;
    if (!encryptionSettings.Cipher.IsEmpty())
    {
        encryption.Cipher = crypto.FindCipher(encryptionSettings.Cipher.AsView());
        if (encryption.Cipher == nullptr)
        {
            errors.Append(Format("no linked PakCipher is called {0}\n", encryptionSettings.Cipher));
            return false;
        }
        if (desc.EncryptionKey.Count() != encryption.Cipher->GetKeySize())
        {
            errors.Append(Format("{0} needs a key of {1} bytes\n", encryption.Cipher->GetName(),
                                 encryption.Cipher->GetKeySize()));
            return false;
        }
        encryption.KeyId = encryptionSettings.KeyId;
        encryption.Key.AddRange(desc.EncryptionKey.AsSpan());
        // Previous packages of an encrypted release are read with the same key.
        crypto.AddKey(encryption.KeyId, desc.EncryptionKey.AsSpan());
    }
    const PackageSigning &signing = desc.Settings.Signing;
    const PakSignatureScheme *scheme = nullptr;
    if (!signing.Scheme.IsEmpty())
    {
        scheme = crypto.FindScheme(signing.Scheme.AsView());
        if (scheme == nullptr || scheme->GetName().Length() >= sizeof(PakSignatureHeader::Scheme))
        {
            errors.Append(Format("no linked PakSignatureScheme is called {0}\n", signing.Scheme));
            return false;
        }
        if (desc.SigningKey.IsEmpty())
        {
            errors.Append(Format("Engine.Packaging.Signing names {0} but no signing key was given\n", signing.Scheme));
            return false;
        }
    }

    // The release a patch builds on: its base packages, then its patches in order, each overriding what came before.
    HashMap<Guid, Released> previousAssets;
    HashMap<String, Released> previousFiles;
    List<SharedPtr<PakFile>> previous;
    for (const String &path : desc.Previous)
    {
        SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), codecs, &crypto);
        if (!pak.IsValid())
        {
            errors.Append(Format("{0} is not a package this packager can read\n", path));
            return false;
        }
        previous.Add(Move(pak));
    }
    previous.Sort([](const SharedPtr<PakFile> &a, const SharedPtr<PakFile> &b) {
        const bool patchA = a->GetChunk().StartsWith("Patch_");
        const bool patchB = b->GetChunk().StartsWith("Patch_");
        return patchA != patchB ? patchB : patchA && String(a->GetChunk()) < String(b->GetChunk());
    });
    for (const SharedPtr<PakFile> &pak : previous)
    {
        for (const PakAssetEntry &entry : pak->GetAssets())
        {
            Released &released = previousAssets.FindOrAdd(entry.Uuid);
            released.ContentHash = entry.ContentHash;
            released.Deleted = entry.BlockSize == 0;
        }
        for (const PakFileEntry &entry : pak->GetFiles())
        {
            const StringView mountPoint = pak->GetString(entry.MountPoint);
            const StringView layer = pak->GetString(entry.Layer);
            const StringView path = pak->GetString(entry.Path);
            Released &released = previousFiles.FindOrAdd(FileKey(mountPoint, layer, path));
            released.ContentHash = entry.ContentHash;
            released.Deleted = (entry.Flags & PakFileDeleted) != 0;
            released.MountPoint = mountPoint;
            released.Layer = layer;
            released.Path = path;
        }
    }
    const bool patching = !desc.Previous.IsEmpty();
    const String patchChunk = Format("Patch_{0}", Format("{0}", 10000 + desc.PatchNumber).AsView().RemoveStart(1));
    const auto unchanged = [](const Released *released, const Hash128 &hash) {
        return released != nullptr && !released->Deleted && released->ContentHash == hash;
    };

    const auto formatOf = [&](StringView packageClass) {
        const PackageCompression *cell = desc.Settings.Compression.Find(String(packageClass));
        if (cell == nullptr)
        {
            cell = desc.Settings.Compression.Find(String("Package"));
        }
        return cell != nullptr ? *cell : PackageCompression();
    };
    const PackageCompression packageFormat = formatOf("Package");
    const PakEncryption *encrypted = encryption.Cipher != nullptr ? &encryption : nullptr;
    HashMap<String, UniquePtr<PakWriter>> writers;
    List<String> writerOrder;
    /// The package objects of *packageClass* in *chunk* go into.
    const auto nameOf = [&](StringView chunk, StringView packageClass) -> String {
        const PackageCompression format = formatOf(packageClass);
        return format.Format == packageFormat.Format
                   ? String(chunk)
                   : Format("{0}.{1}", chunk, format.Format.IsEmpty() ? StringView("Stored") : format.Format.AsView());
    };
    const auto writerFor = [&](StringView chunk, StringView packageClass) -> PakWriter * {
        const PackageCompression format = formatOf(packageClass);
        const String name = nameOf(chunk, packageClass);
        if (UniquePtr<PakWriter> *existing = writers.Find(name))
        {
            return existing->Get();
        }
        const CompressionCodec *codec = format.Format.IsEmpty() ? nullptr : codecs.Find(format.Format.AsView());
        if (!format.Format.IsEmpty() && codec == nullptr)
        {
            errors.Append(Format("{0} data is packaged as {1}, which no linked CompressionCodec provides\n",
                                 packageClass, format.Format));
            return nullptr;
        }
        UniquePtr<PakWriter> &writer = writers.FindOrAdd(name);
        writer = MakeUnique<PakWriter>(chunk, codec, format.Level, desc.Settings.BlockSize, encrypted);
        writerOrder.Add(name);
        return writer.Get();
    };
    const auto chunkOf = [&](StringView path) -> String {
        if (patching)
        {
            return patchChunk;
        }
        for (const PackageChunkRule &rule : desc.Settings.Chunks)
        {
            for (const String &folder : rule.Paths)
            {
                if (path.StartsWith(folder.AsView()) &&
                    (path.Length() == folder.Length() || path[folder.Length()] == '/'))
                {
                    return rule.Name;
                }
            }
        }
        return String("Base");
    };

    // Objects of the load order first, in that order, so a package reads front to back as the game starts; the rest
    // in the order they were found.
    List<const Item *> sequence;
    {
        HashMap<Guid, usize> positions;
        for (usize i = 0; i < items.Count(); ++i)
        {
            positions.FindOrAdd(items[i].Uuid) = i;
        }
        List<bool> placed;
        placed.Resize(items.Count());
        for (const Guid &uuid : desc.LoadOrder)
        {
            const usize *position = positions.Find(uuid);
            if (position != nullptr && !placed[*position])
            {
                placed[*position] = true;
                sequence.Add(&items[*position]);
            }
        }
        for (usize i = 0; i < items.Count(); ++i)
        {
            if (!placed[i])
            {
                sequence.Add(&items[i]);
            }
        }
    }

    // The loose files, read first: what they hold is part of what a package is written from.
    struct PendingFile
    {
        const PackageFileSource *Source = nullptr;
        String Relative;
        List<uint8> Bytes;
        Hash128 Hash;
    };
    List<PendingFile> pendingFiles;
    HashSet<String> currentFiles;
    for (const PackageFileSource &source : desc.Files)
    {
        List<String> files;
        CollectFiles(source.Directory, source.Prefix, files);
        for (const String &relative : files)
        {
            List<uint8> bytes;
            const String native = Format("{0}/{1}", source.Directory, relative);
            const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting);
            uint64 size = 0;
            usize read = 0;
            const bool readable = file.IsValid() && PlatformFile::GetSize(file, size) &&
                                  (bytes.ResizeUninitialized(static_cast<usize>(size)), true) &&
                                  PlatformFile::Read(file, bytes.Data(), bytes.Count(), read) && read == bytes.Count();
            if (file.IsValid())
            {
                PlatformFile::Close(file);
            }
            if (!readable)
            {
                errors.Append(Format("{0} cannot be read\n", native));
                return false;
            }
            const String key = FileKey(source.MountPoint.AsView(), source.Layer.AsView(), relative.AsView());
            (void)currentFiles.Add(key);
            const Hash128 hash = Xxh3Hash128(bytes.Data(), bytes.Count());
            if (patching && unchanged(previousFiles.Find(key), hash))
            {
                ++result.Unchanged;
                continue;
            }
            pendingFiles.Add({&source, relative, Move(bytes), hash});
        }
    }

    // Which packages are as the last build wrote them: the same settings, the same objects in the same order by
    // result key (a key names its content), the same files. Those are neither read from GlobalStore nor written.
    HashSet<String> kept;
    HashMap<String, Hash128> fingerprints;
    const bool remember = !patching && !desc.StateDirectory.IsEmpty();
    const auto statePathOf = [&](StringView name) {
        const Hash128 folder = Xxh3Hash128(desc.OutputDirectory.Data(), desc.OutputDirectory.Length());
        return Format("{0}/{1:x}{2:x}.{3}.inputs", desc.StateDirectory, folder.High, folder.Low, name);
    };
    if (remember)
    {
        HashMap<String, String> texts;
        List<String> names;
        // What decides a package's bytes besides its content: the block size, the encryption and the signing.
        const Hash128 encryptionKey = Xxh3Hash128(desc.EncryptionKey.Data(), desc.EncryptionKey.Count());
        const Hash128 signingKey = Xxh3Hash128(desc.SigningKey.Data(), desc.SigningKey.Count());
        const String settings =
            Format("{0}\n{1} {2} {3:x}{4:x}\n{5} {6} {7:x}{8:x}\n", desc.Settings.BlockSize, encryptionSettings.Cipher,
                   encryptionSettings.KeyId, encryptionKey.High, encryptionKey.Low, signing.Scheme, signing.KeyId,
                   signingKey.High, signingKey.Low);
        const auto textOf = [&](const String &name) -> String & {
            if (String *existing = texts.Find(name))
            {
                return *existing;
            }
            names.Add(name);
            String &text = texts.FindOrAdd(name);
            text.Append(name.AsView()).Append('\n').Append(settings.AsView());
            return text;
        };
        for (const Item *item : sequence)
        {
            const PackageCompression format = formatOf(item->PackageClass.AsView());
            String &text = textOf(nameOf(chunkOf(item->Path.AsView()).AsView(), item->PackageClass.AsView()));
            text.Append(Format("{0} {1} {2} {3}\n", item->Uuid, item->Result.Location.Key, format.Format, format.Level)
                            .AsView());
        }
        for (const PendingFile &file : pendingFiles)
        {
            String &text = textOf(nameOf("Base", "Package"));
            text.Append(FileKey(file.Source->MountPoint.AsView(), file.Source->Layer.AsView(), file.Relative.AsView())
                            .AsView());
            text.Append(Format(" {0:x}{1:x} {2} {3}\n", file.Hash.High, file.Hash.Low, packageFormat.Format,
                               packageFormat.Level)
                            .AsView());
        }
        for (const String &name : names)
        {
            const String &text = *texts.Find(name);
            const Hash128 fingerprint = Xxh3Hash128(text.Data(), text.Length());
            fingerprints.FindOrAdd(name) = fingerprint;
            const String path = Format("{0}/{1}.mpak", desc.OutputDirectory, name);
            String stored;
            StringBuilder<64> expected;
            expected.Append(Format("{0:x}{1:x}", fingerprint.High, fingerprint.Low).AsView());
            if (PlatformFile::FileExists(path.CStr()) &&
                (scheme == nullptr || PlatformFile::FileExists(PakSigning::SignaturePath(path.AsView()).CStr())) &&
                ReadNativeText(statePathOf(name.AsView()), stored) && stored.AsView() == expected.AsView())
            {
                (void)kept.Add(name);
            }
        }
    }
    const auto isKept = [&](const Item &item) {
        return kept.Contains(nameOf(chunkOf(item.Path.AsView()).AsView(), item.PackageClass.AsView()));
    };

    // Each object's block and bulk are read on the workers, only those parts, straight into one buffer: the block,
    // then the bulk, as PakWriter keeps them. The block's own dependency list goes into the package's table.
    struct Converted
    {
        List<uint8> Content;
        usize BlockSize = 0;
        List<Guid> Dependencies;
        Hash128 ContentHash;
        bool Read = false;
    };
    List<Converted> converted;
    converted.Resize(sequence.Count());
    Jobs::ParallelFor(static_cast<uint32>(sequence.Count()), 1, [&](uint32 begin, uint32 end) {
        for (uint32 i = begin; i < end; ++i)
        {
            Converted &entry = converted[i];
            const ConvertedResultLocation &location = sequence[i]->Result.Location;
            if (isKept(*sequence[i]))
            {
                entry.Read = true;
                continue;
            }
            if (!store->ReadResult(location, entry.Content))
            {
                continue;
            }
            entry.BlockSize = static_cast<usize>(location.BlockSize);
            AssetBlockHeader header;
            if (entry.BlockSize >= sizeof(header))
            {
                MemCopy(&header, entry.Content.Data(), sizeof(header));
                if (header.Magic == AssetBlockMagic && header.DependencyOffset <= entry.BlockSize &&
                    uint64{header.DependencyCount} * sizeof(Guid) <= entry.BlockSize - header.DependencyOffset)
                {
                    entry.Dependencies.Resize(header.DependencyCount);
                    MemCopy(entry.Dependencies.Data(), entry.Content.Data() + header.DependencyOffset,
                            header.DependencyCount * sizeof(Guid));
                }
            }
            entry.ContentHash = Xxh3Hash128(entry.Content.Data(), entry.Content.Count());
            entry.Read = true;
        }
    });
    for (usize i = 0; i < sequence.Count(); ++i)
    {
        const Item *item = sequence[i];
        Converted &entry = converted[i];
        if (!entry.Read)
        {
            errors.Append(Format("{0}: its converted result cannot be read\n", item->Uuid));
            return false;
        }
        if (patching && unchanged(previousAssets.Find(item->Uuid), entry.ContentHash))
        {
            ++result.Unchanged;
            entry.Content = List<uint8>();
            continue;
        }
        if (!isKept(*item))
        {
            PakWriter *writer = writerFor(chunkOf(item->Path.AsView()).AsView(), item->PackageClass.AsView());
            if (writer == nullptr)
            {
                return false;
            }
            writer->AddAsset(item->Uuid, Move(entry.Content), entry.BlockSize, entry.Dependencies.AsSpan());
        }
        if (!item->Script.IsEmpty())
        {
            ++result.ScriptModules;
        }
        else if (item->Shader.IsEmpty())
        {
            ++result.Objects;
        }
        else
        {
            ++result.Shaders;
        }
    }

    for (const PendingFile &file : pendingFiles)
    {
        ++result.Files;
        if (kept.Contains(nameOf("Base", "Package")))
        {
            continue;
        }
        PakWriter *writer = writerFor(patching ? patchChunk.AsView() : StringView("Base"), "Package");
        if (writer == nullptr)
        {
            return false;
        }
        writer->AddFile(file.Source->MountPoint.AsView(), file.Source->Layer.AsView(), file.Relative.AsView(),
                        file.Bytes.AsSpan());
    }

    // A patch removes what the release has and this build no longer packages: objects whose source is gone or no
    // longer referenced, and files deleted from their folders.
    if (patching)
    {
        for (const auto &entry : previousAssets)
        {
            if (!entry.Value.Deleted && !wanted.Contains(entry.Key))
            {
                PakWriter *writer = writerFor(patchChunk.AsView(), "Package");
                if (writer == nullptr)
                {
                    return false;
                }
                writer->AddDeletedAsset(entry.Key);
                ++result.Deleted;
            }
        }
        for (const auto &entry : previousFiles)
        {
            if (!entry.Value.Deleted && !currentFiles.Contains(entry.Key))
            {
                PakWriter *writer = writerFor(patchChunk.AsView(), "Package");
                if (writer == nullptr)
                {
                    return false;
                }
                writer->AddDeletedFile(entry.Value.MountPoint.AsView(), entry.Value.Layer.AsView(),
                                       entry.Value.Path.AsView());
                ++result.Deleted;
            }
        }
    }

    (void)PlatformFile::CreateDirectory(desc.OutputDirectory.CStr());
    for (const String &name : writerOrder)
    {
        const PakWriter &writer = *writers.Find(name)->Get();
        if (writer.IsEmpty())
        {
            continue;
        }
        const String path = Format("{0}/{1}.mpak", desc.OutputDirectory, name);
        // A signature an earlier build left would no longer match, and the game would refuse the package.
        if (scheme == nullptr && PlatformFile::FileExists(PakSigning::SignaturePath(path.AsView()).CStr()))
        {
            errors.Append(Format("{0} is from a signed build and would not match; remove it or sign this build\n",
                                 PakSigning::SignaturePath(path.AsView())));
            return false;
        }
        if (remember)
        {
            // Until the package is written and signed, nothing says it is as any build made it.
            (void)PlatformFile::Delete(statePathOf(name.AsView()).CStr());
        }
        StringBuilder<256> error;
        List<uint8> hashes;
        if (!writer.Write(path.AsView(), error, scheme != nullptr ? &hashes : nullptr))
        {
            errors.Append(error.AsView());
            errors.Append("\n");
            return false;
        }
        if (scheme != nullptr && !SignPackage(path, *scheme, desc.SigningKey.AsSpan(), signing.KeyId, hashes, errors))
        {
            return false;
        }
        result.Packages.Add(path);
        if (const Hash128 *fingerprint = fingerprints.Find(name); remember && fingerprint != nullptr)
        {
            (void)PlatformFile::CreateDirectory(desc.StateDirectory.CStr());
            (void)WriteNativeText(statePathOf(name.AsView()),
                                  Format("{0:x}{1:x}", fingerprint->High, fingerprint->Low).AsView());
        }
    }
    for (const auto &[name, fingerprint] : fingerprints)
    {
        if (kept.Contains(name))
        {
            result.Packages.Add(Format("{0}/{1}.mpak", desc.OutputDirectory, name));
            ++result.Kept;
        }
    }
    return true;
}
} // namespace Marlin
