#pragma once

#include "Asset/AssetRef.h"
#include "Containers/FrozenArray.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/String.h"
#include "AssetTestTypes.gen.h"

namespace Marlin::Tests
{
MENUM()
enum class TestFilter : uint8
{
    Nearest,
    Linear,
    Anisotropic,
};

MSTRUCT()
struct TestLayer
{
    MGENERATED_BODY()

    MPROPERTY()
    FrozenString Label;
    MPROPERTY()
    float32 Weight = 0;
    MPROPERTY()
    FrozenArray<uint16> Indices;
};

/// Fields that cannot live in a memory image.
MSTRUCT()
struct TestHeapFields
{
    MGENERATED_BODY()

    MPROPERTY()
    List<int32> Values;
    MPROPERTY()
    String Text;
    MPROPERTY(Transient)
    int32 Cached = 0;
};

MCLASS()
class TestTextureAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    uint32 Width = 0;
    MPROPERTY()
    uint32 Height = 0;
    MPROPERTY()
    FrozenArray<uint8> Pixels;

    static int32 Live;

    TestTextureAsset()
    {
        ++Live;
    }

    ~TestTextureAsset() override
    {
        --Live;
    }

    MARLIN_NONCOPYABLE(TestTextureAsset);
};

MCLASS()
class TestMaterialAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    TestFilter Filter = TestFilter::Nearest;
    MPROPERTY()
    float32 Tint[3] = {};
    MPROPERTY()
    int64 Flags = 0;
    MPROPERTY()
    FrozenArray<TestLayer> Layers;
    MPROPERTY()
    AssetRef<TestTextureAsset> BaseColor;
    MPROPERTY()
    FrozenArray<AssetRef<TestTextureAsset>> Details;
    MPROPERTY()
    SoftAssetRef<TestTextureAsset> Fallback;
    MPROPERTY()
    AssetRef<TestMaterialAsset> Parent;

    MPROPERTY(Transient)
    List<String> RuntimeNotes;
    MPROPERTY(Transient)
    int32 PostLoads = 0;
    MPROPERTY(Transient)
    bool BaseColorReadyAtPostLoad = false;

    TestMaterialAsset() = default;
    ~TestMaterialAsset() override = default;
    MARLIN_NONCOPYABLE(TestMaterialAsset);

    void PostLoad() override
    {
        ++PostLoads;
        BaseColorReadyAtPostLoad = !BaseColor.IsValid() || BaseColor.IsLoaded();
    }
};
} // namespace Marlin::Tests
