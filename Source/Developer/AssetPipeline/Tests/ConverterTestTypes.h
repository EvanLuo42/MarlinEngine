#pragma once

#include "Asset/AssetRef.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/ConversionStore.h"
#include "Containers/HashMap.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "ConverterTestTypes.gen.h"

namespace Marlin::Tests
{
/// Converted by reading the source straight into it.
MCLASS(SourceIsRuntime)
class TestTableAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    FrozenArray<int32> Values;
    MPROPERTY()
    AssetRef<TestTableAsset> Next;

    TestTableAsset() = default;
    ~TestTableAsset() override = default;
    MARLIN_NONCOPYABLE(TestTableAsset);
};

/// The source of TestWordsAsset: words to upper-case.
MSTRUCT()
struct TestWordsSource
{
    MGENERATED_BODY()

    MPROPERTY()
    String Text;
    MPROPERTY()
    AssetRef<TestTableAsset> Table;
};

MCLASS()
class TestWordsAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    FrozenString Text;
    MPROPERTY()
    int32 TableSum = 0;

    TestWordsAsset() = default;
    ~TestWordsAsset() override = default;
    MARLIN_NONCOPYABLE(TestWordsAsset);
};

/// Upper-cases the words and sums the values of the table they reference, which it reads as a dependency.
MCLASS()
class TestWordsConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "TestWordsSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<TestWordsAsset>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Convert(ConversionContext &context) override;

    static int32 Runs;
};

/// Wraps TestWordsConverter and appends a mark, the way another module changes how the engine converts a type.
MCLASS(Replaces = "TestWordsConverter", Priority = 10)
class TestMarkedWordsConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "TestWordsSource";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<TestWordsAsset>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }
    bool Convert(ConversionContext &context) override;

    MFUNCTION()
    static bool ShouldCreate(const ExtensionContext &context);

    static bool Enabled;
};
} // namespace Marlin::Tests
