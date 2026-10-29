#pragma once

#include "Containers/Span.h"
#include "CrashAnalysis/CrashAnalysis.h"
#include "CrashAnalysis/Minidump.h"
#include "String/String.h"

/// The platform's symbol engine, used one analysis at a time.
namespace Marlin::Private
{
/// Loads the debug files of the dump's modules; *images* has each module's image path, or an empty one. Marks the
/// modules of *result* that got symbols.
void LoadSymbols(const Minidump &dump, const CrashAnalysisDesc &desc, Span<const String> images,
                 CrashAnalysisResult &result);
/// Fills the function, file, line and offset of *frame*; leaves them as they are without symbols.
void Symbolize(uint64 address, CrashFrame &frame);
void UnloadSymbols();
} // namespace Marlin::Private
