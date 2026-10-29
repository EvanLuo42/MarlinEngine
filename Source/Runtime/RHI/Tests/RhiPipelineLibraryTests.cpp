#include "RHI/RhiPipelineLibrary.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
RhiPipelineRecord Record(uint64 low, bool missed, Span<const uint8> payload)
{
    RhiPipelineRecord record;
    record.Key.Hash = {low, ~low};
    record.Key.Kind = low % 2 == 0 ? RhiPipelineKind::Graphics : RhiPipelineKind::Compute;
    record.Missed = missed;
    record.Payload.AddRange(payload);
    return record;
}
} // namespace

MARLIN_TEST(RhiPipelineLibrary, ListsRoundTripAndMerge)
{
    const uint8 pass[] = {'G', 'B', 'u', 'f', 'f', 'e', 'r'};
    List<RhiPipelineRecord> written;
    written.Add(Record(10, false, pass));
    written.Add(Record(11, true, {}));
    const Name backend("Vulkan");
    MARLIN_REQUIRE(RhiPipelineLibrary::Save("/Temp/Tests/Pipelines.mpl", backend, written));

    List<RhiPipelineRecord> read;
    MARLIN_REQUIRE(RhiPipelineLibrary::Load("/Temp/Tests/Pipelines.mpl", backend, read));
    MARLIN_REQUIRE(read.Count() == 2);
    MARLIN_EXPECT(read[0].Key == written[0].Key && !read[0].Missed && read[0].Payload.Count() == sizeof(pass));
    MARLIN_EXPECT(read[0].Payload[0] == 'G' && read[0].Payload[6] == 'r');
    MARLIN_EXPECT(read[1].Key == written[1].Key && read[1].Missed && read[1].Payload.IsEmpty());
    MARLIN_EXPECT(read[1].Key.Kind == RhiPipelineKind::Compute);

    List<RhiPipelineRecord> other;
    other.Add(Record(10, true, pass));
    other.Add(Record(12, false, {}));
    RhiPipelineLibrary::Merge(read, other);
    MARLIN_REQUIRE(read.Count() == 3);
    MARLIN_EXPECT(read[0].Missed && read[2].Key == other[1].Key);

    List<RhiPipelineRecord> wrongBackend;
    MARLIN_EXPECT(!RhiPipelineLibrary::Load("/Temp/Tests/Pipelines.mpl", Name("D3D12"), wrongBackend));
    MARLIN_EXPECT(wrongBackend.IsEmpty());
}
} // namespace Marlin::Tests
