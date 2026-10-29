#pragma once

#include "CoreTypes.h"
#include "Engine/Engine.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "EditorCore/EditorSubsystem.gen.h"

namespace Marlin
{
/// Lives as long as the editor process (Documentation/Architecture/01 §3): the asset indexer client, source control,
/// undo, the editor UI. The editor scope starts once every engine subsystem has (01 §2, step 10) and stops before any
/// of them does, so dependencies may name any engine subsystem. Each frame hook runs inside the engine's hook of the
/// same name; every engine Update ends before any EndFrame, so the editor records its UI in Update.
MCLASS()
class EDITORCORE_API EditorSubsystem : public Subsystem
{
    MGENERATED_BODY()
public:
    virtual void BeginFrame(const FrameTime &time)
    {
    }

    virtual void Update(const FrameTime &time)
    {
    }

    virtual void EndFrame(const FrameTime &time)
    {
    }
};

/// Hosts the editor scope inside the engine's: creates the EditorSubsystems on Start, runs their frame hooks and
/// destroys them on Stop.
MCLASS()
class EDITORCORE_API EditorHostSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    EditorHostSubsystem();
    ~EditorHostSubsystem() override;

    void Start() override;
    void Stop() override;
    void BeginFrame(const FrameTime &time) override;
    void Update(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

    /// The editor-scope subsystems, or null outside Start and Stop.
    [[nodiscard]] SubsystemCollection *GetSubsystems() const
    {
        return _subsystems.Get();
    }

private:
    UniquePtr<SubsystemCollection> _subsystems;
};

namespace Editor
{
/// The editor-scope subsystem of type T, or null.
template <typename T> [[nodiscard]] T *GetSubsystem()
{
    const EditorHostSubsystem *host = Engine::GetSubsystem<EditorHostSubsystem>();
    const SubsystemCollection *subsystems = host != nullptr ? host->GetSubsystems() : nullptr;
    return subsystems != nullptr ? subsystems->Get<T>() : nullptr;
}
} // namespace Editor
} // namespace Marlin
