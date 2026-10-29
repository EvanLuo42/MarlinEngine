#include "EditorCore/EditorSubsystem.h"

namespace Marlin
{
EditorHostSubsystem::EditorHostSubsystem() = default;
EditorHostSubsystem::~EditorHostSubsystem() = default;

void EditorHostSubsystem::Start()
{
    _subsystems = MakeUnique<SubsystemCollection>(TypeOf<EditorSubsystem>(), Engine::GetSubsystems());
    _subsystems->Initialize();
}

void EditorHostSubsystem::Stop()
{
    if (_subsystems != nullptr)
    {
        _subsystems->Deinitialize();
        _subsystems.Reset();
    }
}

void EditorHostSubsystem::BeginFrame(const FrameTime &time)
{
    if (_subsystems == nullptr)
    {
        return;
    }
    for (Subsystem *subsystem : _subsystems->GetAll())
    {
        static_cast<EditorSubsystem *>(subsystem)->BeginFrame(time);
    }
}

void EditorHostSubsystem::Update(const FrameTime &time)
{
    if (_subsystems == nullptr)
    {
        return;
    }
    for (Subsystem *subsystem : _subsystems->GetAll())
    {
        static_cast<EditorSubsystem *>(subsystem)->Update(time);
    }
}

void EditorHostSubsystem::EndFrame(const FrameTime &time)
{
    if (_subsystems == nullptr)
    {
        return;
    }
    for (Subsystem *subsystem : _subsystems->GetAll())
    {
        static_cast<EditorSubsystem *>(subsystem)->EndFrame(time);
    }
}
} // namespace Marlin
