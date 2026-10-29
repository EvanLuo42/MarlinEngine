#pragma once

#include "CoreTypes.h"
#include "EnumFlags.h"
#include "RHI/RhiHandle.h"
#include "RHI/RhiResources.h"

namespace Marlin
{
/// The work a barrier waits for or blocks, in the style of D3D12 enhanced barriers; RenderGraph tracks the states.
enum class RhiSync : uint32
{
    None = 0,
    IndexInput = 1 << 0,
    VertexShading = 1 << 1,
    TaskShading = 1 << 2,
    MeshShading = 1 << 3,
    PixelShading = 1 << 4,
    DepthStencil = 1 << 5,
    RenderTarget = 1 << 6,
    ComputeShading = 1 << 7,
    RayTracing = 1 << 8,
    Copy = 1 << 9,
    Resolve = 1 << 10,
    IndirectArguments = 1 << 11,
    AccelerationStructureBuild = 1 << 12,
    AccelerationStructureCopy = 1 << 13,
    ShadingRate = 1 << 14,
    /// Generating commands for ExecuteIndirect.
    IndirectPreprocess = 1 << 15,
    /// Reads and writes by the host on mapped memory.
    Host = 1 << 16,
    AllShading = VertexShading | TaskShading | MeshShading | PixelShading | ComputeShading | RayTracing,
    All = 0x7FFFFFFF,
};
MARLIN_ENUM_FLAGS(RhiSync)

enum class RhiAccess : uint32
{
    None = 0,
    IndexBuffer = 1 << 0,
    /// Read through a resource view or a buffer address.
    ShaderResource = 1 << 1,
    UnorderedAccess = 1 << 2,
    RenderTarget = 1 << 3,
    DepthStencilRead = 1 << 4,
    DepthStencilWrite = 1 << 5,
    CopySource = 1 << 6,
    CopyDest = 1 << 7,
    ResolveSource = 1 << 8,
    ResolveDest = 1 << 9,
    IndirectArguments = 1 << 10,
    AccelerationStructureRead = 1 << 11,
    AccelerationStructureWrite = 1 << 12,
    ShadingRateSource = 1 << 13,
    IndirectPreprocessRead = 1 << 14,
    IndirectPreprocessWrite = 1 << 15,
    HostRead = 1 << 16,
    HostWrite = 1 << 17,
    /// Breadcrumbs and timestamp results written by the queue.
    MarkerWrite = 1 << 18,
};
MARLIN_ENUM_FLAGS(RhiAccess)

enum class RhiLayout : uint8
{
    /// The contents are undefined; only valid as a barrier's layout before.
    Undefined,
    /// Any access, at some cost on some GPUs; what textures used by more than one queue at a time are in.
    General,
    Present,
    RenderTarget,
    DepthStencilWrite,
    DepthStencilRead,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDest,
    ResolveSource,
    ResolveDest,
    ShadingRateSource,
};

/// Orders memory accesses with no resource named: the cheapest barrier for buffers.
struct RhiGlobalBarrier
{
    RhiSync SyncBefore = RhiSync::None;
    RhiSync SyncAfter = RhiSync::None;
    RhiAccess AccessBefore = RhiAccess::None;
    RhiAccess AccessAfter = RhiAccess::None;
};

/// Hands a resource from one queue to another: the source queue records the release, then the destination, after
/// waiting for the source's submission, records the acquire. Within one queue family nothing changes owner: the
/// release does the whole barrier and the acquire records nothing.
struct RhiQueueTransfer
{
    RhiQueueType From = RhiQueueType::Graphics;
    RhiQueueType To = RhiQueueType::Graphics;
};

struct RhiBufferBarrier
{
    RhiBuffer Buffer;
    RhiSync SyncBefore = RhiSync::None;
    RhiSync SyncAfter = RhiSync::None;
    RhiAccess AccessBefore = RhiAccess::None;
    RhiAccess AccessAfter = RhiAccess::None;
    uint64 Offset = 0;
    /// Zero for the rest of the buffer.
    uint64 Size = 0;
    bool TransferQueue = false;
    RhiQueueTransfer Transfer;
};

struct RhiTextureBarrier
{
    RhiTexture Texture;
    RhiSync SyncBefore = RhiSync::None;
    RhiSync SyncAfter = RhiSync::None;
    RhiAccess AccessBefore = RhiAccess::None;
    RhiAccess AccessAfter = RhiAccess::None;
    RhiLayout LayoutBefore = RhiLayout::Undefined;
    RhiLayout LayoutAfter = RhiLayout::Undefined;
    RhiSubresourceRange Range;
    /// The contents may be thrown away: LayoutBefore is treated as Undefined.
    bool Discard = false;
    bool TransferQueue = false;
    RhiQueueTransfer Transfer;
};

struct RhiBarriers
{
    Span<const RhiGlobalBarrier> Global;
    Span<const RhiBufferBarrier> Buffers;
    Span<const RhiTextureBarrier> Textures;
};
} // namespace Marlin
