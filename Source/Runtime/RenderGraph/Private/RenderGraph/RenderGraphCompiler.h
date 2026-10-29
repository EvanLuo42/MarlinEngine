#pragma once

#include "CoreTypes.h"
#include "RHI/RhiBarriers.h"
#include "String/Name.h"

namespace Marlin
{
struct RenderGraphExport;
struct RenderGraphFrame;

struct RenderGraphCompileOptions
{
    bool AsyncCompute = true;
    bool AsyncCopy = true;
    bool Culling = true;
    Name CaptureFirst;
    Name CaptureLast;
};

/// Turns a declared graph into kept passes in order, per-subresource barriers, cross-queue waits and submissions.
namespace RenderGraphCompiler
{
/// Everything but the RHI handles in the barriers, which ResolveBarrierHandles fills in once resources exist.
RENDERGRAPH_API void Compile(RenderGraphFrame &frame, const RenderGraphCompileOptions &options);
RENDERGRAPH_API void ResolveBarrierHandles(RenderGraphFrame &frame);

[[nodiscard]] RENDERGRAPH_API const char *GetQueueName(RhiQueueType queue);
[[nodiscard]] RENDERGRAPH_API const char *GetLayoutName(RhiLayout layout);
} // namespace RenderGraphCompiler

/// Fills *output* from a compiled frame; *graph* is RenderGraphSubsystem::GetGraphCount before it executed.
RENDERGRAPH_API void BuildRenderGraphExport(const RenderGraphFrame &frame, uint64 graph, RenderGraphExport &output);
} // namespace Marlin
