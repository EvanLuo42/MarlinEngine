#pragma once

#include "Containers/List.h"
#include "ScriptVM/ScriptNative.h"
#include "String/String.h"
#include "World/Component.h"
#include "ScriptVM/ScriptWorld.gen.h"

namespace Marlin
{
/// What every script system writes, so that the scheduler runs the script systems of a phase one at a time: they
/// share their world's ScriptDomain, which runs one VM entry at a time (MarlinScript §11.2).
MSTRUCT()
struct ScriptDomainAccess
{
    MGENERATED_BODY()
};

/// The ECS side of scripts (MarlinScript §11.2): a struct marked `[Component]` registers as a component with a
/// dynamic TypeInfo built from its layout, and a function marked `[System]` as a system type whose systems call it,
/// once per entity its `ref` and `in` component parameters select. Both are process-wide and found by their
/// qualified script names, so every world's domain shares them; a name keeps its first type, and a module that
/// declares it again must give it the same layout.
namespace ScriptEcs
{
/// The attributes the ECS reads from a module.
inline constexpr StringView ComponentAttributeName = "Marlin.ComponentAttribute";
inline constexpr StringView SystemAttributeName = "Marlin.SystemAttribute";

/// Registers the components, then the systems, of *module*. False with *errors* for those it cannot register; the
/// others are registered all the same.
SCRIPTVM_API bool Register(const ScriptModuleData &module, List<String> &errors);
/// The component a script struct of qualified name *name* is registered as; null if none.
[[nodiscard]] SCRIPTVM_API const ComponentType *FindComponent(StringView name);
/// The system type a script function of qualified name *name*, without parameters, is registered as; null if none.
[[nodiscard]] SCRIPTVM_API const TypeInfo *FindSystem(StringView name);
} // namespace ScriptEcs

/// Registers the natives of `/Engine/Language/Scripts/Core/World.mscript`: entities, their components and the time.
SCRIPTVM_API void RegisterScriptWorldNatives(ScriptNativeRegistry &registry);
} // namespace Marlin
