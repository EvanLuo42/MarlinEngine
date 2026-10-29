#pragma once

#include "Compression/CompressionCodec.h"
#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "GlobalStore/ConvertedResult.h"
#include "GlobalStore/GlobalStoreClient.h"
#include "GlobalStoreAssetStore/AssetLocate.h"
#include "HAL/PlatformSync.h"
#include "Hash/Hash128.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "Net/NetLoop.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
/// What a conversion read besides its source object; with their current hashes they complete the result key.
enum class DependencyInputKind : uint8
{
    Object,
    Binary,
    Config,
    /// The image decoders that claim an extension (Section), for the binary file Uuid.
    Decoder,
    /// An input the converter names (Section) and hashes itself, such as the encoders of a texture format.
    Converter,
    /// The scene decoders that claim an extension (Section), for the binary file Uuid.
    SceneDecoder,
};

struct DependencyInput
{
    DependencyInputKind Kind = DependencyInputKind::Object;
    Guid Uuid;
    /// The config section, for Config inputs.
    String Section;
};

/// Which GlobalStore a ConversionStore talks to.
struct ASSETPIPELINE_API ConversionStoreDesc
{
    /// Empty: the GlobalStore on this machine, started (MarlinGlobalStore) when none answers. Otherwise a studio
    /// server; when it cannot be reached, the local one stands in with a warning and it is retried at most every
    /// RetrySeconds.
    String Host;
    uint16 Port = GlobalStoreDefaultPort;
    /// Codec and level converted results are compressed with; null stores every block as is.
    const CompressionCodec *Codec = nullptr;
    int32 Level = 0;
    /// Non-empty: run a GlobalStore server inside this process with its data here, instead of connecting to one,
    /// for tests and tools that must not depend on a MarlinGlobalStore.
    String InProcessDirectory;

    /// Host, Port, Codec and Level from Engine.GlobalStore.
    static ConversionStoreDesc FromConfig();
    /// A server in this process with its data in the virtual folder *directory*, created if need be; for tests.
    static ConversionStoreDesc InProcess(StringView directory);
};

/// Where conversions keep what they make (Documentation/Architecture/06 §3.2, §3.3): dependency records and
/// converted results, only in GlobalStore. Keys are seeded by GlobalStoreDomain; dependency records are looked up
/// in pipelined batches, then the result keys they complete. Safe to share between threads.
class ASSETPIPELINE_API ConversionStore
{
public:
    static constexpr uint64 RetrySeconds = 10;

    explicit ConversionStore(ConversionStoreDesc desc = ConversionStoreDesc::FromConfig());
    ~ConversionStore();
    MARLIN_NONCOPYABLE(ConversionStore);

    /// The client, connected (starting the local server or falling back to it if need be); null when no
    /// GlobalStore can be reached.
    [[nodiscard]] GlobalStoreClient *GetClient();

    /// What it connects to; with InProcessDirectory, Port is the port the server in this process listens on.
    [[nodiscard]] const ConversionStoreDesc &GetDesc() const
    {
        return _desc;
    }

    /// The dependency records of *inputKeys*, in one pipelined round trip; *found* says which there are.
    bool GetDependencies(Span<const Hash128> inputKeys, List<List<DependencyInput>> &records, List<bool> &found);
    bool GetDependencies(const Hash128 &inputKey, List<DependencyInput> &inputs);
    bool PutDependencies(const Hash128 &inputKey, Span<const DependencyInput> inputs);
    /// A record a converter keeps beside its dependency record in its own format, such as the names a shader
    /// compile looked up; *key* is a DependencyKey. False if there is none.
    bool GetRecord(const Hash128 &key, List<uint8> &bytes);
    bool PutRecord(const Hash128 &key, Span<const uint8> bytes);

    /// Which of *keys* have a converted result.
    bool HasResults(Span<const Hash128> keys, List<bool> &found);
    /// Where the results of *keys* are, from their headers, in one pipelined round trip; *found* is false for the
    /// missing ones and for those this process cannot decode (their codec is not linked), which convert again.
    bool LocateResults(Span<const Hash128> keys, List<ConvertedResultLocation> &locations, List<bool> &found);
    bool LocateResult(const Hash128 &key, ConvertedResultLocation &location);
    /// Stores the converted result *key* of *block* and *bulk*, compressed per block; *location* is where they are.
    bool PutResult(const Hash128 &key, Span<const uint8> block, Span<const uint8> bulk,
                   ConvertedResultLocation &location);
    /// The asset block and bulk of the result *key*, decompressed: what the Packager writes into packages.
    bool ReadResult(const Hash128 &key, List<uint8> &block, List<uint8> &bulk);
    /// The asset block of the result *location* names followed by its bulk, in one buffer.
    bool ReadResult(const ConvertedResultLocation &location, List<uint8> &content);
    /// Writes the logical stream of the result *key* to the virtual file *path*, so the block and bulk are at the
    /// offsets ConvertedResultLocation gives; for tests and tools that read results as files.
    bool ExportResult(const Hash128 &key, StringView path);

    /// The key of a dependency record, or a result key, from the text that identifies it.
    [[nodiscard]] static Hash128 DependencyKey(StringView text);
    [[nodiscard]] static Hash128 ResultKey(StringView text);
    /// Where the dependency record goes that follows *missed*, a result key a dependency record led to that turned out
    /// missing or wrong. Values are written once, so the record under an input key keeps the inputs of the first
    /// conversion; when a later one reads other inputs (a dependency now links one more object, a shader now uses one
    /// more file), its record goes here, and lookups follow these redirects from the key the old record gives.
    [[nodiscard]] static Hash128 RedirectKey(const Hash128 &missed);
    /// How many redirects a lookup follows before it converts.
    static constexpr uint32 MaxRedirects = 8;

private:
    struct InProcess;

    bool Connect();
    bool Launch();

    ConversionStoreDesc _desc;
    UniquePtr<InProcess> _inProcess;
    /// Without a NetSubsystem, as in MarlinConverter, the store runs its own loop.
    UniquePtr<NetLoop> _ownLoop;
    UniquePtr<GlobalStoreClient> _client;
    Mutex _connectLock;
    /// While the studio server is unreachable: connected to the local one, and when to try the studio again.
    bool _fellBack = false;
    uint64 _retryAt = 0;
};
} // namespace Marlin
