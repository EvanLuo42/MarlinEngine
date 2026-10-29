#pragma once

#include "Model.h"

namespace Marlin::Doc
{
/// A C++ module as the job file describes it.
struct CppModuleJob
{
    String Name;
    String Kind;
    String Directory;
    String DocsFile;
    List<String> Headers;
    /// The module's compiler options as one command line, split by Windows rules.
    String Arguments;
};

/// Loads libclang from *path* (Documentation/Architecture/16 §3.1); false if it cannot be loaded or lacks a
/// function MarlinDoc calls.
bool LoadLibClang(StringView path, Reporter &reporter);
void UnloadLibClang();

/// Parses every public header of *job* as one translation unit and adds its declarations to *module*.
bool ExtractCpp(const CppModuleJob &job, StringView root, Module &module, Reporter &reporter);
} // namespace Marlin::Doc
