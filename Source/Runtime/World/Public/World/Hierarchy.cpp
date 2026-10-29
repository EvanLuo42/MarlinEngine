#include "World/Hierarchy.h"

#include "Concurrency/Jobs.h"
#include "World/EntityManager.h"

namespace Marlin
{
namespace
{
/// Deeper chains, or a loop of parents, are cut here.
constexpr uint32 MaxHierarchyDepth = 256;
} // namespace

void HierarchySystem::Configure(SystemBuilder &builder)
{
    builder.AddQuery(_attached);
    builder.AddQuery(_moved);
    builder.AddQuery(_detached);
    builder.WriteComponent<Children>();
}

void HierarchySystem::Execute(SystemContext &context)
{
    EntityManager &entities = context.GetEntities();
    CommandBuffer &commands = context.GetCommands();
    // Children of parents that have no Children component yet, added in one command each.
    HashMap<Entity, Children> created;
    const auto unlink = [&](Entity parent, Entity child) {
        if (Children *children = entities.GetForWrite<Children>(parent))
        {
            (void)children->Entities.Remove(child);
        }
    };
    const auto link = [&](Entity parent, Entity child) {
        if (!entities.Exists(parent))
        {
            return;
        }
        Children *children = entities.GetForWrite<Children>(parent);
        if (children == nullptr)
        {
            children = &created.FindOrAdd(parent);
        }
        if (!children->Entities.Contains(child))
        {
            children->Entities.Add(child);
        }
    };

    _detached.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const Span<const PreviousParent> previous = chunk.Read<PreviousParent>();
        const Span<const Entity> rows = chunk.GetEntities();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            unlink(previous[row].Value, rows[row]);
            commands.Remove<PreviousParent>(rows[row]);
        }
    });
    _moved.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const Span<const Parent> parents = chunk.Read<Parent>();
        const Span<const PreviousParent> current = chunk.Read<PreviousParent>();
        const Span<const Entity> rows = chunk.GetEntities();
        PreviousParent *previous = nullptr;
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            if (current[row].Value == parents[row].Value)
            {
                continue;
            }
            // Only write, and so mark the column changed, when a parent did change.
            previous = previous != nullptr ? previous : chunk.Write<PreviousParent>().Data();
            unlink(previous[row].Value, rows[row]);
            link(parents[row].Value, rows[row]);
            previous[row].Value = parents[row].Value;
        }
    });
    _attached.ForEachChunk(context.GetVersions(), [&](ChunkView &chunk) {
        const Span<const Parent> parents = chunk.Read<Parent>();
        const Span<const Entity> rows = chunk.GetEntities();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            link(parents[row].Value, rows[row]);
            commands.Add(rows[row], PreviousParent{parents[row].Value});
        }
    });
    for (const auto &[parent, children] : created)
    {
        commands.Add(parent, children);
    }
}

void TransformPropagationSystem::Configure(SystemBuilder &builder)
{
    builder.AddQuery(_unplaced);
    builder.AddQuery(_roots);
    builder.AddQuery(_attached);
}

void TransformPropagationSystem::Watch(const Archetype &archetype)
{
    for (const Watched &watched : _watched)
    {
        if (watched.Target == &archetype)
        {
            return;
        }
    }
    _watched.Add({&archetype, archetype.GetStructureVersion()});
}

bool TransformPropagationSystem::IsStale(EntityManager &entities, uint32 lastVersion)
{
    if (_built != &entities)
    {
        return true;
    }
    const Span<Archetype *const> archetypes = _attached.GetArchetypes();
    if (archetypes.Count() != _builtArchetypes)
    {
        return true;
    }
    for (const Watched &watched : _watched)
    {
        if (watched.Target->GetStructureVersion() != watched.StructureVersion)
        {
            return true;
        }
    }
    // A Parent written in place changes the levels without moving a row.
    const ComponentId parentId = Components::IdOf<Parent>();
    for (const Archetype *archetype : archetypes)
    {
        const auto column = static_cast<uint32>(archetype->FindColumn(parentId));
        for (const Chunk *chunk : archetype->GetChunks())
        {
            if (IsNewerVersion(chunk->GetVersion(column), lastVersion))
            {
                return true;
            }
        }
    }
    return false;
}

