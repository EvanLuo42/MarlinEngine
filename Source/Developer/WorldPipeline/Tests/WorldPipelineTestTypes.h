#pragma once

#include "CoreTypes.h"
#include "WorldPipelineTestTypes.gen.h"

namespace Marlin::Tests
{
/// A component the prefab tests set from sources and overrides, one field at a time.
MSTRUCT(Component)
struct PrefabTestWheel
{
    MGENERATED_BODY()

    MPROPERTY()
    float32 Radius = 1;

    MPROPERTY()
    int32 Spokes = 12;
};

/// A tag, to check that tags survive flattening.
MSTRUCT(Component, Tag)
struct PrefabTestSpinning
{
    MGENERATED_BODY()
};
} // namespace Marlin::Tests
