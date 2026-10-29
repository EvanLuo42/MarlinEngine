#pragma once

#include "Memory/UniquePtr.h"
#include "ScriptVM/ScriptNative.h"

namespace Marlin
{
/// The process's script VM (MarlinScript §6.1): the native function registry and the module
/// images every domain shares. It registers the core library's natives itself.
class SCRIPTVM_API ScriptVM
{
public:
    ScriptVM();
    MARLIN_NONCOPYABLE(ScriptVM);

    [[nodiscard]] ScriptNativeRegistry &GetNatives()
    {
        return _natives;
    }

    [[nodiscard]] const ScriptNativeRegistry &GetNatives() const
    {
        return _natives;
    }

    /// Reads a module and keeps its image; a module of the same package replaces the one before it. Null when the
    /// bytes are not a module this VM reads.
    const ScriptModuleData *AddModule(Span<const uint8> bytes, String &error);
    const ScriptModuleData *AddModule(ScriptModuleData module);

    [[nodiscard]] const ScriptModuleData *FindModule(StringView package) const;

    [[nodiscard]] Span<const UniquePtr<ScriptModuleData>> GetModules() const
    {
        return _modules.AsSpan();
    }

    /// Counts the modules added: a domain that loaded the modules at another count is missing some or has old ones.
    [[nodiscard]] uint64 GetGeneration() const
    {
        return _generation;
    }

private:
    ScriptNativeRegistry _natives;
    List<UniquePtr<ScriptModuleData>> _modules;
    uint64 _generation = 0;
};

/// Registers the natives of the core library, `/Engine/Language/Scripts/Core/` (MarlinScript §7.1, §10.1).
SCRIPTVM_API void RegisterScriptCoreNatives(ScriptNativeRegistry &registry);
} // namespace Marlin
