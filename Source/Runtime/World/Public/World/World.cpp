#include "World/World.h"

#include "Config/Config.h"
#include "Diagnostics/Log.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"

MARLIN_DEFINE_LOG_CATEGORY(LogWorld);

namespace Marlin
{
namespace
{
/// True if *names* holds the name of *type*.
bool NamesType(const List<String> &names, const TypeInfo &type)
{
    for (const String &name : names)
    {
        if (name.AsView() == type.GetName())
        {
            return true;
        }
    }
    return false;
}

/// Splits a `Sets=(A, B)` metadata value into names.
void ParseSetNames(StringView value, List<StringView> &names)
{
    names.Clear();
    StringView text = value.Trim();
    if (text.StartsWith('(') && text.EndsWith(')'))
    {
        text = text.Substring(1, text.Length() - 2);
    }
    for (const StringView part : text.Split(','))
    {
        if (const StringView name = part.Trim(); !name.IsEmpty())
        {
            names.Add(name);
        }
    }
}
} // namespace

bool WorldProfile::Has(const TypeInfo &set) const
{
    return NamesType(Sets, set);
}

bool WorldProfile::Load(StringView name, WorldProfile &profile)
{
    // The chain from *name* up through its Bases; the sections then apply from the root down.
    List<String> chain;
    String current(name);
    while (!current.IsEmpty())
    {
        if (chain.Contains(current))
        {
            MARLIN_LOG(LogWorld, Warning, "World profile {0} inherits itself through Base", current);
            return false;
        }
        const String section = Format("WorldProfiles.{0}", current);
        bool defined = false;
        Config::ForEachSection(section.AsView(), [&](const MarlinTextObject &, StringView) { defined = true; });
        if (!defined)
        {
            MARLIN_LOG(LogWorld, Warning, "No config layer defines world profile {0}", current);
            return false;
        }
        chain.Add(current);
        WorldProfile own;
        (void)Config::Apply(section.AsView(), TypeOf<WorldProfile>(), &own);
        current = own.Base;
    }
    profile = WorldProfile();
    for (usize i = chain.Count(); i-- > 0;)
    {
        (void)Config::Apply(Format("WorldProfiles.{0}", chain[i]).AsView(), TypeOf<WorldProfile>(), &profile);
    }
    profile.Name = String(name);
    return true;
}

bool WorldContext::HasSet(const TypeInfo &set) const
{
    return Owner != nullptr && Owner->HasSet(set);
}

const WorldProfile &WorldContext::GetProfile() const
{
    return Owner->GetProfile();
}

void WorldSubsystem::Bind(SubsystemContext &context)
{
    _world = static_cast<WorldContext &>(context).Owner;
}

World::World(const WorldProfile &profile, StringView name, SubsystemCollection *outer)
    : _name(name), _profile(&profile), _scheduler(*this),
      _subsystems(TypeOf<WorldSubsystem>(), outer, TypeOf<WorldContext>())
{
    _subsystems.SetFilter([this](const TypeInfo &type) { return AllowsSubsystem(type); });
    ApplyChanges();
}

World::~World()
{
    _subsystems.Deinitialize();
    PlaybackDeferred();
}

void World::SetProfile(const WorldProfile &profile)
{
    _profile = &profile;
    _dirty = true;
}

void World::PushSets(Span<const TypeInfo *const> sets)
{
    _pushedSets.Emplace(sets);
    _dirty = true;
}

void World::PopSets()
{
    if (!_pushedSets.IsEmpty())
    {
        _pushedSets.RemoveAt(_pushedSets.Count() - 1);
        _dirty = true;
    }
}

bool World::HasSet(const TypeInfo &set) const
{
    return _activeSets.Contains(&set);
}

bool World::AllowsSubsystem(const TypeInfo &type) const
{
    const WorldProfileSubsystems &rules = _profile->Subsystems;
    List<StringView> sets;
    ParseSetNames(type.GetMetadata().GetOr("Sets", StringView()), sets);
    // The profile names a subsystem by its type or by one of its sets.
    const auto named = [&](const List<String> &names) {
        if (NamesType(names, type))
        {
            return true;
        }
        for (const StringView set : sets)
        {
            if (names.FindIndex([&](const String &name) { return name.AsView() == set; }) != List<String>::NotFound)
            {
                return true;
            }
        }
        return false;
    };
    if (named(rules.Exclude))
    {
        return false;
    }
    if (named(rules.Include) || sets.IsEmpty())
    {
        return true;
    }
    for (const StringView set : sets)
    {
        const TypeInfo *setType = Reflection::FindType(set);
        if (setType != nullptr && HasSet(*setType))
        {
            return true;
        }
    }
    return false;
}

void World::ApplyChanges()
{
    if (!_dirty)
    {
        return;
    }
    _dirty = false;
    _activeSets.Clear();
    for (const String &name : _profile->Sets)
    {
        const TypeInfo *set = Reflection::FindType(name.AsView());
        if (set == nullptr || !set->GetMetadata().Has("SystemSet"))
        {
            MARLIN_LOG(LogWorld, Warning, "World profile {0} names {1}, which is not a linked system set",
                       _profile->Name, name);
            continue;
        }
        if (!_activeSets.Contains(set))
        {
            _activeSets.Add(set);
        }
    }
    for (const List<const TypeInfo *> &pushed : _pushedSets)
    {
        for (const TypeInfo *set : pushed)
        {
            if (!_activeSets.Contains(set))
            {
                _activeSets.Add(set);
            }
        }
    }

    _scheduler.Build(_activeSets.AsSpan());
    WorldContext context;
    context.Collection = &_subsystems;
    context.Owner = this;
    if (_subsystems.IsInitialized())
    {
        _subsystems.Update(context);
    }
    else
    {
        _subsystems.Initialize(context);
    }
}

void World::BeginFrame(const FrameTime &time)
{
    ApplyChanges();
    _events.Update();
    for (Subsystem *subsystem : _subsystems.GetAll())
    {
        static_cast<WorldSubsystem *>(subsystem)->BeginFrame(time);
    }
    PlaybackDeferred();
}

void World::FixedUpdate(float64 delta)
{
    if (_profile->Clock == WorldClock::Fixed)
    {
        _scheduler.Run(WorldStage::Fixed, delta);
        PlaybackDeferred();
    }
}

void World::Update(const FrameTime &time)
{
    const WorldClock clock = _profile->Clock;
    if (clock == WorldClock::Variable)
    {
        _scheduler.Run(WorldStage::Fixed, time.Delta);
    }
    const bool stopped = clock == WorldClock::Paused || clock == WorldClock::Manual;
    _scheduler.Run(WorldStage::Variable, stopped ? 0.0 : time.Delta);
    PlaybackDeferred();
}

void World::EndFrame(const FrameTime &time)
{
    const Span<Subsystem *const> subsystems = _subsystems.GetAll();
    for (usize i = subsystems.Count(); i-- > 0;)
    {
        static_cast<WorldSubsystem *>(subsystems[i])->EndFrame(time);
    }
    PlaybackDeferred();
}

void World::Defer(FunctionRef<void(CommandBuffer &)> record)
{
    const ScopeLock lock(_deferredLock);
    record(_deferred);
}

void World::PlaybackDeferred()
{
    const ScopeLock lock(_deferredLock);
    _deferred.Playback(_entities);
}

void World::Step(float64 delta)
{
    _scheduler.Run(WorldStage::Fixed, delta);
    PlaybackDeferred();
}
} // namespace Marlin
