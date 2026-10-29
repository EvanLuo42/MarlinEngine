#pragma once

#include "ScriptTestRun.h"
#include "ScriptVM/ScriptSubsystems.h"
#include "World/World.h"

namespace Marlin::Tests
{
/// An engine scope with only a ScriptVMSubsystem, and a world in it whose profile runs the gameplay systems on a
/// fixed clock of 0.5 s. The script source is compiled into the subsystem's VM first, so the world's domain loads it.
struct ScriptWorldRun
{
    explicit ScriptWorldRun(StringView source)
    {
        Engine.SetFilter([](const TypeInfo &type) { return &type == &TypeOf<ScriptVMSubsystem>(); });
        Engine.Initialize();
        Run = MakeUnique<ScriptRun>(source, ScriptDomainOptions{}, false, &Engine.Get<ScriptVMSubsystem>()->GetVM());
        Profile.Name = String("ScriptTest");
        Profile.Sets = {String("CoreSimulationSet"), String("GameplaySet")};
        Profile.Clock = WorldClock::Fixed;
        TheWorld = MakeUnique<World>(Profile, "ScriptTest", &Engine);
    }

    ~ScriptWorldRun()
    {
        TheWorld = nullptr;
        Engine.Deinitialize();
    }

    void RunFrame()
    {
        ++_frame;
        const FrameTime time{_frame, 0.5, 0.5 * static_cast<float64>(_frame)};
        TheWorld->BeginFrame(time);
        TheWorld->FixedUpdate(0.5);
        TheWorld->Update(time);
        TheWorld->EndFrame(time);
    }

    /// The entities with *component*.
    void Find(const ComponentType &component, List<Entity> &entities)
    {
        EntityQueryBase query;
        EntityQueryDesc desc;
        desc.Read.Add(component.GetId());
        query.SetDesc(Move(desc));
        query.Bind(TheWorld->GetEntities());
        query.ForEachChunk([&](ChunkView &chunk) {
            for (const Entity entity : chunk.GetEntities())
            {
                entities.Add(entity);
            }
        });
    }

    SubsystemCollection Engine{TypeOf<EngineSubsystem>()};
    UniquePtr<ScriptRun> Run;
    WorldProfile Profile;
    UniquePtr<World> TheWorld;

private:
    uint64 _frame = 0;
};
} // namespace Marlin::Tests
