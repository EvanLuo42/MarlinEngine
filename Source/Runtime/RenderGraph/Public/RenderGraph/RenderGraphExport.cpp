#include "RenderGraph/RenderGraphExport.h"

#include "RHI/RhiFormat.h"
#include "RenderGraph/RenderGraphCompiler.h"
#include "RenderGraph/RenderGraphFrame.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
struct FlagName
{
    uint32 Bit = 0;
    const char *Name = nullptr;
};

const FlagName AccessNames[] = {
    {static_cast<uint32>(RhiAccess::IndexBuffer), "IndexBuffer"},
    {static_cast<uint32>(RhiAccess::ShaderResource), "ShaderResource"},
    {static_cast<uint32>(RhiAccess::UnorderedAccess), "UnorderedAccess"},
    {static_cast<uint32>(RhiAccess::RenderTarget), "RenderTarget"},
    {static_cast<uint32>(RhiAccess::DepthStencilRead), "DepthStencilRead"},
    {static_cast<uint32>(RhiAccess::DepthStencilWrite), "DepthStencilWrite"},
    {static_cast<uint32>(RhiAccess::CopySource), "CopySource"},
    {static_cast<uint32>(RhiAccess::CopyDest), "CopyDest"},
    {static_cast<uint32>(RhiAccess::ResolveSource), "ResolveSource"},
    {static_cast<uint32>(RhiAccess::ResolveDest), "ResolveDest"},
    {static_cast<uint32>(RhiAccess::IndirectArguments), "IndirectArguments"},
    {static_cast<uint32>(RhiAccess::AccelerationStructureRead), "AccelerationStructureRead"},
    {static_cast<uint32>(RhiAccess::AccelerationStructureWrite), "AccelerationStructureWrite"},
    {static_cast<uint32>(RhiAccess::ShadingRateSource), "ShadingRateSource"},
    {static_cast<uint32>(RhiAccess::IndirectPreprocessRead), "IndirectPreprocessRead"},
    {static_cast<uint32>(RhiAccess::IndirectPreprocessWrite), "IndirectPreprocessWrite"},
    {static_cast<uint32>(RhiAccess::HostRead), "HostRead"},
    {static_cast<uint32>(RhiAccess::HostWrite), "HostWrite"},
    {static_cast<uint32>(RhiAccess::MarkerWrite), "MarkerWrite"},
};

const FlagName SyncNames[] = {
    {static_cast<uint32>(RhiSync::IndexInput), "IndexInput"},
    {static_cast<uint32>(RhiSync::VertexShading), "VertexShading"},
    {static_cast<uint32>(RhiSync::TaskShading), "TaskShading"},
    {static_cast<uint32>(RhiSync::MeshShading), "MeshShading"},
    {static_cast<uint32>(RhiSync::PixelShading), "PixelShading"},
    {static_cast<uint32>(RhiSync::DepthStencil), "DepthStencil"},
    {static_cast<uint32>(RhiSync::RenderTarget), "RenderTarget"},
    {static_cast<uint32>(RhiSync::ComputeShading), "ComputeShading"},
    {static_cast<uint32>(RhiSync::RayTracing), "RayTracing"},
    {static_cast<uint32>(RhiSync::Copy), "Copy"},
    {static_cast<uint32>(RhiSync::Resolve), "Resolve"},
    {static_cast<uint32>(RhiSync::IndirectArguments), "IndirectArguments"},
    {static_cast<uint32>(RhiSync::AccelerationStructureBuild), "AccelerationStructureBuild"},
    {static_cast<uint32>(RhiSync::AccelerationStructureCopy), "AccelerationStructureCopy"},
    {static_cast<uint32>(RhiSync::ShadingRate), "ShadingRate"},
    {static_cast<uint32>(RhiSync::IndirectPreprocess), "IndirectPreprocess"},
    {static_cast<uint32>(RhiSync::Host), "Host"},
};

/// The names of the bits set in *value*, joined by '|'; empty for none.
String FlagsToString(uint32 value, Span<const FlagName> names)
{
    StringBuilder<256> text;
    for (const FlagName &name : names)
    {
        if ((value & name.Bit) != 0)
        {
            if (text.Length() > 0)
            {
                text.Append('|');
            }
            text.Append(StringView(name.Name));
        }
    }
    return String(text.AsView());
}

