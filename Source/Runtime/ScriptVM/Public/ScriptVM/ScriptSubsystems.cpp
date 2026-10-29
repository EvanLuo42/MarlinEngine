#include "ScriptVM/ScriptSubsystems.h"

#include "Engine/Engine.h"

namespace Marlin
{
struct ScriptModuleLoaderSubsystem::Module
{
    String Package;
    AssetRef<ScriptModuleAsset> Asset;
    AssetReloadId Reload = 0;
    /// Loaded or failed.
    bool Settled = false;
    /// In the VM, or empty and skipped.
    bool Added = false;
};

ScriptModuleLoaderSubsystem::ScriptModuleLoaderSubsystem() = default;
ScriptModuleLoaderSubsystem::~ScriptModuleLoaderSubsystem() = default;

void ScriptModuleLoaderSubsystem::Initialize(SubsystemContext &context)
{
    ScriptVMSubsystem *vm = context.Collection->Get<ScriptVMSubsystem>();
    _vm = vm != nullptr ? &vm->GetVM() : nullptr;
}

void ScriptModuleLoaderSubsystem::Deinitialize()
{
    for (const UniquePtr<Module> &module : _modules)
    {
        if (module->Reload != 0)
        {
            Assets::UnwatchReload(module->Reload);
        }
    }
    _modules.Clear();
    _vm = nullptr;
}

void ScriptModuleLoaderSubsystem::BeginFrame(const FrameTime &time)
{
    (void)time;
    if (_started || _vm == nullptr)
    {
        return;
    }
    _started = true;
    List<StringView> packages;
    packages.Add("Engine");
    if (Engine::GetEnvironment().HasProject())
    {
        packages.Add("Project");
    }
    for (const StringView package : packages)
    {
        const auto index = static_cast<uint32>(_modules.Count());
        auto module = MakeUnique<Module>();
        module->Package = String(package);
        module->Asset = Assets::Load<ScriptModuleAsset>(GetScriptModuleGuid(package), IoPriority::Critical);
        AssetSlot *slot = module->Asset.GetSlot();
        _modules.Add(Move(module));
        if (slot == nullptr)
        {
            _modules.Last()->Settled = true;
            continue;
        }
        Assets::OnCompleted(*slot, [this, index](AssetSlot &) {
            if (index < _modules.Count())
            {
                _modules[index]->Settled = true;
                AddReady();
            }
        });
        _modules.Last()->Reload = Assets::WatchReload(*slot, [this, index](AssetSlot &) { OnReloaded(index); });
    }
    AddReady();
}

bool ScriptModuleLoaderSubsystem::IsSettled() const
{
    if (!_started)
    {
        return false;
    }
    for (const UniquePtr<Module> &module : _modules)
    {
        if (!module->Settled)
        {
            return false;
        }
    }
    return true;
}

void ScriptModuleLoaderSubsystem::AddReady()
{
    for (const UniquePtr<Module> &module : _modules)
    {
        if (!module->Settled)
        {
            return;
        }
        if (module->Added)
        {
            continue;
        }
        module->Added = true;
        const ScriptModuleAsset *asset = module->Asset.Get();
        if (asset == nullptr)
        {
            MARLIN_LOG(LogScript, Error, "The {0} scripts did not load", module->Package);
            continue;
        }
        if (asset->Bytecode.IsEmpty())
        {
            continue;
        }
        String error;
        const ScriptModuleData *loaded = _vm->AddModule(asset->Bytecode.AsSpan(), error);
        if (loaded == nullptr)
        {
            MARLIN_LOG(LogScript, Error, "The {0} scripts cannot be read: {1}", module->Package, error);
        }
        else
        {
            MARLIN_LOG(LogScript, Info, "Loaded the {0} scripts: {1} functions, {2} types", module->Package,
                       loaded->Functions.Count(), loaded->Types.Count());
        }
    }
}

void ScriptModuleLoaderSubsystem::OnReloaded(uint32 index)
{
    if (index >= _modules.Count() || !_modules[index]->Added)
    {
        return;
    }
    const Module &module = *_modules[index];
    const ScriptModuleAsset *asset = module.Asset.Get();
    if (asset == nullptr || asset->Bytecode.IsEmpty())
    {
        return;
    }
    // A module of the same package replaces the old one where it was, so the order holds.
    String error;
    if (_vm->AddModule(asset->Bytecode.AsSpan(), error) == nullptr)
    {
        MARLIN_LOG(LogScript, Error, "The reloaded {0} scripts cannot be read: {1}", module.Package, error);
        return;
    }
    MARLIN_LOG(LogScript, Info, "Reloaded the {0} scripts", module.Package);
}

void ScriptWorldSubsystem::Initialize(SubsystemContext &context)
{
    ScriptVMSubsystem *vm = context.Collection->Get<ScriptVMSubsystem>();
    if (vm == nullptr)
    {
        return;
    }
    _vm = &vm->GetVM();
    MakeDomain();
}

void ScriptWorldSubsystem::Deinitialize()
{
    _domain = nullptr;
    _vm = nullptr;
}

void ScriptWorldSubsystem::MakeDomain()
{
    _domain = MakeUnique<ScriptDomain>(*_vm);
    _domain->SetWorld(&GetWorld());
    _vmGeneration = _vm->GetGeneration();
    ++_domainGeneration;
    for (const UniquePtr<ScriptModuleData> &module : _vm->GetModules())
    {
        String error;
        if (!_domain->Load(*module, error))
        {
            MARLIN_LOG(LogScript, Error, "World {0} cannot load a script module: {1}", GetWorld().GetName(),
                       error.AsView());
        }
    }
}

void ScriptWorldSubsystem::BeginFrame(const FrameTime &time)
{
    if (_vm != nullptr && _vm->GetGeneration() != _vmGeneration)
    {
        MakeDomain();
        // The modules may declare systems the scheduler does not have yet; it adds them at the next frame boundary.
        World &world = GetWorld();
        world.SetProfile(world.GetProfile());
    }
    if (_domain != nullptr)
    {
        _domain->AdvanceTime(time.Delta);
        _domain->SetDeltaTime(time.Delta);
        _domain->BeginFrame();
    }
}

void ScriptWorldSubsystem::EndFrame(const FrameTime &time)
{
    (void)time;
    if (_domain != nullptr)
    {
        _domain->RunReady();
    }
}
} // namespace Marlin
