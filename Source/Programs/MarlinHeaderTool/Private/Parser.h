#pragma once

#include "Model.h"

#include <string>
#include <string_view>
#include <vector>

namespace Marlin::HeaderTool
{
/// Reads the reflection markup of one file. Only marked declarations are understood; everything else is skipped by
/// balancing brackets, and markup the parser cannot follow is an error rather than a guess.
bool ParseFile(const std::string &path, std::string_view text, ParsedFile &result, std::vector<Diagnostic> &errors);
} // namespace Marlin::HeaderTool
