#pragma once

#include "LanguageIR/MirModule.h"

namespace Marlin
{
class StringWriter;

[[nodiscard]] LANGUAGEIR_API StringView GetMirOpName(MirOp op);
[[nodiscard]] LANGUAGEIR_API StringView GetMirIntrinsicName(MirIntrinsic intrinsic);
[[nodiscard]] LANGUAGEIR_API StringView GetMirStageName(MirStage stage);
[[nodiscard]] LANGUAGEIR_API StringView GetMirBuiltinName(MirBuiltin builtin);

/// Writes *module* as text, for tests and debugging. Values are numbered in order of definition per function.
LANGUAGEIR_API void PrintMirModule(const MirModule &module, StringWriter &output);
LANGUAGEIR_API void PrintMirFunction(const MirFunction &function, StringWriter &output);
} // namespace Marlin
