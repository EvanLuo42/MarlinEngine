#include "String/Name.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Memory/Memory.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr uint32 NameSegmentBits = 12;
constexpr uint32 NameSegmentSize = 1u << NameSegmentBits;
constexpr uint32 MaxNameSegments = 4096;
constexpr uint32 NameShardCount = 16;
constexpr usize NameChunkSize = usize{64} * 1024;

/// Index to text; written once per entry and read without locks.
Atomic<StringView *> _nameSegments[MaxNameSegments];

/// A cache line each, so adding names in different shards does not contend.
struct alignas(64) NameShard
{
    SpinLock Lock;
    HashMap<StringView, uint32> Map;
};

/// Adding names: the shards map text to indices, and the entries hold the interned bytes.
struct NameTable
{
    NameShard Shards[NameShardCount];
    SpinLock EntryLock;
    uint32 Count = 1;
    char *Chunk = nullptr;
    usize ChunkRemaining = 0;

    StringView AddEntry(StringView text, uint32 &index)
    {
        const ScopeLock lock(EntryLock);
        char *copy = nullptr;
        if (text.Length() > NameChunkSize / 4)
        {
            copy = static_cast<char *>(Memory::Allocate(text.Length()));
        }
        else
        {
            if (ChunkRemaining < text.Length())
            {
                Chunk = static_cast<char *>(Memory::Allocate(NameChunkSize));
                ChunkRemaining = NameChunkSize;
            }
            copy = Chunk;
            Chunk += text.Length();
            ChunkRemaining -= text.Length();
        }
        MemCopy(copy, text.Data(), text.Length());

        index = Count++;
        const uint32 segment = index >> NameSegmentBits;
        MARLIN_ASSERT(segment < MaxNameSegments);
        StringView *entries = _nameSegments[segment].Load(MemoryOrder::Relaxed);
        if (entries == nullptr)
        {
            entries = static_cast<StringView *>(Memory::Allocate(NameSegmentSize * sizeof(StringView)));
            _nameSegments[segment].Store(entries, MemoryOrder::Release);
        }
        entries[index & (NameSegmentSize - 1)] = {copy, text.Length()};
        return {copy, text.Length()};
    }
};

/// Built on first use and never destroyed, so names stay valid through static destruction.
NameTable &Table()
{
    alignas(NameTable) static uint8 storage[sizeof(NameTable)];
    static NameTable *table = ConstructAt(reinterpret_cast<NameTable *>(storage));
    return *table;
}

NameShard &ShardFor(uint64 hash)
{
    return Table().Shards[hash >> 60];
}
} // namespace

Name::Name(StringView text)
{
    if (text.IsEmpty())
    {
        return;
    }
    // One hash picks the shard and probes and fills its map.
    const uint64 hash = text.GetHash();
    NameShard &shard = ShardFor(hash);
    const ScopeLock lock(shard.Lock);
    if (const uint32 *found = shard.Map.FindHashed(text, hash))
    {
        _index = *found;
        return;
    }
    uint32 index = 0;
    const StringView stored = Table().AddEntry(text, index);
    shard.Map.AddHashed(stored, hash, index);
    _index = index;
}

Name Name::Find(StringView text)
{
    if (text.IsEmpty())
    {
        return {};
    }
    const uint64 hash = text.GetHash();
    NameShard &shard = ShardFor(hash);
    const ScopeLock lock(shard.Lock);
    const uint32 *found = shard.Map.FindHashed(text, hash);
    return found != nullptr ? FromIndex(*found) : Name();
}

StringView Name::ToView() const
{
    if (_index == 0)
    {
        return {};
    }
    const StringView *entries = _nameSegments[_index >> NameSegmentBits].Load(MemoryOrder::Acquire);
    return entries[_index & (NameSegmentSize - 1)];
}

void Name::FormatTo(StringWriter &output, StringView /*spec*/) const
{
    output.Append(ToView());
}
} // namespace Marlin
