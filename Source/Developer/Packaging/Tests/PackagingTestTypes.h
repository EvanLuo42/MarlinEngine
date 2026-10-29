#pragma once

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Compression/CompressionCodec.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "PackagingTestTypes.gen.h"

namespace Marlin::Tests
{
/// Packaged as plain data.
MCLASS(SourceIsRuntime)
class TestPackagedAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    int32 Value = 0;
    MPROPERTY()
    AssetRef<TestPackagedAsset> Next;

    TestPackagedAsset() = default;
    ~TestPackagedAsset() override = default;
    MARLIN_NONCOPYABLE(TestPackagedAsset);
};

/// Packaged as GPU data, which the test's matrix gives another format.
MCLASS(SourceIsRuntime, PackageClass = "GpuTexture")
class TestGpuAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    FrozenArray<int32> Texels;

    TestGpuAsset() = default;
    ~TestGpuAsset() override = default;
    MARLIN_NONCOPYABLE(TestGpuAsset);
};

/// Run-length coding, so the packaging tests compress without linking a codec module.
MCLASS()
class TestRleCodec final : public CompressionCodec
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "TestRle";
    }
    bool Compress(Span<const uint8> input, int32 level, List<uint8> &output) const override;
    bool Decompress(Span<const uint8> input, Span<uint8> output) const override;
};
} // namespace Marlin::Tests