void TransformPropagationSystem::Rebuild(EntityManager &entities)
{
    constexpr uint32 Unknown = ~0u;
    constexpr uint32 InProgress = ~0u - 1;
    const ComponentId localId = Components::IdOf<LocalTransform>();
    const ComponentId parentId = Components::IdOf<Parent>();
    const ComponentId worldId = Components::IdOf<WorldTransform>();
    _built = &entities;
    _watched.Clear();
    _attachedIndex.Clear();
    _pending.Clear();
    _pendingParents.Clear();

    const Span<Archetype *const> archetypes = _attached.GetArchetypes();
    _builtArchetypes = archetypes.Count();
    for (const Archetype *archetype : archetypes)
    {
        Watch(*archetype);
        const auto localColumn = static_cast<uint32>(archetype->FindColumn(localId));
        const auto parentColumn = static_cast<uint32>(archetype->FindColumn(parentId));
        const auto worldColumn = static_cast<uint32>(archetype->FindColumn(worldId));
        for (Chunk *chunk : archetype->GetChunks())
        {
            const auto *locals = static_cast<const LocalTransform *>(chunk->GetColumnData(localColumn));
            const auto *parents = static_cast<const Parent *>(chunk->GetColumnData(parentColumn));
            auto *worlds = static_cast<WorldTransform *>(chunk->GetColumnData(worldColumn));
            const Span<const Entity> rows = chunk->GetEntities();
            for (uint32 row = 0; row < chunk->GetCount(); ++row)
            {
                _attachedIndex.Set(rows[row], static_cast<uint32>(_pending.Count()));
                Link &link = _pending.AddDefaulted();
                link.Local = locals + row;
                link.World = worlds + row;
                link.Owner = chunk;
                link.LocalColumn = localColumn;
                link.ParentColumn = parentColumn;
                link.WorldColumn = worldColumn;
                _pendingParents.Add(parents[row].Value);
            }
        }
    }

    // Depth 0 is a parent that is not attached. A chain longer than the limit, or a loop of parents, is cut: the
    // entity at the cut counts as having no parent.
    const auto count = static_cast<uint32>(_pending.Count());
    _pendingDepths.Clear();
    _pendingDepths.Resize(count, Unknown);
    uint32 levelCount = 0;
    for (uint32 i = 0; i < count; ++i)
    {
        _path.Clear();
        uint32 current = i;
        uint32 base = 0;
        for (;;)
        {
            if (_pendingDepths[current] < InProgress)
            {
                base = _pendingDepths[current] + 1;
                break;
            }
            if (_pendingDepths[current] == InProgress || _path.Count() >= MaxHierarchyDepth)
            {
                _pendingParents[_path.Last()] = Entity();
                break;
            }
            _pendingDepths[current] = InProgress;
            _path.Add(current);
            const uint32 *parent = _attachedIndex.Find(_pendingParents[current]);
            if (parent == nullptr)
            {
                break;
            }
            current = *parent;
        }
        const auto length = static_cast<uint32>(_path.Count());
        for (uint32 k = 0; k < length; ++k)
        {
            _pendingDepths[_path[k]] = base + length - 1 - k;
        }
        if (length > 0 && base + length > levelCount)
        {
            levelCount = base + length;
        }
    }

    // Bucket by depth, then point each link at its parent's world transform.
    while (_levels.Count() < levelCount)
    {
        _levels.AddDefaulted();
    }
    for (Level &level : _levels)
    {
        level.Links.Clear();
        level.Written.Clear();
    }
    _levelCount = levelCount;
    _pendingSlots.Clear();
    _pendingSlots.Resize(count);
    for (uint32 i = 0; i < count; ++i)
    {
        List<Link> &links = _levels[_pendingDepths[i]].Links;
        _pendingSlots[i] = static_cast<uint32>(links.Count());
        links.Add(_pending[i]);
    }
    for (uint32 i = 0; i < count; ++i)
    {
        Link &link = _levels[_pendingDepths[i]].Links[_pendingSlots[i]];
        if (const uint32 *parent = _attachedIndex.Find(_pendingParents[i]))
        {
            link.ParentLink = _pendingSlots[*parent];
            link.ParentWorld = _pending[*parent].World;
            continue;
        }
        const EntityLocation location = entities.GetLocation(_pendingParents[i]);
        if (location.Chunk == nullptr)
        {
            continue;
        }
        const Archetype &archetype = location.Chunk->GetArchetype();
        const int32 column = archetype.FindColumn(worldId);
        if (column < 0)
        {
            continue;
        }
        const auto *worlds =
            static_cast<const WorldTransform *>(location.Chunk->GetColumnData(static_cast<uint32>(column)));
        link.ParentWorld = worlds + location.Row;
        link.ParentChunk = location.Chunk;
        link.ParentWorldColumn = static_cast<uint32>(column);
        Watch(archetype);
    }
    for (uint32 depth = 0; depth < _levelCount; ++depth)
    {
        _levels[depth].Written.Resize(_levels[depth].Links.Count());
    }
}

