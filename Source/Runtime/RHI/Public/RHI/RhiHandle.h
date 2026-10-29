#pragma once

#include "CoreTypes.h"
#include "Hash/Hash.h"

namespace Marlin
{
/// Names an RHI object: a slot of the backend's pool and the generation the slot had when the object was made.
/// Generation zero is never used, so a default handle is invalid; a destroyed object's handle goes stale.
template <typename TTag> struct RhiHandle
{
    uint32 Index = 0;
    uint32 Generation = 0;

    [[nodiscard]] constexpr bool IsValid() const
    {
        return Generation != 0;
    }

    [[nodiscard]] constexpr uint64 GetHash() const
    {
        return HashInteger((uint64{Generation} << 32) | Index);
    }

    constexpr bool operator==(const RhiHandle &) const = default;
};

struct RhiBufferTag;
struct RhiTextureTag;
struct RhiResourceViewTag;
struct RhiSamplerTag;
struct RhiHeapTag;
struct RhiAccelerationStructureTag;
struct RhiPipelineTag;
struct RhiFenceTag;
struct RhiQueryPoolTag;
struct RhiSwapchainTag;
struct RhiCommandSignatureTag;
struct RhiIndirectPipelineSetTag;

using RhiBuffer = RhiHandle<RhiBufferTag>;
using RhiTexture = RhiHandle<RhiTextureTag>;
/// A view in the bindless resource heap; its Index is the descriptor index shaders use.
using RhiResourceView = RhiHandle<RhiResourceViewTag>;
/// A sampler in the bindless sampler heap; its Index is the descriptor index shaders use.
using RhiSampler = RhiHandle<RhiSamplerTag>;
using RhiHeap = RhiHandle<RhiHeapTag>;
using RhiAccelerationStructure = RhiHandle<RhiAccelerationStructureTag>;
using RhiPipeline = RhiHandle<RhiPipelineTag>;
using RhiFence = RhiHandle<RhiFenceTag>;
using RhiQueryPool = RhiHandle<RhiQueryPoolTag>;
using RhiSwapchain = RhiHandle<RhiSwapchainTag>;
using RhiCommandSignature = RhiHandle<RhiCommandSignatureTag>;
using RhiIndirectPipelineSet = RhiHandle<RhiIndirectPipelineSetTag>;
} // namespace Marlin
