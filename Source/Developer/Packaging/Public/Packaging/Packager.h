#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "GlobalStoreAssetStore/AssetLocate.h"
#include "ScriptPipeline/ScriptCompiler.h"
#include "ShaderPipeline/ShaderCompiler.h"
#include "String/String.h"
#include "Packaging/Packager.gen.h"

namespace Marlin
{
class StringWriter;

/// Objects whose source lies under one of Paths go into chunk Name instead of Base.
MSTRUCT()
struct PackageChunkRule
{
    MGENERATED_BODY()

    /// Such as Language_zh-Hans, DLC_Foo or Optional.
    MPROPERTY()
    String Name;
    /// Virtual folders, such as /Project/Content/Loc/zh-Hans.
    MPROPERTY()
    List<String> Paths;
};

/// One cell of the compression matrix: the CompressionCodec a kind of data is packaged with, and its level.
MSTRUCT()
struct PackageCompression
{
    MGENERATED_BODY()

    MPROPERTY()
    String Format;
    MPROPERTY()
    int32 Level = 0;
};

/// Whether packages are encrypted (06 §5.2): the PakCipher by name, empty for none, and the id of the key. The key
/// itself never sits in config; the packager reads it from a file only the build machine has.
MSTRUCT()
struct PackageEncryption
{
    MGENERATED_BODY()

    /// Such as Aes256Ctr; empty for none.
    MPROPERTY()
    String Cipher;
    /// What the package header names, so that the game's PakKeyProvider hands out the matching key.
    MPROPERTY()
    Guid KeyId;
};

/// Whether packages are signed (06 §5.3): the PakSignatureScheme by name, empty for none, and the id of the key pair.
/// The private key never sits in config; the packager reads it from a file only the build machine has, and the game
/// holds the public key in a PakVerifyKeyProvider.
MSTRUCT()
struct PackageSigning
{
    MGENERATED_BODY()

    /// Such as EcdsaP256; empty for none.
    MPROPERTY()
    String Scheme;
    /// What each .msig names, so that the game's PakVerifyKeyProvider hands out the matching public key.
    MPROPERTY()
    Guid KeyId;
};

/// The Engine.Packaging config section (Documentation/Architecture/06 §5.1); platform layers override it.
MSTRUCT()
struct PackagingSettings
{
    MGENERATED_BODY()

    /// The bytes of the logical stream each block holds, compressed on its own; 0 for PakDefaultBlockSize. Smaller
    /// blocks read less for a small object, larger ones compress better.
    MPROPERTY()
    uint32 BlockSize = 0;
    /// The compression matrix: the format of each kind of data, keyed by the run-time type's PackageClass (Package,
    /// Bulk, GpuBuffer, GpuTexture, or any a type names), Package for files and types that name none. Each platform
    /// names its fastest format. Data whose format differs from Package's goes into its own package per chunk, so a
    /// package always has exactly one format.
    MPROPERTY()
    HashMap<String, PackageCompression> Compression;
    MPROPERTY()
    List<PackageChunkRule> Chunks;
    MPROPERTY()
    PackageEncryption Encryption;
    MPROPERTY()
    PackageSigning Signing;
};

/// A native folder whose files a package carries: Directory/Prefix/... becomes Prefix/... under MountPoint in Layer.
struct PackageFileSource
{
    /// Such as /Engine.
    String MountPoint;
    /// A platform layer, such as Windows, or empty for the root layer.
    String Layer;
    /// The native folder the mount point or layer maps, such as <engine>/Engine/Platforms/Windows.
    String Directory;
    /// The folder below Directory to take, such as Config.
    String Prefix;
};

struct PackageDesc
{
    const AssetIndex *Index = nullptr;
    /// The objects to package with everything they reference, and folders whose every object is packaged.
    List<Guid> Roots;
    List<String> RootFolders;
    ConversionTarget Target;
    /// Whether the global shaders of the mounted source set are compiled for Target and packaged (MarlinLanguage.md
    /// §1.6).
    bool GlobalShaders = false;
    ShaderCompileOptions ShaderOptions;
    /// Whether the script packages of the mounted source set are compiled and their modules packaged (MarlinScript
    /// §11.1).
    bool ScriptModules = false;
    ScriptCompileOptions ScriptOptions;
    /// The GlobalStore conversions put their results in and the packages are read from (06 §3.3); null for one
    /// connected as Engine.GlobalStore says. Shared by every conversion thread.
    ConversionStore *Store = nullptr;
    /// Conversion threads; 0 for one per logical core.
    uint32 Threads = 0;
    PackagingSettings Settings;
    List<PackageFileSource> Files;
    /// The native folder the packages are written to; it must be an absolute path. Packages are written over, and
    /// nothing in it is ever deleted.
    String OutputDirectory;
    /// A native folder, such as <Project>/Intermediate/Packaging, where the packager remembers what each package was
    /// written from: a package whose settings, objects (by result key) and files are the same as last time and that
    /// is still there is neither read nor written again. Empty to write every package; a patch always writes.
    String StateDirectory;
    /// For a patch: the native paths of every package of the release it patches, the base and earlier patches. The
    /// patch holds only what changed since then, and deletion entries for what is gone, in chunk Patch_<PatchNumber>.
    List<String> Previous;
    uint32 PatchNumber = 0;
    /// The objects in the order a recorded run loaded them (AssetSubsystem::RecordLoadOrder); they are laid out first
    /// and in that order, the rest after them.
    List<Guid> LoadOrder;
    /// The key of Settings.Encryption, which never comes from config.
    List<uint8> EncryptionKey;
    /// The private key of Settings.Signing, which never comes from config.
    List<uint8> SigningKey;
};

struct PackageResult
{
    uint64 Objects = 0;
    uint64 Files = 0;
    /// Global shaders, which are objects too.
    uint64 Shaders = 0;
    /// Script modules, which are objects too.
    uint64 ScriptModules = 0;
    /// Objects and files a patch leaves out because the release it patches has them as they are.
    uint64 Unchanged = 0;
    /// Objects and files a patch marks as removed.
    uint64 Deleted = 0;
    /// The native paths of the packages written, or kept as they were (Kept of them).
    List<String> Packages;
    uint64 Kept = 0;
};

/// Converts the objects *desc* asks for and everything they reference through the DDC, and writes them with the
/// files into packages: one per chunk and compression format (Documentation/Architecture/06 §5).
PACKAGING_API bool BuildPackages(const PackageDesc &desc, PackageResult &result, StringWriter &errors);
} // namespace Marlin