void TransformPropagationSystem::Execute(SystemContext &context)
{
    EntityManager &entities = context.GetEntities();
    const QueryVersions &versions = context.GetVersions();
    // WorldTransform is computed, never authored (templates leave it out): an entity with a LocalTransform gets one,
    // its local transform for now; the next run computes it, since adding it stamps the chunk's columns.
    _unplaced.ForEachChunk(versions, [&](ChunkView &chunk) {
        const Span<const LocalTransform> locals = chunk.Read<LocalTransform>();
        const Span<const Entity> rows = chunk.GetEntities();
        for (uint32 row = 0; row < chunk.GetCount(); ++row)
        {
            const LocalTransform &local = locals[row];
            context.GetCommands().Add<WorldTransform>(rows[row], {local.Translation, local.Rotation, local.Scale});
        }
    });
    context.ParallelForEach(_roots, [](const LocalTransform &local, WorldTransform &world) {
        world = {local.Translation, local.Rotation, local.Scale};
    });

    const bool rebuilt = IsStale(entities, versions.LastVersion);
    if (rebuilt)
    {
        Rebuild(entities);
    }
    // A level's parents are all in the level above, roots or other entities, so each level runs in parallel once
    // the one above is done. An entity whose parent vanished takes its local transform once, at the rebuild.
    const uint32 last = versions.LastVersion;
    for (uint32 depth = 0; depth < _levelCount; ++depth)
    {
        Level &level = _levels[depth];
        const Level *above = depth > 0 ? &_levels[depth - 1] : nullptr;
        Jobs::ParallelFor(static_cast<uint32>(level.Links.Count()), 64, [&](uint32 begin, uint32 end) {
            for (uint32 i = begin; i < end; ++i)
            {
                const Link &link = level.Links[i];
                bool write = IsNewerVersion(link.Owner->GetVersion(link.LocalColumn), last) ||
                             IsNewerVersion(link.Owner->GetVersion(link.ParentColumn), last);
                if (link.ParentLink != NoLink)
                {
                    write = write || above->Written[link.ParentLink] != 0;
                }
                else if (link.ParentChunk != nullptr)
                {
                    write = write || IsNewerVersion(link.ParentChunk->GetVersion(link.ParentWorldColumn), last);
                }
                else
                {
                    write = write || rebuilt;
                }
                level.Written[i] = write ? 1 : 0;
                if (!write)
                {
                    continue;
                }
                const LocalTransform &local = *link.Local;
                if (link.ParentWorld != nullptr)
                {
                    *link.World = link.ParentWorld->Combine(local);
                }
                else
                {
                    *link.World = {local.Translation, local.Rotation, local.Scale};
                }
            }
        });
    }
    // Levels share chunks, so the written columns are stamped here rather than from the jobs.
    for (uint32 depth = 0; depth < _levelCount; ++depth)
    {
        const Level &level = _levels[depth];
        const Chunk *stamped = nullptr;
        for (usize i = 0; i < level.Links.Count(); ++i)
        {
            const Link &link = level.Links[i];
            if (level.Written[i] != 0 && link.Owner != stamped)
            {
                link.Owner->SetVersion(link.WorldColumn, versions.Version);
                stamped = link.Owner;
            }
        }
    }
}
} // namespace Marlin
