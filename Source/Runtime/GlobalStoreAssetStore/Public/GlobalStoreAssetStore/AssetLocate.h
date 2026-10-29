#pragma once

#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "GlobalStoreAssetStore/AssetLocate.gen.h"

namespace Marlin
{
class RemoteClient;

/// What the asset indexer's Asset.Locate takes and gives (Documentation/Architecture/06 §2, §3): shared by the
/// indexer, which converts, and every process that loads converted data in development.
MENUM()
enum class TargetKind : uint8
{
    Client,
    /// Dedicated servers: textures, audio and other client-only data are stripped.
    Server,
};

/// What a conversion is for; part of every result key.
MSTRUCT()
struct ConversionTarget
{
    MGENERATED_BODY()

    /// Such as Windows or PS5.
    MPROPERTY()
    String Platform;
    MPROPERTY()
    TargetKind Kind = TargetKind::Client;
};

/// Where a converted result lies: the GlobalStore key of its ConvertedResult value (06 §3.3) and the asset block and
/// bulk as logical positions in it, which is what Asset.Locate and Shader.Locate answer.
MSTRUCT()
struct ConvertedResultLocation
{
    MGENERATED_BODY()

    /// The result key as 32 hexadecimal digits, high half first (FormatResultKey).
    MPROPERTY()
    String Key;
    MPROPERTY()
    uint64 BlockOffset = 0;
    MPROPERTY()
    uint64 BlockSize = 0;
    MPROPERTY()
    uint64 BulkOffset = 0;
    MPROPERTY()
    uint64 BulkSize = 0;
};

MSTRUCT()
struct AssetLocateParams
{
    MGENERATED_BODY()

    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    ConversionTarget Target;
    /// A load waits for it; otherwise it is background warming.
    MPROPERTY()
    bool Urgent = true;
};

/// What the indexer's Shader.Locate takes: a global shader by its entry point (MarlinLanguage.md §1.6). It answers
/// with a ConvertedResultLocation like Asset.Locate, and source edits announce the shader's Guid on Asset.Changed.
MSTRUCT()
struct ShaderLocateParams
{
    MGENERATED_BODY()

    /// The qualified name, such as Marlin.Passes.Ui.Raster.
    MPROPERTY()
    String EntryPoint;
    /// GetGlobalShaderGuid of the entry point, which the conversion and Asset.Changed name it by.
    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    ConversionTarget Target;
    MPROPERTY()
    bool Urgent = true;
};

/// What the indexer's Script.Locate takes: the script module of a package (MarlinScript §11.1). It answers like
/// Shader.Locate, and script source edits announce the module's Guid on Asset.Changed.
MSTRUCT()
struct ScriptLocateParams
{
    MGENERATED_BODY()

    /// The package, such as Engine or Project.
    MPROPERTY()
    String Package;
    /// GetScriptModuleGuid of the package, which the conversion and Asset.Changed name it by.
    MPROPERTY()
    Guid Uuid;
    MPROPERTY()
    ConversionTarget Target;
    MPROPERTY()
    bool Urgent = true;
};

/// ConvertedResultLocation::Key's text for *key*: 32 lowercase hexadecimal digits, High then Low.
[[nodiscard]] GLOBALSTOREASSETSTORE_API String FormatResultKey(const Hash128 &key);
/// The key FormatResultKey wrote; false if *text* is not 32 hexadecimal digits.
GLOBALSTOREASSETSTORE_API bool ParseResultKey(StringView text, Hash128 &key);

namespace AssetIndexer
{
/// The pipe the indexer of the project in *projectRoot* serves: one indexer per project and machine.
[[nodiscard]] GLOBALSTOREASSETSTORE_API String PipeNameFor(StringView projectRoot);
/// Connects *client* to the project's indexer; with *launch*, starts MarlinAssetIndexer from the executable's
/// folder for *projectFile* if none answers, and waits up to *timeout* milliseconds for it.
GLOBALSTOREASSETSTORE_API bool Connect(RemoteClient &client, StringView projectRoot, StringView projectFile,
                                       bool launch, uint32 timeout);
} // namespace AssetIndexer
} // namespace Marlin
