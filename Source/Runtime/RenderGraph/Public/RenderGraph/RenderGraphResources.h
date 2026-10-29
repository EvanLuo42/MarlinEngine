#pragma once

#include "CoreTypes.h"
#include "RHI/RhiBarriers.h"
#include "RHI/RhiCommandList.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiResources.h"
#include "Templates/Optional.h"

namespace Marlin
{
inline constexpr uint32 RenderGraphMaxRenderTargets = RhiMaxRenderTargets;

/// Names a resource or view of one graph by its index; valid until the graph has executed.
template <typename TTag> struct RenderGraphHandle
{
    uint32 Index = ~0u;

    [[nodiscard]] constexpr bool IsValid() const
    {
        return Index != ~0u;
    }

    constexpr bool operator==(const RenderGraphHandle &) const = default;
};

struct RenderGraphTextureTag;
struct RenderGraphBufferTag;
struct RenderGraphViewTag;

using RenderGraphTexture = RenderGraphHandle<RenderGraphTextureTag>;
using RenderGraphBuffer = RenderGraphHandle<RenderGraphBufferTag>;
/// A view a pass declared: of a texture, a bindless descriptor (RenderGraphContext::GetDescriptorIndex); of a buffer,
/// a range of device addresses (RenderGraphContext::GetAddress, GetCount).
using RenderGraphView = RenderGraphHandle<RenderGraphViewTag>;

/// A texture that outlives the graph, such as a swapchain image or a persistent atlas. Execute leaves it in
/// FinalLayout and writes that to Layout, so the next graph that imports it starts from there.
struct RenderGraphExternalTexture
{
    RhiTexture Texture;
    /// The layout the texture is in when the graph starts; Undefined when its contents do not matter.
    RhiLayout Layout = RhiLayout::Undefined;
    /// The layout to leave it in, such as Present; Undefined keeps the layout of its last access.
    RhiLayout FinalLayout = RhiLayout::Undefined;
};

/// A buffer that outlives the graph.
struct RenderGraphExternalBuffer
{
    RhiBuffer Buffer;
    /// Created with RhiResourceFlags::Concurrent, so queues share it without ownership transfers.
    bool Concurrent = false;
};

struct RenderGraphTextureViewDesc
{
    /// A count of zero means the rest, as in RhiSubresourceRange.
    RhiSubresourceRange Range;
    /// Unknown uses the texture's format.
    RhiFormat Format = RhiFormat::Unknown;
    /// Unset picks the dimension from the texture and the range.
    Optional<RhiTextureViewDimension> Dimension;
};

/// The part of a buffer a pass gives its shaders, which read it through its device address as a `Ptr` or `Span`
/// (MarlinLanguage §4.5); buffers have no descriptors.
struct RenderGraphBufferViewDesc
{
    uint64 Offset = 0;
    /// Zero for the rest of the buffer.
    uint64 Size = 0;
    /// The size of an element, which RenderGraphContext::GetCount divides by; zero counts bytes.
    uint32 Stride = 0;
};

struct RenderGraphRenderTarget
{
    RenderGraphTexture Texture;
    uint32 Mip = 0;
    uint32 FirstSlice = 0;
    uint32 SliceCount = 1;
    /// Unknown uses the texture's format.
    RhiFormat Format = RhiFormat::Unknown;
    RhiLoadOp Load = RhiLoadOp::Load;
    RhiStoreOp Store = RhiStoreOp::Store;
    /// Unset clears to the texture's optimized clear value.
    Optional<RhiClearValue> Clear;
    RenderGraphTexture ResolveTarget;
    uint32 ResolveMip = 0;
    uint32 ResolveSlice = 0;
};

struct RenderGraphDepthTarget
{
    RenderGraphTexture Texture;
    uint32 Mip = 0;
    uint32 FirstSlice = 0;
    uint32 SliceCount = 1;
    RhiLoadOp DepthLoad = RhiLoadOp::Load;
    RhiStoreOp DepthStore = RhiStoreOp::Store;
    RhiLoadOp StencilLoad = RhiLoadOp::Load;
    RhiStoreOp StencilStore = RhiStoreOp::Store;
    /// Unset clears to the texture's optimized clear value.
    Optional<RhiClearValue> Clear;
    /// Bound for depth testing only: the pass reads the texture in DepthStencilRead.
    bool ReadOnly = false;
};
} // namespace Marlin