String AccessToString(RhiAccess access)
{
    return FlagsToString(static_cast<uint32>(access), AccessNames);
}

String SyncToString(RhiSync sync)
{
    return FlagsToString(static_cast<uint32>(sync), SyncNames);
}

void AddBarriers(const RenderGraphFrame &frame, const RenderGraphBarrierRange &range,
                 List<RenderGraphExportBarrier> &output)
{
    const auto transfer = [](bool transfers, const RhiQueueTransfer &queues) {
        if (!transfers)
        {
            return String();
        }
        StringBuilder<64> text;
        text.Append(StringView(RenderGraphCompiler::GetQueueName(queues.From)));
        text.Append(StringView("->"));
        text.Append(StringView(RenderGraphCompiler::GetQueueName(queues.To)));
        return String(text.AsView());
    };
    for (uint32 index = range.FirstTexture; index < range.FirstTexture + range.TextureCount; ++index)
    {
        const RhiTextureBarrier &barrier = frame.TextureBarriers[index];
        RenderGraphExportBarrier &entry = output.AddDefaulted();
        entry.Resource = static_cast<int32>(frame.TextureBarrierResources[index]);
        entry.SyncBefore = SyncToString(barrier.SyncBefore);
        entry.SyncAfter = SyncToString(barrier.SyncAfter);
        entry.AccessBefore = AccessToString(barrier.AccessBefore);
        entry.AccessAfter = AccessToString(barrier.AccessAfter);
        entry.LayoutBefore = String(StringView(RenderGraphCompiler::GetLayoutName(barrier.LayoutBefore)));
        entry.LayoutAfter = String(StringView(RenderGraphCompiler::GetLayoutName(barrier.LayoutAfter)));
        entry.FirstMip = barrier.Range.FirstMip;
        entry.MipCount = barrier.Range.MipCount;
        entry.FirstSlice = barrier.Range.FirstSlice;
        entry.SliceCount = barrier.Range.SliceCount;
        entry.Discard = barrier.Discard;
        entry.Transfer = transfer(barrier.TransferQueue, barrier.Transfer);
    }
    for (uint32 index = range.FirstBuffer; index < range.FirstBuffer + range.BufferCount; ++index)
    {
        const RhiBufferBarrier &barrier = frame.BufferBarriers[index];
        RenderGraphExportBarrier &entry = output.AddDefaulted();
        entry.Resource = static_cast<int32>(frame.BufferBarrierResources[index]);
        entry.SyncBefore = SyncToString(barrier.SyncBefore);
        entry.SyncAfter = SyncToString(barrier.SyncAfter);
        entry.AccessBefore = AccessToString(barrier.AccessBefore);
        entry.AccessAfter = AccessToString(barrier.AccessAfter);
        entry.Transfer = transfer(barrier.TransferQueue, barrier.Transfer);
    }
    for (uint32 index = range.FirstGlobal; index < range.FirstGlobal + range.GlobalCount; ++index)
    {
        const RhiGlobalBarrier &barrier = frame.GlobalBarriers[index];
        RenderGraphExportBarrier &entry = output.AddDefaulted();
        entry.SyncBefore = SyncToString(barrier.SyncBefore);
        entry.SyncAfter = SyncToString(barrier.SyncAfter);
        entry.AccessBefore = AccessToString(barrier.AccessBefore);
        entry.AccessAfter = AccessToString(barrier.AccessAfter);
    }
}
} // namespace

