#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "World/Component.h"
#include "Replication/ReplicationSchema.gen.h"

MARLIN_DECLARE_LOG_CATEGORY(REPLICATION_API, LogReplication, Info, Warning);

namespace Marlin
{
/// Marks an entity the server replicates; the server gives it a network id the first time it sees it, which the
/// client keeps in the same component.
MSTRUCT(Component)
struct NetworkIdentity
{
    MGENERATED_BODY()

    /// Zero until the server assigns one.
    MPROPERTY()
    uint32 Id = 0;
};

/// The connection that owns the entity: its OwnerOnly fields go to that connection alone, and it is always relevant
/// there.
MSTRUCT(Component)
struct NetOwner
{
    MGENERATED_BODY()

    /// A ServerReplicator connection id; zero is no connection.
    MPROPERTY()
    uint32 Connection = 0;
};

/// How a connection weighs the entity: beyond Range metres of the connection's viewer it is not sent at all (zero
/// means everywhere), and Priority scales how fast it climbs the queue when bandwidth is short.
MSTRUCT(Component)
struct NetRelevancy
{
    MGENERATED_BODY()

    MPROPERTY()
    float32 Range = 0;

    MPROPERTY()
    float32 Priority = 1;
};

/// How one leaf of a replicated component goes on the wire.
enum class NetFieldEncoding : uint8
{
    Bool,
    Signed,
    Unsigned,
    Float32,
    Float64,
    /// A float sent as a whole number of Quantize steps.
    Quantized,
    /// An entity sent as its network id; zero for none or an entity the client does not know.
    Entity,
};

/// A number, enum, bool, float or entity at the bottom of a replicated field, with its options from
/// `MPROPERTY(Replicated, Quantize=0.01, Condition=OwnerOnly|InitialOnly)`.
struct NetLeafField
{
    uint32 Offset = 0;
    uint32 Size = 0;
    NetFieldEncoding Encoding = NetFieldEncoding::Unsigned;
    float64 Quantize = 0;
    /// Sent only to the connection in the entity's NetOwner.
    bool OwnerOnly = false;
    /// Sent only when the entity first reaches a connection.
    bool InitialOnly = false;
};

/// A component with Replicated fields, flattened into leaves.
struct ReplicatedComponent
{
    ComponentId Component = InvalidComponentId;
    /// The same in every process that links the same components: snapshots name components by it.
    uint32 NetIndex = 0;
    List<NetLeafField> Fields;
    /// Each leaf of a default-constructed component, the baseline of an entity new to a connection.
    List<uint64> Defaults;
};

/// The replicated components of the process, built on first use from reflection: every `MSTRUCT(Component)` with a
/// `MPROPERTY(Replicated)` field.
namespace ReplicationSchema
{
[[nodiscard]] REPLICATION_API Span<const ReplicatedComponent *const> GetAll();
/// Null if *component* has no replicated field.
[[nodiscard]] REPLICATION_API const ReplicatedComponent *Find(ComponentId component);
[[nodiscard]] REPLICATION_API const ReplicatedComponent *FromNetIndex(uint32 index);

/// A leaf as 64 bits: integers sign-extended, floats by their bits or, quantized, as a step count, entities as
/// Index | Generation << 32.
[[nodiscard]] REPLICATION_API uint64 ReadLeaf(const NetLeafField &field, const void *component);
REPLICATION_API void WriteLeaf(const NetLeafField &field, void *component, uint64 value);
} // namespace ReplicationSchema
} // namespace Marlin
