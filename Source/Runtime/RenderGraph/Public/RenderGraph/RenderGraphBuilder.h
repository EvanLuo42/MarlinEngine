#pragma once

#include "CoreTypes.h"
#include "Memory/MemoryOps.h"
#include "Memory/UniquePtr.h"
#include "RHI/RhiCommandList.h"
#include "RHI/RhiResources.h"
#include "RHI/RhiShaderTypes.h"
#include "RenderGraph/RenderGraphResources.h"
#include "String/Name.h"
#include "String/StringView.h"
#include "Templates/Result.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
class RenderGraphBuilder;
class RenderGraphContext;
class RenderGraphSubsystem;
class RhiDevice;
struct RenderGraphFrame;

/// A pass's execute function and data, kept in the frame's arena until the graph has executed.
struct RenderGraphPassFunction
{
    void (*Invoke)(void *body, void *data, RenderGraphContext &context) = nullptr;
    void (*Destroy)(void *body, void *data) = nullptr;
    void *Body = nullptr;
    void *Data = nullptr;
};

/// Declares what one pass reads and writes, inside AddPass's setup function.
class RENDERGRAPH_API RenderGraphPassBuilder
{
public:
    /// An access to a range of *texture*; ShaderResource and UnorderedAccess accesses get a bindless view.
    RenderGraphView Texture(RenderGraphTexture texture, RhiAccess access, const RenderGraphTextureViewDesc &view = {});

    RenderGraphView ReadTexture(RenderGraphTexture texture, const RenderGraphTextureViewDesc &view = {})
    {
        return Texture(texture, RhiAccess::ShaderResource, view);
    }

    RenderGraphView WriteTexture(RenderGraphTexture texture, const RenderGraphTextureViewDesc &view = {})
    {
        return Texture(texture, RhiAccess::UnorderedAccess, view);
    }

    /// An UnorderedAccess view of a range of *texture* that the pass writes in full without reading, such as a compute
    /// pass's output: what the range held before is discarded, so a transient texture needs no earlier writer.
    RenderGraphView OverwriteTexture(RenderGraphTexture texture, const RenderGraphTextureViewDesc &view = {});

    /// An access to all of *buffer*; ShaderResource and UnorderedAccess accesses get a view whose address and count
    /// the pass reads with RenderGraphContext::GetAddress and GetCount.
    RenderGraphView Buffer(RenderGraphBuffer buffer, RhiAccess access, const RenderGraphBufferViewDesc &view = {});

    RenderGraphView ReadBuffer(RenderGraphBuffer buffer, const RenderGraphBufferViewDesc &view = {})
    {
        return Buffer(buffer, RhiAccess::ShaderResource, view);
    }

    RenderGraphView WriteBuffer(RenderGraphBuffer buffer, const RenderGraphBufferViewDesc &view = {})
    {
        return Buffer(buffer, RhiAccess::UnorderedAccess, view);
    }

    /// An UnorderedAccess view of *buffer* whose earlier contents the pass does not read: what it held before is
    /// discarded, so a transient buffer needs no earlier writer. The pass may write only part of it, such as an append
    /// list whose readers stop at its count.
    RenderGraphView OverwriteBuffer(RenderGraphBuffer buffer, const RenderGraphBufferViewDesc &view = {});

    /// Binds the next render target; a pass with targets executes inside BeginRendering and EndRendering, with the
    /// viewport and scissor set to the render area.
    void RenderTarget(const RenderGraphRenderTarget &target);
    void RenderTarget(RenderGraphTexture texture, RhiLoadOp load, RhiStoreOp store = RhiStoreOp::Store);
    void DepthTarget(const RenderGraphDepthTarget &target);
    void DepthTarget(RenderGraphTexture texture, RhiLoadOp load, RhiStoreOp store = RhiStoreOp::Store);
    void ShadingRate(RenderGraphTexture texture);
    /// The rendered area; the first target's mip size by default. A pass with a render area and no targets
    /// rasterizes without attachments, such as one that writes its results with atomics.
    void SetRenderArea(uint32 width, uint32 height);
    void SetViewMask(uint32 viewMask);

    /// The stages that use the pass's views; by default the raster stages for a pass with targets, compute and ray
    /// tracing otherwise.
    void SetShaderStages(RhiSync stages);
    /// Keeps the pass when nothing reads what it writes, such as one with effects outside the graph.
    void NeverCull();

private:
    friend class RenderGraphBuilder;

    RenderGraphView AddTextureAccess(RenderGraphTexture texture, RhiAccess access,
                                     const RenderGraphTextureViewDesc &view, bool reads);
    RenderGraphView AddBufferAccess(RenderGraphBuffer buffer, RhiAccess access, const RenderGraphBufferViewDesc &view,
                                    bool reads);

    RenderGraphPassBuilder(RenderGraphFrame &frame, uint32 pass) : _frame(frame), _pass(pass)
    {
    }

    RenderGraphFrame &_frame;
    uint32 _pass;
};

/// What a pass's execute function records with, on a job of its own.
class RENDERGRAPH_API RenderGraphContext
{
public:
    RenderGraphContext(RhiCommandList &list, const RenderGraphFrame &frame, uint32 pass)
        : List(list), _frame(frame), _pass(pass)
    {
    }
    MARLIN_NONCOPYABLE(RenderGraphContext);

    RhiCommandList &List;

    [[nodiscard]] RhiDevice &GetDevice() const;
    [[nodiscard]] Name GetPassName() const;
    [[nodiscard]] RhiQueueType GetQueue() const;
    [[nodiscard]] RhiTexture GetTexture(RenderGraphTexture texture) const;
    [[nodiscard]] RhiBuffer GetBuffer(RenderGraphBuffer buffer) const;
    [[nodiscard]] uint64 GetBufferAddress(RenderGraphBuffer buffer) const;
    [[nodiscard]] RhiResourceView GetView(RenderGraphView view) const;

