#pragma once

#include "LanguageIR/MirTarget.h"
#include "ScriptVM/ScriptModuleData.h"

namespace Marlin
{
/// The name the script target registers under (MarlinScript §6).
inline constexpr StringView ScriptTargetName = "script";

/// Registers the script target, which writes MIR as a script bytecode module.
SCRIPTBACKEND_API void RegisterScriptTarget(MirTargetRegistry &registry);

/// Compiles *module*, legalized for the script target, into *result*; false with *errors* when it cannot.
SCRIPTBACKEND_API bool CompileScriptModule(MirModule &module, const MirTargetOptions &options,
                                           ScriptModuleData &result, List<String> &errors);
} // namespace Marlin
