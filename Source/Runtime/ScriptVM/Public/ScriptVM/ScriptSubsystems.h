#pragma once

#include "Asset/AssetRef.h"
#include "Asset/Assets.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "ScriptVM/ScriptDomain.h"
#include "ScriptVM/ScriptModuleAsset.h"
#include "World/World.h"
#include "ScriptVM/ScriptSubsystems.gen.h"

namespace Marlin
{
/// Owns the process's ScriptVM: its native registry and the module images every world's domain shares
/// (MarlinScript §6.1).
MCLASS()
class SCRIPTVM_API ScriptVMSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    [[nodiscard]] ScriptVM &GetVM()
    {
        return _vm;
    }

private:
    ScriptVM _vm;
};

/// Loads the script modules of the Engine package and, with a project, the Project package into the ScriptVM
/// (MarlinScript §11.1): as ScriptModuleAssets, from packages when packaged and from GlobalStore in development, where
/// a script edit reloads them. Loading starts at the first frame, once every store is in place; the modules join the
/// VM in package order, a package once those before it have loaded, as a module links to those before it. An empty
/// module, of a package without scripts, is skipped.
MCLASS(Dependencies = "AssetSubsystem, ScriptVMSubsystem")
class SCRIPTVM_API ScriptModuleLoaderSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    ScriptModuleLoaderSubsystem();
    ~ScriptModuleLoaderSubsystem() override;

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;

    /// True once every package has loaded or failed to.
    [[nodiscard]] bool IsSettled() const;

private:
    struct Module;

    /// Adds the loaded modules at the front that have not joined the VM yet.
    void AddReady();
    void OnReloaded(uint32 index);

    ScriptVM *_vm = nullptr;
    bool _started = false;
    List<UniquePtr<Module>> _modules;
};

/// A world's ScriptDomain (MarlinScript §6.1, §9.5): it loads the VM's modules when the world starts, and at each frame
/// boundary moves the domain's clock, resumes waiting tasks and gives the collector its budget. When the VM's modules
/// change, such as when they arrive after the world started or a script edit reloads them, it makes a new domain with
/// all of them at the next frame boundary and has the world rebuild its systems, so new script systems join. The new
/// domain starts over: script statics and tasks do not carry over (hot reload, §12, keeps them; it is not
/// implemented).
MCLASS()
class SCRIPTVM_API ScriptWorldSubsystem final : public WorldSubsystem
{
    MGENERATED_BODY()
public:
    /// Null when the process has no ScriptVMSubsystem.
    [[nodiscard]] ScriptDomain *GetDomain() const
    {
        return _domain.Get();
    }

    /// Counts the domains made: what a domain's functions were found in is gone once it changes.
    [[nodiscard]] uint64 GetDomainGeneration() const
    {
        return _domainGeneration;
    }

    void Initialize(SubsystemContext &context) override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

private:
    /// A new domain with every module of the VM.
    void MakeDomain();

    ScriptVM *_vm = nullptr;
    uint64 _vmGeneration = 0;
    uint64 _domainGeneration = 0;
    UniquePtr<ScriptDomain> _domain;
};
} // namespace Marlin
