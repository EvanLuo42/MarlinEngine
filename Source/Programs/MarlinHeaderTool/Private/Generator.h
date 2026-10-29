#pragma once

#include "Model.h"

#include <string>
#include <vector>

namespace Marlin::HeaderTool
{
struct GeneratedFile
{
    /// The header as its module includes it, such as `Math/Vector.h`.
    std::string IncludePath;
    /// Unique among all modules: the module name and the include path, with `_` for everything but letters and
    /// digits.
    std::string FileId;
    ParsedFile Parsed;
};

struct ModuleInfo
{
    std::string Name;
    /// `<MODULE>_API`.
    std::string Api;
};

std::string GenerateHeader(const ModuleInfo &module, const GeneratedFile &file);
std::string GenerateSource(const ModuleInfo &module, const GeneratedFile &file);
/// Register_<Module>, which registers every file's types, the module's schema upgrades and its tests.
std::string GenerateModule(const ModuleInfo &module, const std::vector<const GeneratedFile *> &files,
                           const std::vector<UpgradeDecl> &upgrades, const std::vector<TestDecl> &tests);
} // namespace Marlin::HeaderTool