    /// The index shaders use for the texture view *view* in the resource heap.
    [[nodiscard]] uint32 GetDescriptorIndex(RenderGraphView view) const
    {
        return GetView(view).Index;
    }

    /// The device address of the buffer view *view*: where a shader's `Ptr` or `Span` to it points (MarlinLanguage §4.5).
    [[nodiscard]] uint64 GetAddress(RenderGraphView view) const;
    /// The elements of the buffer view *view*: its size, or the rest of the buffer, over its stride.
    [[nodiscard]] uint32 GetCount(RenderGraphView view) const;
    /// The buffer view *view* as a shader's `Span<T>` (MarlinLanguage §4.5).
    [[nodiscard]] ShaderSpan GetSpan(RenderGraphView view) const
    {
        return {.Pointer = GetAddress(view), .Length = GetCount(view)};
    }

private:
    const RenderGraphFrame &_frame;
    uint32 _pass;
};

/// One frame's graph (Documentation/Architecture/07 §2). Declare resources and passes in order; Execute culls the
/// passes nothing uses, derives barriers and queue synchronization, places transient resources in aliased heap
/// memory, records the passes on jobs and submits them per queue. Execute runs once, on the render submit thread;
/// pass execute functions run in parallel and must not share mutable state.
class RENDERGRAPH_API RenderGraphBuilder
{
public:
    explicit RenderGraphBuilder(RenderGraphSubsystem &subsystem);
    ~RenderGraphBuilder();
    MARLIN_NONCOPYABLE(RenderGraphBuilder);

    /// A texture that lives within the frame, in transient heap memory; usage follows from how passes access it.
    [[nodiscard]] RenderGraphTexture CreateTexture(StringView name, const RhiTextureDesc &desc);
    [[nodiscard]] RenderGraphBuffer CreateBuffer(StringView name, const RhiBufferDesc &desc);
    /// *texture* must stay alive until Execute returns, which updates its state.
    [[nodiscard]] RenderGraphTexture ImportTexture(StringView name, RenderGraphExternalTexture &texture);
    [[nodiscard]] RenderGraphBuffer ImportBuffer(StringView name, RenderGraphExternalBuffer &buffer);

    [[nodiscard]] const RhiTextureDesc &GetDesc(RenderGraphTexture texture) const;
    [[nodiscard]] const RhiBufferDesc &GetDesc(RenderGraphBuffer buffer) const;

    /// Adds a pass on *queue*: *setup(RenderGraphPassBuilder &)* declares its accesses now, *execute(RenderGraphContext
    /// &)* records it during Execute. Compute and Copy passes run on their own queues unless r.RenderGraph.AsyncCompute
    /// or r.RenderGraph.AsyncCopy is off.
    template <typename TSetup, typename TExecute>
    void AddPass(StringView name, RhiQueueType queue, TSetup &&setup, TExecute &&execute)
    {
        using TBody = RemoveCvRef<TExecute>;
        RenderGraphPassBuilder builder = BeginPass(name, queue);
        setup(builder);
        RenderGraphPassFunction function;
        function.Body =
            ConstructAt(static_cast<TBody *>(Allocate(sizeof(TBody), alignof(TBody))), Forward<TExecute>(execute));
        function.Invoke = [](void *body, void * /*data*/, RenderGraphContext &context) {
            (*static_cast<TBody *>(body))(context);
        };
        if constexpr (!IsTriviallyDestructible<TBody>)
        {
            function.Destroy = [](void *body, void * /*data*/) { DestroyAt(static_cast<TBody *>(body)); };
        }
        EndPass(builder, function);
    }

    /// The same with pass data: *setup(RenderGraphPassBuilder &, TData &)* fills it, such as with the views it
    /// declares, and *execute(const TData &, RenderGraphContext &)* reads it.
    template <typename TData, typename TSetup, typename TExecute>
    const TData &AddPass(StringView name, RhiQueueType queue, TSetup &&setup, TExecute &&execute)
    {
        using TBody = RemoveCvRef<TExecute>;
        TData *data = ConstructAt(static_cast<TData *>(Allocate(sizeof(TData), alignof(TData))));
        RenderGraphPassBuilder builder = BeginPass(name, queue);
        setup(builder, *data);
        RenderGraphPassFunction function;
        function.Body =
            ConstructAt(static_cast<TBody *>(Allocate(sizeof(TBody), alignof(TBody))), Forward<TExecute>(execute));
        function.Data = data;
        function.Invoke = [](void *body, void *passData, RenderGraphContext &context) {
            (*static_cast<TBody *>(body))(*static_cast<const TData *>(passData), context);
        };
        if constexpr (!IsTriviallyDestructible<TBody> || !IsTriviallyDestructible<TData>)
        {
            function.Destroy = [](void *body, void *passData) {
                DestroyAt(static_cast<TBody *>(body));
                DestroyAt(static_cast<TData *>(passData));
            };
        }
        EndPass(builder, function);
        return *data;
    }

    /// Compiles, records and submits the graph. Fails if transient memory cannot be allocated or a submission fails.
    Result<void, RhiError> Execute();

private:
    RenderGraphPassBuilder BeginPass(StringView name, RhiQueueType queue);
    void EndPass(const RenderGraphPassBuilder &builder, const RenderGraphPassFunction &function);
    [[nodiscard]] void *Allocate(usize size, usize alignment);

    RenderGraphSubsystem &_subsystem;
    UniquePtr<RenderGraphFrame> _frame;
};
} // namespace Marlin
