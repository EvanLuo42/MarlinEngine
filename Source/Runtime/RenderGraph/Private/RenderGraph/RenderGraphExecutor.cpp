#include "RenderGraph/RenderGraphExecutor.h"

#include "Concurrency/Jobs.h"
#include "Diagnostics/AssertFormat.h"
#include "RHI/RhiDevice.h"
#include "RHI/RhiGpuProfiler.h"
#include "RHI/RhiSubsystem.h"
#include "RenderGraph/RenderGraphCompiler.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "RenderGraph/RenderGraphLog.h"
#include "RenderGraph/RenderGraphSubsystem.h"
#include "String/String.h"

namespace Marlin
{
Result<void, RhiError> RenderGraphExecutor::Run(RenderGraphFrame &frame, RenderGraphSubsystem &subsystem)
{
    // Profiler zone names are looked up here, once per pass, so the recording jobs take no lock for them.
    frame.ZoneNames.Clear();
    frame.ZoneNames.Resize(frame.Kept.Count());
    for (uint32 order = 0; order < frame.Kept.Count(); ++order)
    {
        const RenderGraphPass &pass = frame.GetKept(order);
        if (pass.Function.Invoke != nullptr && subsystem._profilers[static_cast<uint32>(pass.Queue)] != nullptr)
        {
            frame.ZoneNames[order] = subsystem.GetZoneName(pass.PassName);
        }
    }
    // Each segment's passes are recorded PassesPerList at a time, by jobs of their own into lists of their own,
    // which the segment submits together.
    List<RenderGraphRecordJob> &jobs = frame.RecordJobs;
    jobs.Clear();
    for (uint32 index = 0; index < frame.Segments.Count(); ++index)
    {
        RenderGraphSegment &segment = frame.Segments[index];
        segment.FirstList = static_cast<uint32>(frame.CommandLists.Count());
        for (uint32 first = 0; first < segment.PassCount;)
        {
            const uint32 perList = RenderGraphSubsystem::PassesPerList > 0 ? RenderGraphSubsystem::PassesPerList : 1;
            RhiCommandList *list = frame.Device.BeginCommandList(segment.Queue);
            if (list == nullptr)
            {
                MARLIN_FATALF("No command list to record the graph into");
            }
            RenderGraphRecordJob &job = jobs.AddDefaulted();
            job.Frame = &frame;
            job.Subsystem = &subsystem;
            job.Segment = index;
            job.FirstPass = segment.FirstPass + first;
            job.PassCount = segment.PassCount - first < perList ? segment.PassCount - first : perList;
            job.List = static_cast<uint32>(frame.CommandLists.Count());
            frame.CommandLists.Add(list);
            ++segment.ListCount;
            first += job.PassCount;
        }
    }
    List<JobDecl> &decls = frame.RecordDecls;
    decls.Clear();
    decls.Reserve(jobs.Count());
    for (RenderGraphRecordJob &job : jobs)
    {
        decls.Add({[](void *argument) { Record(*static_cast<const RenderGraphRecordJob *>(argument)); }, &job,
                   JobPriority::High, true});
    }
    JobCounter counter;
    Jobs::Run(decls.AsSpan(), &counter);
    Jobs::Wait(counter);
    const Result<void, RhiError> submitted = Submit(frame, subsystem);
    if (submitted.IsError())
    {
        return submitted;
    }
    // Imported textures start the next graph in the layout this one left them in.
    for (const RenderGraphResource &resource : frame.Resources)
    {
        if (resource.ExternalTexture != nullptr && resource.Used)
        {
            resource.ExternalTexture->Layout = resource.EndLayout;
        }
    }
    for (UniquePtr<RhiGpuProfiler> &profiler : subsystem._profilers)
    {
        if (profiler != nullptr)
        {
            profiler->Collect();
        }
    }
    return {};
}

void RenderGraphExecutor::Record(const RenderGraphRecordJob &job)
{
    RenderGraphFrame &frame = *job.Frame;
    RhiCommandList &list = *frame.CommandLists[job.List];
    for (uint32 index = job.FirstPass; index < job.FirstPass + job.PassCount; ++index)
    {
        RecordPass(list, frame, *job.Subsystem, frame.SegmentPasses[index]);
    }
}

void RenderGraphExecutor::RecordPass(RhiCommandList &list, RenderGraphFrame &frame, RenderGraphSubsystem &subsystem,
                                     uint32 passIndex)
{
    const RenderGraphPass &pass = frame.GetKept(passIndex);
    RecordBarriers(list, frame, pass.Before);
    if (pass.Function.Invoke != nullptr)
    {
        RhiGpuProfiler *profiler = subsystem._profilers[static_cast<uint32>(pass.Queue)].Get();
        RenderGraphContext context(list, frame, frame.Kept[passIndex]);
        const uint32 zone = profiler != nullptr ? profiler->BeginZone(list, frame.ZoneNames[passIndex]) : ~0u;
        list.BeginMarker(pass.PassName);
        if (pass.Renders())
        {
            BeginRendering(list, frame, passIndex);
        }
        pass.Function.Invoke(pass.Function.Body, pass.Function.Data, context);
        if (pass.Renders())
        {
            list.EndRendering();
        }
        if (profiler != nullptr)
        {
            profiler->EndZone(list, zone);
        }
        list.EndMarker();
    }
    RecordBarriers(list, frame, pass.After);
}

void RenderGraphExecutor::RecordBarriers(RhiCommandList &list, const RenderGraphFrame &frame,
                                         const RenderGraphBarrierRange &range)
{
    if (range.IsEmpty())
    {
        return;
    }
    RhiBarriers barriers;
    barriers.Global = frame.GlobalBarriers.AsSpan().Slice(range.FirstGlobal, range.GlobalCount);
    barriers.Buffers = frame.BufferBarriers.AsSpan().Slice(range.FirstBuffer, range.BufferCount);
    barriers.Textures = frame.TextureBarriers.AsSpan().Slice(range.FirstTexture, range.TextureCount);
    list.Barrier(barriers);
}

void RenderGraphExecutor::BeginRendering(RhiCommandList &list, const RenderGraphFrame &frame, uint32 passIndex)
{
    const RenderGraphPass &pass = frame.GetKept(passIndex);
    RhiRenderTarget targets[RenderGraphMaxRenderTargets];
    for (uint32 index = 0; index < pass.TargetCount; ++index)
    {
        const RenderGraphRenderTarget &source = frame.Targets[pass.FirstTarget + index];
        const RenderGraphResource &resource = frame.Resources[source.Texture.Index];
        RhiRenderTarget &target = targets[index];
        target.Texture = resource.Texture;
        target.Mip = source.Mip;
        target.FirstSlice = source.FirstSlice;
        target.SliceCount = source.SliceCount;
        target.Format = source.Format;
        target.Load = source.Load;
        target.Store = source.Store;
        target.Clear = source.Clear.HasValue() ? source.Clear.Value() : resource.TextureDesc.OptimizedClear;
        if (source.ResolveTarget.IsValid())
        {
            target.ResolveTarget = frame.Resources[source.ResolveTarget.Index].Texture;
            target.ResolveMip = source.ResolveMip;
            target.ResolveSlice = source.ResolveSlice;
        }
    }
    RhiRenderingDesc desc;
    desc.RenderTargets = Span<const RhiRenderTarget>(targets, pass.TargetCount);
    if (pass.HasDepth)
    {
        const RenderGraphDepthTarget &source = pass.Depth;
        const RenderGraphResource &resource = frame.Resources[source.Texture.Index];
        desc.Depth.Texture = resource.Texture;
        desc.Depth.Mip = source.Mip;
        desc.Depth.FirstSlice = source.FirstSlice;
        desc.Depth.SliceCount = source.SliceCount;
        desc.Depth.DepthLoad = source.DepthLoad;
        desc.Depth.DepthStore = source.DepthStore;
        desc.Depth.StencilLoad = source.StencilLoad;
        desc.Depth.StencilStore = source.StencilStore;
        desc.Depth.Clear = source.Clear.HasValue() ? source.Clear.Value() : resource.TextureDesc.OptimizedClear;
        desc.Depth.ReadOnly = source.ReadOnly;
    }
    if (pass.ShadingRate.IsValid())
    {
        desc.ShadingRate = frame.Resources[pass.ShadingRate.Index].Texture;
    }
    desc.Width = pass.Width;
    desc.Height = pass.Height;
    if (desc.Width == 0 || desc.Height == 0)
    {
        // The size of the first target's mip.
        const bool color = pass.TargetCount > 0;
        const RenderGraphTexture first = color ? frame.Targets[pass.FirstTarget].Texture : pass.Depth.Texture;
        const uint32 mip = color ? frame.Targets[pass.FirstTarget].Mip : pass.Depth.Mip;
        const RhiTextureDesc &texture = frame.Resources[first.Index].TextureDesc;
        desc.Width = texture.Width >> mip > 0 ? texture.Width >> mip : 1;
        desc.Height = texture.Height >> mip > 0 ? texture.Height >> mip : 1;
    }
    desc.ViewMask = pass.ViewMask;
    list.BeginRendering(desc);
    // The whole render area, which the pass may narrow.
    const RhiViewport viewport{0, 0, static_cast<float32>(desc.Width), static_cast<float32>(desc.Height), 0, 1};
    const RhiScissor scissor{0, 0, desc.Width, desc.Height};
    list.SetViewports(Span<const RhiViewport>(&viewport, 1));
    list.SetScissors(Span<const RhiScissor>(&scissor, 1));
}

Result<void, RhiError> RenderGraphExecutor::Submit(RenderGraphFrame &frame, RenderGraphSubsystem &subsystem)
{
    RhiDevice &device = frame.Device;
    RhiFence fences[RhiQueueTypeCount];
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        fences[queue] = device.GetQueueFence(static_cast<RhiQueueType>(queue));
    }
    const uint32 captureFirst =
        frame.CaptureFirst != RenderGraphNone ? frame.GetKept(frame.CaptureFirst).Segment : RenderGraphNone;
    const uint32 captureLast =
        frame.CaptureLast != RenderGraphNone ? frame.GetKept(frame.CaptureLast).Segment : RenderGraphNone;
    bool capturing = false;
    for (usize position = 0; position < frame.SubmitOrder.Count(); ++position)
    {
        const uint32 index = frame.SubmitOrder[position];
        RenderGraphSegment &segment = frame.Segments[index];
        const uint32 queue = static_cast<uint32>(segment.Queue);
        // A pass capture starts with the submission its first pass opens and ends with the one its last closes.
        if (index == captureFirst && subsystem._rhi != nullptr)
        {
            capturing = subsystem._rhi->BeginCapture(frame.GetKept(frame.CaptureFirst).PassName.ToView());
        }
        RhiGpuProfiler *profiler = subsystem._profilers[queue].Get();
        if (segment.Epilogue && profiler != nullptr)
        {
            profiler->ResolveFrame(*frame.CommandLists[segment.FirstList + segment.ListCount - 1]);
        }
        // The queue's first submission waits for the previous graph; the others for the passes they depend on.
        RhiFenceWait waits[RhiQueueTypeCount + 1];
        uint32 waitCount = 0;
        if (segment.First && subsystem._lastGraphValue != 0)
        {
            waits[waitCount++] = {fences[static_cast<uint32>(RhiQueueType::Graphics)], subsystem._lastGraphValue};
        }
        for (uint32 other = 0; other < RhiQueueTypeCount; ++other)
        {
            if (segment.Waits[other] != RenderGraphNone)
            {
                const RenderGraphSegment &signaler = frame.Segments[frame.GetKept(segment.Waits[other]).Segment];
                waits[waitCount++] = {fences[other], signaler.SubmitValue};
            }
        }
        RhiSubmitDesc desc;
        desc.CommandLists =
            Span<RhiCommandList *const>(frame.CommandLists.Data() + segment.FirstList, segment.ListCount);
        desc.Waits = Span<const RhiFenceWait>(waits, waitCount);
        const Result<uint64, RhiError> submitted = device.Submit(segment.Queue, desc);
        if (submitted.IsError())
        {
            MARLIN_LOG(LogRenderGraph, Error, "Submitting the graph to the {0} queue failed: {1}",
                       RenderGraphCompiler::GetQueueName(segment.Queue), GetErrorName(submitted.Error()));
            for (usize later = position + 1; later < frame.SubmitOrder.Count(); ++later)
            {
                const RenderGraphSegment &unsent = frame.Segments[frame.SubmitOrder[later]];
                for (uint32 list = unsent.FirstList; list < unsent.FirstList + unsent.ListCount; ++list)
                {
                    device.DiscardCommandList(frame.CommandLists[list]);
                }
            }
            if (capturing)
            {
                String file;
                (void)subsystem._rhi->EndCapture(file);
            }
            return Failure(submitted.Error());
        }
        segment.SubmitValue = submitted.Value();
        if (segment.Epilogue && profiler != nullptr)
        {
            profiler->FrameSubmitted(segment.SubmitValue);
        }
        if (segment.Epilogue && segment.Queue == RhiQueueType::Graphics)
        {
            subsystem._lastGraphValue = segment.SubmitValue;
        }
        if (index == captureLast && capturing)
        {
            String file;
            (void)subsystem._rhi->EndCapture(file);
            capturing = false;
        }
    }
    return {};
}
} // namespace Marlin
