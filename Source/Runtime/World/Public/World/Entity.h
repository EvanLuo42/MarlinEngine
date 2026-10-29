#pragma once

#include "CoreTypes.h"
#include "Hash/Hash.h"
#include "World/Entity.gen.h"

namespace Marlin
{
/// A handle to an entity of one EntityManager: a slot index and the generation the slot had when the entity was
/// created, so a handle to a destroyed entity never reaches the slot's next occupant. Generation 0 is the null entity.
MSTRUCT()
struct Entity
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Index = 0;

    MPROPERTY()
    uint32 Generation = 0;

    /// The generation of an entity a CommandBuffer has recorded but not created: Index counts its creations.
    static constexpr uint32 DeferredGeneration = ~uint32{0};

    [[nodiscard]] constexpr bool IsNull() const
    {
        return Generation == 0;
    }

    [[nodiscard]] constexpr bool IsDeferred() const
    {
        return Generation == DeferredGeneration;
    }

    constexpr bool operator==(const Entity &) const = default;

    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger((static_cast<uint64>(Generation) << 32) | Index);
    }
};
} // namespace Marlin
