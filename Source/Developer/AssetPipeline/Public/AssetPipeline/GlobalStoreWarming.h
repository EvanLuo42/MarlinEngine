#pragma once

#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/ConversionStore.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "GlobalStoreAssetStore/AssetLocate.h"

namespace Marlin
{
class StringWriter;

struct GlobalStoreWarmingDesc
{
    /// Every object is converted once for each of these.
    List<ConversionTarget> Targets;
    /// Conversion threads; zero uses one per core.
    uint32 Threads = 0;
    /// The GlobalStore to fill, Engine.GlobalStore by default.
    ConversionStoreDesc Store = ConversionStoreDesc::FromConfig();
};

struct GlobalStoreWarmingStats
{
    uint64 Conversions = 0;
    uint64 CacheHits = 0;
    uint64 Failures = 0;
};

/// Converts every object of *index* that some converter takes, for every target, into GlobalStore, in this process,
/// so that a build machine can fill the studio's store ahead of the people who will load the results (Marcelis: the
/// farm warms the store, people only fetch; 06 §3.3). Objects already converted count as hits: each thread checks
/// a chunk of objects in two pipelined round trips before converting the misses. Each failure appends a line to
/// *failures*, if given.
[[nodiscard]] ASSETPIPELINE_API GlobalStoreWarmingStats WarmGlobalStore(const AssetIndex &index,
                                                                        const GlobalStoreWarmingDesc &desc,
                                                                        StringWriter *failures = nullptr);
} // namespace Marlin
