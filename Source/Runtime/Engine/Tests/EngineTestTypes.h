#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "String/String.h"
#include "EngineTestTypes.gen.h"

namespace Marlin::Tests
{
/// A scope of its own, so these subsystems never join the engine's.
MCLASS()
class TestScopeSubsystem : public Subsystem
{
    MGENERATED_BODY()
public:
    static List<String> Events;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
};

MCLASS(Dependencies = "TestStorageSubsystem")
class TestRendererSubsystem final : public TestScopeSubsystem
{
    MGENERATED_BODY()
};

MCLASS()
class TestStorageSubsystem final : public TestScopeSubsystem
{
    MGENERATED_BODY()
};

MCLASS()
class TestDisabledSubsystem final : public TestScopeSubsystem
{
    MGENERATED_BODY()
public:
    MFUNCTION()
    static bool ShouldCreate(const SubsystemContext &context);
};

MCLASS(Dependencies = "TestDisabledSubsystem")
class TestNeedsDisabledSubsystem final : public TestScopeSubsystem
{
    MGENERATED_BODY()
};

/// Named to sort first, it still comes after the renderer; that one is disabled and the other not linked at all
/// does not keep it from being created.
MCLASS(OptionalDependencies = "TestRendererSubsystem, TestDisabledSubsystem, TestNotLinkedSubsystem")
class TestAudioSubsystem final : public TestScopeSubsystem
{
    MGENERATED_BODY()
};
} // namespace Marlin::Tests
