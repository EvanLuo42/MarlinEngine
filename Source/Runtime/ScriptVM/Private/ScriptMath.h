#pragma once

#include "CoreTypes.h"

namespace Marlin
{
/// Runs the Math instruction whose trailing word is *word* (MarlinScript §14.1): *b*, *c* and *d* are the registers
/// of its operands and *result* those of its result. False when the operation does not apply to the operands.
bool ExecuteScriptMath(uint32 word, uint8 *result, const uint8 *b, const uint8 *c, const uint8 *d);
} // namespace Marlin
