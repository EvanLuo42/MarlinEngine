#pragma once

#include "CoreTypes.h"
#include "Math/MathUtility.h"
#include "RenderGraph/RenderGraphBuilder.h"
#include "RHI/RhiDevice.h"

/// What the scene view features build their passes with; for SceneRenderer's own feature files.
namespace Marlin::SceneFeatureHelpers
{
/// Groups per row of a two-dimensional dispatch: the least maxMeshWorkGroupCount and maxComputeWorkGroupCount allow.
inline constexpr uint32 GroupsPerRow = 65535;

inline RenderGraphBufferViewDesc Structured(uint32 stride)
{
    return {0, 0, stride};
}

/// A transient buffer of at least 16 bytes.
inline RenderGraphBuffer CreateBuffer(RenderGraphBuilder &graph, StringView name, uint64 size)
{
    RhiBufferDesc desc;
    desc.Size = Max<uint64>(size, 16);
    desc.DebugName = name;
    return graph.CreateBuffer(name, desc);
}

/// Fills *buffer*, made by CreateBuffer with *size*, with copies of *value*, zero by default.
inline void AddClear(RenderGraphBuilder &graph, StringView name, RenderGraphBuffer buffer, uint64 size,
                     uint32 value = 0)
{
    graph.AddPass(
        name, RhiQueueType::Graphics,
        [&](RenderGraphPassBuilder &pass) { (void)pass.Buffer(buffer, RhiAccess::CopyDest); },
        [buffer, size, value](RenderGraphContext &context) {
            context.List.FillBuffer(context.GetBuffer(buffer), 0, Max<uint64>(size, 16), value);
        });
}

/// Indirect arguments, zeroed first: until the shader of the pass that writes them has loaded, that pass does not
/// run, and the dispatch or draw they feed must then do nothing rather than read what the memory held.
inline RenderGraphBuffer CreateArguments(RenderGraphBuilder &graph, StringView name)
{
    const RenderGraphBuffer buffer = CreateBuffer(graph, name, 16);
    AddClear(graph, name, buffer, 16);
    return buffer;
}
} // namespace Marlin::SceneFeatureHelpers
