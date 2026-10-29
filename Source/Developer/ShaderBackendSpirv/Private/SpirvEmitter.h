#pragma once

#include "Containers/List.h"
#include "LanguageIR/MirTarget.h"
#include "String/String.h"

namespace Marlin
{
/// Writes *module*, legalized for the SPIR-V target, as SPIR-V 1.6 words for Vulkan 1.4 with VK_EXT_descriptor_heap:
/// every entry point, bindless resources through the descriptor heaps, the [Uniform] parameter block as push data (§11.3).
/// Types and constants SPIR-V needs are added to *module*.
bool EmitSpirvModule(MirModule &module, const MirTargetOptions &options, List<uint32> &words, List<String> &errors);
} // namespace Marlin
