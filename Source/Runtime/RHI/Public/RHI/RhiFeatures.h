#pragma once

#include "String/StringView.h"

/// Names of the optional device features engine code asks for with RhiDevice::HasFeature. The set is open: the
/// shader language's optional features (MarlinLanguage §9.7) use the same names, and backends add their own.
/// The hardware baseline (mesh shaders, ray queries and pipelines, bindless) is not a feature.
namespace Marlin::RhiFeatures
{
/// Reserved textures and buffers mapped tile by tile.
inline constexpr StringView SparseResources = "SparseResources";
/// SetPipeline commands in ExecuteIndirect.
inline constexpr StringView IndirectPipelineSwitch = "IndirectPipelineSwitch";
/// Per-draw, per-primitive and attachment shading rates.
inline constexpr StringView VariableRateShading = "VariableRateShading";
/// 64-bit atomics on R64Uint textures.
inline constexpr StringView ImageAtomic64 = "ImageAtomic64";
inline constexpr StringView ShaderExecutionReorder = "ShaderExecutionReorder";
inline constexpr StringView CooperativeVector = "CooperativeVector";
inline constexpr StringView CooperativeVectorTraining = "CooperativeVectorTraining";
inline constexpr StringView CooperativeMatrix = "CooperativeMatrix";
/// Swapchains in HDR color spaces on at least one display.
inline constexpr StringView HdrOutput = "HdrOutput";
} // namespace Marlin::RhiFeatures
