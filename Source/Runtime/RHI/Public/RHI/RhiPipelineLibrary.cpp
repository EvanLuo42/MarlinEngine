#include "RHI/RhiPipelineLibrary.h"

#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "RHI/RhiLog.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
constexpr uint32 ListMagic = 0x4F53504D;
constexpr uint32 ListVersion = 1;

struct ListHeader
{
    uint32 Magic = ListMagic;
    uint32 Version = ListVersion;
    uint32 Count = 0;
    uint32 BackendLength = 0;
};

struct StoredRecord
{
    uint64 Low = 0;
    uint64 High = 0;
    uint8 Kind = 0;
    uint8 Missed = 0;
    uint8 Padding[2] = {};
    uint32 PayloadSize = 0;
};
static_assert(sizeof(StoredRecord) == 24);

template <typename T> void Append(List<uint8> &bytes, const T &value)
{
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(&value), sizeof(T)));
}
} // namespace

RhiPipelineLibrary::RhiPipelineLibrary(RhiDevice &device) : _device(device)
{
}

RhiPipelineLibrary::~RhiPipelineLibrary()
{
    for (const auto &entry : _entries)
    {
        if (entry.Value.Pipeline.IsValid())
        {
            _device.Destroy(entry.Value.Pipeline);
        }
    }
}

RhiPipeline RhiPipelineLibrary::Get(const RhiPipelineRecord &record, RhiPipelineFactory factory, void *context)
{
    {
        const ScopeLock lock(_lock);
        if (Entry *entry = _entries.Find(record.Key))
        {
            if (entry->Pipeline.IsValid() && !entry->Missed &&
                _device.GetPipelineState(entry->Pipeline) == RhiPipelineState::Pending)
            {
                entry->Missed = true;
            }
            return entry->Pipeline;
        }
    }
    const RhiPipeline pipeline = factory(_device, record, context);
    const ScopeLock lock(_lock);
    if (const Entry *raced = _entries.Find(record.Key))
    {
        if (pipeline.IsValid())
        {
            _device.Destroy(pipeline);
        }
        return raced->Pipeline;
    }
    Entry entry;
    entry.Pipeline = pipeline;
    entry.Missed = pipeline.IsValid() && _device.GetPipelineState(pipeline) == RhiPipelineState::Pending;
    entry.Payload.AddRange(record.Payload);
    _entries.Add(record.Key, Move(entry));
    return pipeline;
}

RhiPipeline RhiPipelineLibrary::Find(const RhiPipelineKey &key) const
{
    const ScopeLock lock(_lock);
    const Entry *entry = _entries.Find(key);
    return entry != nullptr ? entry->Pipeline : RhiPipeline{};
}

uint32 RhiPipelineLibrary::Preload(Span<const RhiPipelineRecord> records, RhiPipelineFactory factory, void *context)
{
    uint32 started = 0;
    for (const bool missedPass : {true, false})
    {
        for (const RhiPipelineRecord &record : records)
        {
            if (record.Missed != missedPass)
            {
                continue;
            }
            {
                const ScopeLock lock(_lock);
                if (_entries.Contains(record.Key))
                {
                    continue;
                }
            }
            const RhiPipeline pipeline = factory(_device, record, context);
            if (!pipeline.IsValid())
            {
                continue;
            }
            const ScopeLock lock(_lock);
            Entry entry;
            entry.Pipeline = pipeline;
            entry.Preloaded = true;
            entry.Payload.AddRange(record.Payload);
            if (_entries.TryAdd(record.Key, Move(entry)))
            {
                ++started;
            }
            else
            {
                _device.Destroy(pipeline);
            }
        }
    }
    return started;
}

uint32 RhiPipelineLibrary::GetPendingPreloads() const
{
    const ScopeLock lock(_lock);
    uint32 pending = 0;
    for (const auto &entry : _entries)
    {
        if (entry.Value.Preloaded && _device.GetPipelineState(entry.Value.Pipeline) == RhiPipelineState::Pending)
        {
            ++pending;
        }
    }
    return pending;
}

