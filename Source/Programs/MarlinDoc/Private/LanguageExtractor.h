#pragma once

#include "Model.h"

namespace Marlin::Doc
{
/// A library under Language/, such as `Scripts.Core`.
struct LanguageLibraryJob
{
    String Name;
    /// `mscript`, `mshader` or `mcommon`.
    String Language;
    String Directory;
    List<String> Files;
};

/// Parses the files of *job* with LanguageFrontend and adds their declarations to *module*
/// (Documentation/Architecture/16 §3.2).
bool ExtractLanguage(const LanguageLibraryJob &job, StringView root, Module &module, Reporter &reporter);
} // namespace Marlin::Doc
