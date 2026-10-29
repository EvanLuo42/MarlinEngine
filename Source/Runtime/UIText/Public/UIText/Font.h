#pragma once

#include "Asset/Asset.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "UIText/Font.gen.h"

namespace Marlin
{
/// A font face (Documentation/Architecture/09 §2): the OpenType or TrueType file it comes from, kept whole, and which
/// face of a collection (.ttc) it is. HarfBuzz reads the tables straight from Data; nothing is rasterized ahead.
MCLASS()
class UITEXT_API Font final : public Asset
{
    MGENERATED_BODY()
public:
    /// The sfnt file.
    MPROPERTY()
    FrozenArray<uint8> Data;
    MPROPERTY()
    uint32 FaceIndex = 0;

    Font() = default;
    ~Font() override = default;
    MARLIN_NONCOPYABLE(Font);
};
} // namespace Marlin
