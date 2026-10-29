#pragma once

#include "Containers/List.h"
#include "LanguageIR/MirModule.h"
#include "String/String.h"

namespace Marlin
{
/// Checks that *module* is well formed: every block ends in exactly one terminator, phis and variables come first
/// in their blocks, every operand is defined in a dominating position, operand types fit their operations and
/// merge blocks belong to the function. Problems are appended to *errors*; false if there were any.
LANGUAGEIR_API bool VerifyMirModule(const MirModule &module, List<String> &errors);
LANGUAGEIR_API bool VerifyMirFunction(const MirFunction &function, List<String> &errors);
} // namespace Marlin
