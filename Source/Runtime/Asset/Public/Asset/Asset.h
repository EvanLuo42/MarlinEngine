#pragma once

#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Misc/Guid.h"
#include "Reflection/TypeOf.h"
#include "Asset/Asset.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(ASSET_API, LogAsset, Info, Warning);

namespace Marlin
{
namespace Private
{
struct AssetLoader;
} // namespace Private

/// The base of run-time assets: read-only data converted from a source object and loaded from a block. Fields
/// other than Transient ones must be image-safe (Documentation/Architecture/04 §3.1).
MCLASS(Abstract)
class ASSET_API Asset
{
    MGENERATED_BODY()
public:
    Asset() = default;
    virtual ~Asset() = default;
    MARLIN_NONCOPYABLE(Asset);

    /// The Guid of the source object it was converted from.
    [[nodiscard]] const Guid &GetGuid() const
    {
        return _guid;
    }

    /// Runs on a job once the block is fixed up and every asset it references has loaded.
    virtual void PostLoad()
    {
    }

    /// The source object's !Name.
    MPROPERTY()
    FrozenString Name;

private:
    friend struct Private::AssetLoader;

    Guid _guid;
};
} // namespace Marlin
