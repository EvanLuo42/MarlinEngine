#include "AssetPipeline/GlobalStoreWarming.h"

#include "AssetPipeline/AssetConverter.h"
#include "Concurrency/Atomic.h"
#include "Concurrency/ScopeLock.h"
#include "HAL/PlatformCpu.h"
#include "HAL/PlatformSync.h"
#include "HAL/PlatformThread.h"
#include "String/Format.h"
#include "Templates/Utility.h"

namespace Marlin
{
namespace
{
/// The most conversions a thread takes at once: their dependency records and results are looked up in one batch each.
constexpr usize MaxChunkSize = 64;
/// Chunks per thread at least, while chunks are above one conversion: a chunk is converted in order by one thread, so
/// with too few of them (a project of a hundred large meshes, say) most threads would get nothing to do.
constexpr usize ChunksPerThread = 8;

/// The work the threads share: every target for every object, target by target, handed out by Next in chunks.
struct Warming
{
    const AssetIndex *Index = nullptr;
    const GlobalStoreWarmingDesc *Desc = nullptr;
    ConversionStore *Store = nullptr;
    List<Guid> Objects;
    usize ChunkSize = MaxChunkSize;
    Atomic<usize> Next{0};
    Atomic<uint64> Conversions{0};
    Atomic<uint64> CacheHits{0};
    Atomic<uint64> Failures{0};
    Mutex FailureLock;
    StringWriter *FailureText = nullptr;

    void Run()
    {
        // Each thread converts through its own conversion, and all through one store, whose connection pipelines
        // everyone's requests; converters are stateless, so threads never wait on each other.
        IndexSourceResolver resolver(*Index);
        AssetConversion conversion(resolver, *Store);
        conversion.AssumeFixedConfig();
        const usize objects = Objects.Count();
        const usize total = objects * Desc->Targets.Count();
        List<ConversionResult> results;
        for (usize first = Next.FetchAdd(ChunkSize); first < total; first = Next.FetchAdd(ChunkSize))
        {
            const usize end = first + ChunkSize < total ? first + ChunkSize : total;
            // A chunk spans at most a few targets; each part is one batch.
            for (usize begin = first; begin < end;)
            {
                const usize target = begin / objects;
                const usize stop = (target + 1) * objects < end ? (target + 1) * objects : end;
                const Span<const Guid> uuids = Objects.AsSpan().Slice(begin % objects, stop - begin);
                conversion.ConvertMany(uuids, Desc->Targets[target], results);
                for (usize i = 0; i < results.Count(); ++i)
                {
                    Count(uuids[i], Desc->Targets[target], results[i]);
                }
                begin = stop;
            }
        }
    }

    void Count(const Guid &uuid, const ConversionTarget &target, const ConversionResult &result)
    {
        if (result.Succeeded)
        {
            (result.CacheHit ? CacheHits : Conversions).FetchAdd(1);
            return;
        }
        Failures.FetchAdd(1);
        if (FailureText != nullptr)
        {
            AssetObjectInfo info;
            const String name =
                Index->Find(uuid, info) ? Format("{0} ({1})", info.Name, info.Path) : Format("{0}", uuid);
            const ScopeLock lock(FailureLock);
            FailureText->Append(Format("{0} for {1}: {2}", name, target.Platform, result.Errors).AsView());
            FailureText->Append('\n');
        }
    }
};

void WarmingThreadMain(void *argument)
{
    static_cast<Warming *>(argument)->Run();
}
} // namespace

GlobalStoreWarmingStats WarmGlobalStore(const AssetIndex &index, const GlobalStoreWarmingDesc &desc,
                                        StringWriter *failures)
{
    ConversionStore store(desc.Store);
    Warming warming;
    warming.Index = &index;
    warming.Desc = &desc;
    warming.Store = &store;
    warming.FailureText = failures;

    // Only objects some converter turns into run-time data; the others, such as settings objects, are read by their
    // users as source.
    {
        IndexSourceResolver resolver(index);
        AssetConversion conversion(resolver, store);
        for (const String &type : conversion.GetSourceTypes())
        {
            AssetQuery query;
            query.Type = type;
            query.Limit = ~uint32{0};
            List<AssetObjectInfo> objects;
            index.Query(query, objects);
            for (const AssetObjectInfo &object : objects)
            {
                warming.Objects.Add(object.Uuid);
            }
        }
    }

    uint32 threads = desc.Threads != 0 ? desc.Threads : PlatformCpu::GetTopology().LogicalCount;
    threads = threads == 0 ? 1 : threads;
    const usize total = warming.Objects.Count() * desc.Targets.Count();
    warming.ChunkSize = Clamp<usize>(total / (usize{threads} * ChunksPerThread), 1, MaxChunkSize);
    List<ThreadHandle> handles;
    for (uint32 i = 1; i < threads && !desc.Targets.IsEmpty(); ++i)
    {
        ThreadDesc thread;
        thread.Name = "Marlin GlobalStore Warming";
        thread.Entry = &WarmingThreadMain;
        thread.Argument = &warming;
        ThreadHandle handle = PlatformThread::Create(thread);
        if (handle.IsValid())
        {
            handles.Add(handle);
        }
    }
    if (!desc.Targets.IsEmpty() && !warming.Objects.IsEmpty())
    {
        warming.Run();
    }
    for (ThreadHandle &handle : handles)
    {
        PlatformThread::Join(handle);
    }

    GlobalStoreWarmingStats stats;
    stats.Conversions = warming.Conversions.Load();
    stats.CacheHits = warming.CacheHits.Load();
    stats.Failures = warming.Failures.Load();
    return stats;
}
} // namespace Marlin
