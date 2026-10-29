#include "World/System.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "World/World.h"

namespace Marlin
{
namespace
{
struct SystemFactories
{
    SpinLock Lock;
    HashMap<const TypeInfo *, SystemFactory> ByType;
};

SystemFactories &GetFactories()
{
    static SystemFactories factories;
    return factories;
}
} // namespace

namespace Systems
{
void RegisterFactory(const TypeInfo &type, SystemFactory factory)
{
    SystemFactories &factories = GetFactories();
    const ScopeLock lock(factories.Lock);
    factories.ByType.Set(&type, factory);
}

SystemFactory FindFactory(const TypeInfo &type)
{
    SystemFactories &factories = GetFactories();
    const ScopeLock lock(factories.Lock);
    const SystemFactory *factory = factories.ByType.Find(&type);
    return factory != nullptr ? *factory : nullptr;
}
} // namespace Systems

void SystemBuilder::Record(SystemAccess::Kind kind, uintptr key, bool write)
{
    for (SystemAccess &access : *_access)
    {
        if (access.What == kind && access.Key == key)
        {
            access.Write = access.Write || write;
            return;
        }
    }
    _access->Add({kind, key, write});
}

void SystemBuilder::AddQuery(EntityQueryBase &query)
{
    query.Bind(_world->GetEntities());
    const EntityQueryDesc &desc = query.GetDesc();
    for (const ComponentId component : desc.Read)
    {
        ReadComponent(component);
    }
    for (const ComponentId component : desc.Changed)
    {
        ReadComponent(component);
    }
    for (const ComponentId component : desc.Write)
    {
        WriteComponent(component);
    }
}

void SystemBuilder::ReadComponent(ComponentId component)
{
    Record(SystemAccess::Kind::Component, component, false);
}

void SystemBuilder::WriteComponent(ComponentId component)
{
    Record(SystemAccess::Kind::Component, component, true);
}

void SystemBuilder::ReadResource(const TypeInfo &type)
{
    (void)_world->GetResources().GetOrAdd(type);
    Record(SystemAccess::Kind::Resource, reinterpret_cast<uintptr>(&type), false);
}

void SystemBuilder::WriteResource(const TypeInfo &type)
{
    (void)_world->GetResources().GetOrAdd(type);
    Record(SystemAccess::Kind::Resource, reinterpret_cast<uintptr>(&type), true);
}

void SystemBuilder::ReadEvents(const TypeInfo &type)
{
    Record(SystemAccess::Kind::Events, reinterpret_cast<uintptr>(&type), false);
}

void SystemBuilder::WriteEvents(const TypeInfo &type)
{
    Record(SystemAccess::Kind::Events, reinterpret_cast<uintptr>(&type), true);
}

EntityManager &SystemContext::GetEntities() const
{
    return _world->GetEntities();
}

ResourceStore &SystemContext::GetResources() const
{
    return _world->GetResources();
}

EventStore &SystemContext::GetEvents() const
{
    return _world->GetEvents();
}

CommandBuffer &SystemContext::GetCommands(const ChunkView &chunk)
{
    const ScopeLock lock(_parallelLock);
    const uint32 ordinal = chunk.GetOrdinal();
    if (_parallelCommands.Count() <= ordinal)
    {
        _parallelCommands.Resize(ordinal + 1);
    }
    UniquePtr<CommandBuffer> &commands = _parallelCommands[ordinal];
    if (commands.Get() == nullptr)
    {
        commands = MakeUnique<CommandBuffer>();
    }
    return *commands;
}
} // namespace Marlin
