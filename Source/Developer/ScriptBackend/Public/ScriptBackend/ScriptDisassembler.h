#pragma once

#include "ScriptVM/ScriptModuleData.h"

namespace Marlin
{
class StringWriter;

/// Writes *module* as text: its tables, then each function's instructions with their safepoint maps and lines, for
/// tests and debugging.
SCRIPTBACKEND_API void DisassembleScriptModule(const ScriptModuleData &module, StringWriter &output);
} // namespace Marlin
