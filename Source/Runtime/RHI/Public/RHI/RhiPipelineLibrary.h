#pragma once

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "RHI/RhiDevice.h"
#include "String/Name.h"
#include "String/StringView.h"

namespace Marlin
{
/// Names a pipeline the same way on every run, such as a hash of its pass, its shaders' DDC keys and its state. The
/// layer that declares passes and shaders defines what goes into it.
struct RhiPipelineKey
{
    Hash128 Hash;
    RhiPipelineKind Kind = RhiPipelineKind::Graphics;

    [[nodiscard]] uint64 GetHash() const
    {
        return Hash.Low ^ (Hash.High * 0x9E3779B97F4A7C15ull) ^ static_cast<uint64>(Kind);
    }

    bool operator==(const RhiPipelineKey &) const = default;
};

/// A pipeline list entry: the key, whether a draw had to skip the pipeline because it was still compiling, and what
/// the layer that made it needs to make it again, such as its pass and material variant; the RHI does not read it.
struct RhiPipelineRecord
{
    RhiPipelineKey Key;
    bool Missed = false;
    List<uint8> Payload;
};

/// Builds the pipeline of a record; returns an invalid handle for one it no longer knows.
using RhiPipelineFactory = RhiPipeline (*)(RhiDevice &device, const RhiPipelineRecord &record, void *context);

/// The pipelines of a device by key, and the list that preloads them: every pipeline asked for is recorded, and
/// one that was still compiling when asked for is marked missed, since its draws were skipped. Lists from runs and
/// playtests are saved, merged and shipped, and loading screens preload them. Thread-safe.
class RHI_API RhiPipelineLibrary
{
public:
    explicit RhiPipelineLibrary(RhiDevice &device);
    ~RhiPipelineLibrary();
    MARLIN_NONCOPYABLE(RhiPipelineLibrary);

    /// The pipeline of *record*, made with *factory* the first time; marks it missed while it compiles. The
    /// payload is kept for the list.
    RhiPipeline Get(const RhiPipelineRecord &record, RhiPipelineFactory factory, void *context);
    [[nodiscard]] RhiPipeline Find(const RhiPipelineKey &key) const;

    /// Starts compiling every listed pipeline not made yet, missed ones first; returns how many it started.
    uint32 Preload(Span<const RhiPipelineRecord> records, RhiPipelineFactory factory, void *context);
    /// How many preloaded pipelines are still compiling.
    [[nodiscard]] uint32 GetPendingPreloads() const;

    /// What this run asked for and was not preloaded, or was preloaded and still missed.
    [[nodiscard]] List<RhiPipelineRecord> GetRecords() const;

    /// The list format: a header with the backend, then the records. False if the file cannot be written.
    static bool Save(StringView path, Name backend, Span<const RhiPipelineRecord> records);
    /// Appends the records of a list; false if the file is missing, damaged, or for another backend.
    static bool Load(StringView path, Name backend, List<RhiPipelineRecord> &records);
    /// Adds *other* to *records*: a key missed in either list is missed in the result.
    static void Merge(List<RhiPipelineRecord> &records, Span<const RhiPipelineRecord> other);

private:
    struct Entry
    {
        RhiPipeline Pipeline;
        bool Missed = false;
        bool Preloaded = false;
        List<uint8> Payload;
    };

    RhiDevice &_device;
    mutable SpinLock _lock;
    HashMap<RhiPipelineKey, Entry> _entries;
};
} // namespace Marlin