List<RhiPipelineRecord> RhiPipelineLibrary::GetRecords() const
{
    const ScopeLock lock(_lock);
    List<RhiPipelineRecord> records;
    records.Reserve(_entries.Count());
    for (const auto &entry : _entries)
    {
        if (!entry.Value.Preloaded || entry.Value.Missed)
        {
            RhiPipelineRecord &record = records.AddDefaulted();
            record.Key = entry.Key;
            record.Missed = entry.Value.Missed;
            record.Payload.AddRange(entry.Value.Payload);
        }
    }
    return records;
}

bool RhiPipelineLibrary::Save(StringView path, Name backend, Span<const RhiPipelineRecord> records)
{
    const StringView name = backend.ToView();
    ListHeader header;
    header.Count = static_cast<uint32>(records.Count());
    header.BackendLength = static_cast<uint32>(name.Length());
    List<uint8> bytes;
    Append(bytes, header);
    bytes.AddRange(Span<const uint8>(reinterpret_cast<const uint8 *>(name.Data()), name.Length()));
    for (const RhiPipelineRecord &record : records)
    {
        StoredRecord stored;
        stored.Low = record.Key.Hash.Low;
        stored.High = record.Key.Hash.High;
        stored.Kind = static_cast<uint8>(record.Key.Kind);
        stored.Missed = record.Missed ? 1 : 0;
        stored.PayloadSize = static_cast<uint32>(record.Payload.Count());
        Append(bytes, stored);
        bytes.AddRange(record.Payload);
    }
    return Vfs::WriteAll(path, StringView(reinterpret_cast<const char *>(bytes.Data()), bytes.Count()));
}

bool RhiPipelineLibrary::Load(StringView path, Name backend, List<RhiPipelineRecord> &records)
{
    List<uint8> bytes;
    if (!Vfs::ReadAll(path, bytes) || bytes.Count() < sizeof(ListHeader))
    {
        return false;
    }
    ListHeader header;
    __builtin_memcpy(&header, bytes.Data(), sizeof(header));
    usize offset = sizeof(ListHeader) + header.BackendLength;
    if (header.Magic != ListMagic || header.Version != ListVersion || offset > bytes.Count())
    {
        MARLIN_LOG(LogRhi, Warning, "{0} is not a pipeline list of this version", path);
        return false;
    }
    const StringView name(reinterpret_cast<const char *>(bytes.Data() + sizeof(ListHeader)), header.BackendLength);
    if (name != backend.ToView())
    {
        return false;
    }
    const usize first = records.Count();
    for (uint32 i = 0; i < header.Count; ++i)
    {
        StoredRecord stored;
        if (offset + sizeof(stored) > bytes.Count())
        {
            break;
        }
        __builtin_memcpy(&stored, bytes.Data() + offset, sizeof(stored));
        offset += sizeof(stored);
        if (offset + stored.PayloadSize > bytes.Count())
        {
            break;
        }
        RhiPipelineRecord &record = records.AddDefaulted();
        record.Key = {{stored.Low, stored.High}, static_cast<RhiPipelineKind>(stored.Kind)};
        record.Missed = stored.Missed != 0;
        record.Payload.AddRange(Span<const uint8>(bytes.Data() + offset, stored.PayloadSize));
        offset += stored.PayloadSize;
    }
    if (offset != bytes.Count() || records.Count() - first != header.Count)
    {
        MARLIN_LOG(LogRhi, Warning, "{0} is a damaged pipeline list", path);
        records.Resize(first);
        return false;
    }
    return true;
}

void RhiPipelineLibrary::Merge(List<RhiPipelineRecord> &records, Span<const RhiPipelineRecord> other)
{
    HashMap<RhiPipelineKey, usize> index;
    for (usize i = 0; i < records.Count(); ++i)
    {
        index.Set(records[i].Key, i);
    }
    for (const RhiPipelineRecord &record : other)
    {
        if (const usize *existing = index.Find(record.Key))
        {
            records[*existing].Missed = records[*existing].Missed || record.Missed;
        }
        else
        {
            index.Add(record.Key, records.Count());
            RhiPipelineRecord &added = records.AddDefaulted();
            added.Key = record.Key;
            added.Missed = record.Missed;
            added.Payload.AddRange(record.Payload);
        }
    }
}
} // namespace Marlin
