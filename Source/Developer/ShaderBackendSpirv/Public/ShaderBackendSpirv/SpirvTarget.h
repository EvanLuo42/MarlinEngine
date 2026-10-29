#pragma once

#include "LanguageIR/MirTarget.h"

namespace Marlin
{
/// The name the SPIR-V target registers under.
inline constexpr StringView SpirvTargetName = "spirv";

/// Specialization constant ids that give every stage the descriptor strides of the device's resource and sampler
/// heaps (§11.3), which descriptor heap access chains step by. They are RhiResourceStrideSpecializationId and
/// RhiSamplerStrideSpecializationId; specialization constants of shaders use ids below them.
inline constexpr uint32 SpirvResourceStrideSpecId = 0x7FFF0000u;
inline constexpr uint32 SpirvSamplerStrideSpecId = 0x7FFF0001u;

/// Registers the SPIR-V target: SPIR-V 1.6 for Vulkan 1.4 with VK_EXT_descriptor_heap (§11.3, §11.4).
SHADERBACKENDSPIRV_API void RegisterSpirvTarget(MirTargetRegistry &registry);
} // namespace Marlin