void BuildRenderGraphExport(const RenderGraphFrame &frame, uint64 graph, RenderGraphExport &output)
{
    output.Graph = graph;
    output.TransientBytes = frame.TransientBytes;
    output.UnaliasedBytes = frame.UnaliasedBytes;
    output.HeapCount = frame.HeapCount;

    // Submissions are numbered in the order they were submitted.
    List<int32> submissionOf;
    submissionOf.Resize(frame.Segments.Count(), -1);
    for (usize position = 0; position < frame.SubmitOrder.Count(); ++position)
    {
        submissionOf[frame.SubmitOrder[position]] = static_cast<int32>(position);
    }

    for (const RenderGraphPass &pass : frame.Passes)
    {
        RenderGraphExportPass &entry = output.Passes.AddDefaulted();
        entry.Name = String(pass.PassName.ToView());
        entry.DeclaredQueue = String(StringView(RenderGraphCompiler::GetQueueName(pass.DeclaredQueue)));
        entry.Queue = String(StringView(RenderGraphCompiler::GetQueueName(pass.Queue)));
        entry.Culled = !pass.Kept;
        entry.Renders = pass.Renders();
        entry.Order = pass.Kept ? static_cast<int32>(pass.Order) : -1;
        entry.Submission = pass.Kept && pass.Segment != RenderGraphNone ? submissionOf[pass.Segment] : -1;
        for (uint32 index = pass.FirstAccess; index < pass.FirstAccess + pass.AccessCount; ++index)
        {
            const RenderGraphAccess &access = frame.Accesses[index];
            RenderGraphExportAccess &accessEntry = entry.Accesses.AddDefaulted();
            accessEntry.Resource = access.Resource;
            accessEntry.Access = AccessToString(access.Access);
            accessEntry.Sync = SyncToString(access.Sync);
            accessEntry.FirstMip = access.Range.FirstMip;
            accessEntry.MipCount = access.Range.MipCount;
            accessEntry.FirstSlice = access.Range.FirstSlice;
            accessEntry.SliceCount = access.Range.SliceCount;
        }
        if (!pass.Kept)
        {
            continue;
        }
        for (const uint32 wait : pass.Waits)
        {
            if (wait != RenderGraphNone)
            {
                entry.Waits.Add(frame.Kept[wait]);
            }
        }
        AddBarriers(frame, pass.Before, entry.BarriersBefore);
        AddBarriers(frame, pass.After, entry.BarriersAfter);
    }

    for (const RenderGraphResource &resource : frame.Resources)
    {
        RenderGraphExportResource &entry = output.Resources.AddDefaulted();
        entry.Name = String(resource.Name);
        entry.Kind = String(StringView(resource.IsTexture() ? "Texture" : "Buffer"));
        entry.Imported = resource.IsExternal();
        entry.Culled = !resource.Used;
        if (resource.IsTexture())
        {
            const RhiTextureDesc &desc = resource.TextureDesc;
            entry.Format = String(StringView(GetFormatInfo(desc.Format).Name));
            entry.Width = desc.Width;
            entry.Height = desc.Height;
            entry.DepthOrArraySize = desc.DepthOrArraySize;
            entry.MipCount = desc.MipCount;
            entry.Size = resource.Size;
        }
        else
        {
            entry.Size = resource.BufferDesc.Size;
        }
        entry.Heap = resource.Heap != RenderGraphNone ? static_cast<int32>(resource.Heap) : -1;
        entry.Offset = resource.Offset;
        const uint32 first = resource.GetFirstUse();
        const uint32 last = resource.GetLastUse();
        entry.FirstPass = first != RenderGraphNone ? static_cast<int32>(frame.Kept[first]) : -1;
        entry.LastPass = last != RenderGraphNone ? static_cast<int32>(frame.Kept[last]) : -1;
    }

    for (const uint32 index : frame.SubmitOrder)
    {
        const RenderGraphSegment &segment = frame.Segments[index];
        RenderGraphExportSubmission &entry = output.Submissions.AddDefaulted();
        entry.Queue = String(StringView(RenderGraphCompiler::GetQueueName(segment.Queue)));
        entry.Epilogue = segment.Epilogue;
        for (uint32 position = segment.FirstPass; position < segment.FirstPass + segment.PassCount; ++position)
        {
            entry.Passes.Add(frame.Kept[frame.SegmentPasses[position]]);
        }
        for (const uint32 wait : segment.Waits)
        {
            if (wait != RenderGraphNone)
            {
                entry.Waits.Add(static_cast<uint32>(submissionOf[frame.GetKept(wait).Segment]));
            }
        }
    }
}
} // namespace Marlin
