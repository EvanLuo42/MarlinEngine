#pragma once

#include "CoreTypes.h"
#include "World/Entity.h"
#include "ReplicationTestTypes.gen.h"

namespace Marlin::Tests
{
/// A component with one field of each kind of replication: plain, quantized, an entity, owner-only, initial-only and
/// one the server keeps.
MSTRUCT(Component)
struct TestReplicatedState
{
    MGENERATED_BODY()

    MPROPERTY(Replicated)
    int32 Health = 100;

    MPROPERTY(Replicated, Quantize = 0.01)
    float32 Speed = 0;

    MPROPERTY(Replicated)
    Entity Target;

    MPROPERTY(Replicated, Condition = OwnerOnly)
    int32 Ammo = 0;

    MPROPERTY(Replicated, Condition = InitialOnly)
    uint32 Seed = 0;

    /// Not replicated.
    MPROPERTY()
    int32 ServerOnly = 0;
};

/// A second replicated component, for entities whose set of replicated components changes.
MSTRUCT(Component)
struct TestReplicatedScore
{
    MGENERATED_BODY()

    MPROPERTY(Replicated)
    uint32 Points = 0;
};
} // namespace Marlin::Tests
