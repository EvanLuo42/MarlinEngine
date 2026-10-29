#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "CoreTypes.h"
#include "WorldPipeline/PrefabConverter.gen.h"

namespace Marlin
{
/// Flattens a Prefab's entities, nested instances and overrides (PrefabFlattener) into an EntityTemplate.
MCLASS()
class WORLDPIPELINE_API PrefabConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "Prefab";
    }

    [[nodiscard]] const TypeInfo &GetRuntimeType() const override;

    [[nodiscard]] uint32 GetVersion() const override
    {
        return 1;
    }

    bool Convert(ConversionContext &context) override;
};
} // namespace Marlin
