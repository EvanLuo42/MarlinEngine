#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Math/Quaternion.h"
#include "Math/Vector.h"
#include "Math/WorldPosition.h"
#include "Memory/Allocator.h"
#include "World/Entity.h"
#include "World/EntityManager.h"
#include "World/EntityQuery.h"
#include "World/System.h"
#include "World/Hierarchy.gen.h"

namespace Marlin
{
/// The entity this one is attached to (Documentation/Architecture/05 §4). Set it, or remove it to detach; Children
/// follows at the next PostPhysics phase.
MSTRUCT(Component)
struct Parent
{
    MGENERATED_BODY()

    MPROPERTY()
    Entity Value;
};

/// The entities attached to this one, kept by HierarchySystem from their Parent: read it, never write it. May hold
/// entities destroyed since the last update.
MSTRUCT(Component)
struct Children
{
    MGENERATED_BODY()

    MPROPERTY()
    List<Entity, InlineAllocator<4>> Entities;
};

/// The parent HierarchySystem last linked the entity under, so it can unlink it when Parent changes.
MSTRUCT(Component)
struct PreviousParent
{
    MGENERATED_BODY()

    MPROPERTY()
    Entity Value;
};

/// Position, rotation and scale relative to the parent, or to the world for an entity without one. The translation
/// is in double precision so a root anywhere in a large world stays exact (Documentation/Architecture/05 §6).
MSTRUCT(Component)
struct LocalTransform
{
    MGENERATED_BODY()

    MPROPERTY()
    WorldPosition Translation;

    MPROPERTY()
    Quaternion Rotation = Quaternion::Identity;

    MPROPERTY()
    Vector3 Scale = Vector3::One;
};

/// Where the entity is in the world, computed by TransformPropagationSystem from LocalTransform and the hierarchy.
/// Rendering subtracts the camera cell's origin and converts to float at extraction.
MSTRUCT(Component)
struct WorldTransform
{
    MGENERATED_BODY()

    MPROPERTY()
    WorldPosition Translation;

    MPROPERTY()
    Quaternion Rotation = Quaternion::Identity;

    MPROPERTY()
    Vector3 Scale = Vector3::One;

    /// The world transform of a child at *local* relative to this one.
    [[nodiscard]] WorldTransform Combine(const LocalTransform &local) const
    {
        const Vector3 offset(static_cast<float32>(local.Translation.X), static_cast<float32>(local.Translation.Y),
                             static_cast<float32>(local.Translation.Z));
        return {Translation + Rotation.Rotate(offset * Scale), Rotation * local.Rotation, Scale * local.Scale};
    }
};

/// Keeps Children and PreviousParent in step with Parent: links entities that gained or changed a parent and unlinks
/// those that lost one.
MCLASS(Phase = PostPhysicsPhase, Sets = (CoreSimulationSet))
class WORLD_API HierarchySystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    EntityQuery<Read<Parent>, Without<PreviousParent>> _attached;
    EntityQuery<Changed<Parent>, Read<Parent>, Write<PreviousParent>> _moved;
    EntityQuery<Read<PreviousParent>, Without<Parent>> _detached;
};

/// Computes WorldTransform (Documentation/Architecture/05 §3): roots in parallel by chunk, then the attached entities
/// a hierarchy level at a time, each level in parallel. Only what changed is written, so an unmoved WorldTransform
/// chunk keeps its version and Changed<WorldTransform> skips it: a root when its LocalTransform chunk changed, an
/// attached entity when its own LocalTransform or Parent chunk changed or its parent was written. The levels are
/// kept between runs and rebuilt only when a Parent is written or rows move in the chunks they point into.
MCLASS(Phase = PostPhysicsPhase, Sets = (CoreSimulationSet))
class WORLD_API TransformPropagationSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    static constexpr uint32 NoLink = ~0u;

    /// An attached entity: where its values are and where its parent's world transform comes from.
    struct Link
    {
        const LocalTransform *Local = nullptr;
        WorldTransform *World = nullptr;
        Chunk *Owner = nullptr;
        uint32 LocalColumn = 0;
        uint32 ParentColumn = 0;
        uint32 WorldColumn = 0;
        /// The parent's WorldTransform; null if it has none, then the entity's world transform is its local one.
        const WorldTransform *ParentWorld = nullptr;
        /// The parent's index in the level above, or NoLink if the parent is not attached.
        uint32 ParentLink = NoLink;
        /// For a parent that is not attached: its chunk and WorldTransform column, whose version tells if it moved.
        const Chunk *ParentChunk = nullptr;
        uint32 ParentWorldColumn = 0;
    };

    struct Level
    {
        List<Link> Links;
        /// Whether each link was written this run, for the level below.
        List<uint8> Written;
    };

    /// An archetype the levels point into, and its structure version when they were built.
    struct Watched
    {
        const Archetype *Target = nullptr;
        uint32 StructureVersion = 0;
    };

    [[nodiscard]] bool IsStale(EntityManager &entities, uint32 lastVersion);
    void Rebuild(EntityManager &entities);
    void Watch(const Archetype &archetype);

    /// Entities placed but not yet computed, such as those a template made: they get a WorldTransform.
    EntityQuery<Read<LocalTransform>, Without<WorldTransform>> _unplaced;
    EntityQuery<Changed<LocalTransform>, Read<LocalTransform>, Write<WorldTransform>, Without<Parent>> _roots;
    EntityQuery<Read<LocalTransform>, Read<Parent>, Write<WorldTransform>> _attached;
    List<Level> _levels;
    uint32 _levelCount = 0;
    EntityManager *_built = nullptr;
    usize _builtArchetypes = 0;
    List<Watched> _watched;
    // Scratch for Rebuild.
    HashMap<Entity, uint32> _attachedIndex;
    List<Link> _pending;
    List<Entity> _pendingParents;
    List<uint32> _pendingDepths;
    List<uint32> _pendingSlots;
    List<uint32> _path;
};
} // namespace Marlin
