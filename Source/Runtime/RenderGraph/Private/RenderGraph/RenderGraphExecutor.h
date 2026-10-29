#pragma once

#include "CoreTypes.h"
#include "RHI/RhiResources.h"
#include "Templates/Result.h"

namespace Marlin
{
class RhiCommandList;
class RhiGpuProfiler;
class RenderGraphSubsystem;
struct RenderGraphBarrierRange;
struct RenderGraphFrame;
struct RenderGraphRecordJob;

/// Records a compiled, allocated frame's passes on jobs, a command list per run of passes, and submits the
/// segments in order with their waits.
struct RENDERGRAPH_API RenderGraphExecutor
{
    static Result<void, RhiError> Run(RenderGraphFrame &frame, RenderGraphSubsystem &subsystem);

private:
    static void Record(const RenderGraphRecordJob &job);
    static void RecordPass(RhiCommandList &list, RenderGraphFrame &frame, RenderGraphSubsystem &subsystem, uint32 pass);
    static void RecordBarriers(RhiCommandList &list, const RenderGraphFrame &frame,
                               const RenderGraphBarrierRange &range);
    static void BeginRendering(RhiCommandList &list, const RenderGraphFrame &frame, uint32 pass);
    static Result<void, RhiError> Submit(RenderGraphFrame &frame, RenderGraphSubsystem &subsystem);
};
} // namespace Marlin
