#include "HAL/PlatformTime.h"
#include "RHI/RhiFeatures.h"
#include "RHI/RhiGpuProfiler.h"
#include "RHI/RhiUploadAllocator.h"
#include "RHI/RhiUploadQueue.h"
#include "RHI/RhiTestDevice.h"

namespace Marlin::Tests
{
namespace
{
RhiBufferBarrier BufferBarrier(RhiBuffer buffer, RhiSync before, RhiAccess beforeAccess, RhiSync after,
                               RhiAccess afterAccess)
{
    RhiBufferBarrier barrier;
    barrier.Buffer = buffer;
    barrier.SyncBefore = before;
    barrier.AccessBefore = beforeAccess;
    barrier.SyncAfter = after;
    barrier.AccessAfter = afterAccess;
    return barrier;
}

RhiTextureBarrier TextureBarrier(RhiTexture texture, RhiLayout before, RhiLayout after, RhiSync syncBefore,
                                 RhiAccess accessBefore, RhiSync syncAfter, RhiAccess accessAfter)
{
    RhiTextureBarrier barrier;
    barrier.Texture = texture;
    barrier.LayoutBefore = before;
    barrier.LayoutAfter = after;
    barrier.SyncBefore = syncBefore;
    barrier.AccessBefore = accessBefore;
    barrier.SyncAfter = syncAfter;
    barrier.AccessAfter = accessAfter;
    return barrier;
}

void Barrier(RhiCommandList &list, Span<const RhiBufferBarrier> buffers, Span<const RhiTextureBarrier> textures = {})
{
    RhiBarriers barriers;
    barriers.Buffers = buffers;
    barriers.Textures = textures;
    list.Barrier(barriers);
}

void TimeFill(RhiDevice &device, RhiQueueType queue, RhiBuffer scratch)
{
    RhiGpuProfiler profiler(device, queue, 16, 2);
    const uint64 before = PlatformTime::Ticks();
    RhiCommandList *list = device.BeginCommandList(queue);
    const uint32 zone = profiler.BeginZone(*list, "Fill");
    list->FillBuffer(scratch, 0, 16 << 20, 3);
    profiler.EndZone(*list, zone);
    profiler.ResolveFrame(*list);
    RhiCommandList *lists[] = {list};
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    Result<uint64, RhiError> value = device.Submit(queue, submit);
    MARLIN_REQUIRE(value.IsOk());
    profiler.FrameSubmitted(value.Value());
    MARLIN_REQUIRE(device.Wait(device.GetQueueFence(queue), value.Value(), 10000));
    const uint64 after = PlatformTime::Ticks();
    profiler.Collect();

    const Span<const RhiGpuZone> zones = profiler.GetLastFrame();
    MARLIN_REQUIRE(zones.Count() == 1);
    MARLIN_EXPECT(StringView(zones[0].Name) == "Fill" && zones[0].EndTicks >= zones[0].BeginTicks);
    const uint64 slack = PlatformTime::TicksPerSecond() / 100;
    MARLIN_EXPECTF(zones[0].BeginTicks + slack >= before && zones[0].EndTicks <= after + slack,
                   "queue {0}: the zone {1}..{2} is outside the submission {3}..{4}", static_cast<uint32>(queue),
                   zones[0].BeginTicks, zones[0].EndTicks, before, after);
}

void HostBarrier(RhiCommandList &list)
{
    RhiGlobalBarrier global;
    global.SyncBefore = RhiSync::Copy;
    global.AccessBefore = RhiAccess::CopyDest;
    global.SyncAfter = RhiSync::Host;
    global.AccessAfter = RhiAccess::HostRead;
    RhiBarriers barriers;
    barriers.Global = Span<const RhiGlobalBarrier>(&global, 1);
    list.Barrier(barriers);
}
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, Rhi, DeviceMeetsTheBaseline, Gpu)
{
    const RhiDeviceLimits &limits = Device->GetLimits();
    MARLIN_EXPECT(limits.MaxTextureSize2D >= 16384);
    MARLIN_EXPECT(limits.MaxResourceDescriptors >= 500000 && limits.ResourceDescriptorStride > 0);
    MARLIN_EXPECT(limits.MaxSamplerDescriptors >= 2000 && limits.SamplerDescriptorStride > 0);
    MARLIN_EXPECT(limits.ShaderGroupHandleSize > 0 && limits.MaxRayRecursionDepth >= 1);
    MARLIN_EXPECT(limits.MaxMeshOutputPrimitives >= 256 && limits.TimestampFrequency[0] > 0);
    MARLIN_EXPECT(Device->GetBackendName() == Backend->GetName());
    MARLIN_EXPECT(!Device->GetAdapterInfo().Name.IsEmpty());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, BufferCopiesAndFills, Gpu)
{
    constexpr uint32 Count = 1024;
    const RhiBuffer upload = CreateBuffer(Count * 4, RhiBufferUsage::CopySource, RhiMemoryLocation::Upload);
    const RhiBuffer device =
        CreateBuffer(Count * 4, RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest, RhiMemoryLocation::Device);
    const RhiBuffer readback = CreateBuffer(Count * 4, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    auto *source = reinterpret_cast<uint32 *>(Device->GetMappedData(upload));
    MARLIN_REQUIRE(source != nullptr);
    for (uint32 i = 0; i < Count; ++i)
    {
        source[i] = i * 3 + 1;
    }

    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    MARLIN_REQUIRE(list != nullptr);
    list->CopyBuffer(device, 0, upload, 0, Count * 4);
    const RhiBufferBarrier afterCopy =
        BufferBarrier(device, RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy, RhiAccess::CopyDest);
    Barrier(*list, Span<const RhiBufferBarrier>(&afterCopy, 1));
    list->FillBuffer(device, 256 * 4, 256 * 4, 0xABCD1234u);
    const RhiBufferBarrier afterFill =
        BufferBarrier(device, RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy, RhiAccess::CopySource);
    Barrier(*list, Span<const RhiBufferBarrier>(&afterFill, 1));
    list->CopyBuffer(readback, 0, device, 0, Count * 4);
    HostBarrier(*list);
    SubmitAndWait(list);

    const auto *result = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback));
    uint32 wrong = 0;
    for (uint32 i = 0; i < Count; ++i)
    {
        const uint32 expected = i >= 256 && i < 512 ? 0xABCD1234u : i * 3 + 1;
        wrong += result[i] != expected ? 1 : 0;
    }
    MARLIN_EXPECTF(wrong == 0, "{0} of {1} words differ", wrong, Count);
    Device->Destroy(upload);
    Device->Destroy(device);
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, TextureClearsAndCopiesToBuffer, Gpu)
{
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA8Unorm;
    desc.Width = 64;
    desc.Height = 32;
    desc.MipCount = 2;
    desc.Usage = RhiTextureUsage::CopySource | RhiTextureUsage::CopyDest | RhiTextureUsage::ShaderResource;
    desc.DebugName = "ClearTexture";
    Result<RhiTexture, RhiError> created = Device->CreateTexture(desc);
    MARLIN_REQUIRE(created.IsOk());
    const RhiTexture texture = created.Value();
    const RhiBuffer readback = CreateBuffer(64 * 32 * 4 + 32 * 16 * 4, RhiBufferUsage::CopyDest,
                                            RhiMemoryLocation::Readback);

    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    const RhiTextureBarrier toCopyDest = TextureBarrier(texture, RhiLayout::Undefined, RhiLayout::CopyDest,
                                                        RhiSync::None, RhiAccess::None, RhiSync::Copy,
                                                        RhiAccess::CopyDest);
    Barrier(*list, {}, Span<const RhiTextureBarrier>(&toCopyDest, 1));
    RhiClearValue clear;
    clear.Color[0] = 1;
    clear.Color[1] = 0.5f;
    clear.Color[2] = 0;
    clear.Color[3] = 1;
    RhiSubresourceRange mip0;
    mip0.MipCount = 1;
    list->ClearTexture(texture, mip0, clear);
    RhiSubresourceRange mip1;
    mip1.FirstMip = 1;
    clear.Color[0] = 0;
    clear.Color[2] = 1;
    list->ClearTexture(texture, mip1, clear);
    const RhiTextureBarrier toCopySource = TextureBarrier(texture, RhiLayout::CopyDest, RhiLayout::CopySource,
                                                          RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy,
                                                          RhiAccess::CopySource);
    Barrier(*list, {}, Span<const RhiTextureBarrier>(&toCopySource, 1));
    RhiTextureRegion region;
    list->CopyTextureToBuffer(readback, {}, texture, region);
    region.Mip = 1;
    RhiBufferTextureLayout layout;
    layout.Offset = 64 * 32 * 4;
    list->CopyTextureToBuffer(readback, layout, texture, region);
    HostBarrier(*list);
    SubmitAndWait(list);

    const uint8 *pixels = Device->GetMappedData(readback);
    MARLIN_EXPECTF(pixels[0] == 255 && (pixels[1] == 127 || pixels[1] == 128) && pixels[2] == 0 && pixels[3] == 255,
                   "mip 0 pixel is {0} {1} {2} {3}", pixels[0], pixels[1], pixels[2], pixels[3]);
    const uint8 *last = pixels + 64 * 32 * 4 - 4;
    MARLIN_EXPECT(last[0] == 255 && last[2] == 0);
    const uint8 *mip = pixels + 64 * 32 * 4;
    MARLIN_EXPECTF(mip[0] == 0 && mip[2] == 255 && mip[3] == 255, "mip 1 pixel is {0} {1} {2} {3}", mip[0], mip[1],
                   mip[2], mip[3]);
    Device->Destroy(texture);
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, BufferToTextureRoundTrip, Gpu)
{
    RhiTextureDesc desc;
    desc.Format = RhiFormat::R32Uint;
    desc.Width = 16;
    desc.Height = 16;
    desc.DepthOrArraySize = 2;
    desc.Usage = RhiTextureUsage::CopySource | RhiTextureUsage::CopyDest;
    Result<RhiTexture, RhiError> created = Device->CreateTexture(desc);
    MARLIN_REQUIRE(created.IsOk());
    const RhiTexture texture = created.Value();
    constexpr uint32 Texels = 16 * 16;
    const RhiBuffer upload = CreateBuffer(Texels * 4, RhiBufferUsage::CopySource, RhiMemoryLocation::Upload);
    const RhiBuffer readback = CreateBuffer(Texels * 4, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    auto *source = reinterpret_cast<uint32 *>(Device->GetMappedData(upload));
    for (uint32 i = 0; i < Texels; ++i)
    {
        source[i] = 0x10000 + i;
    }

    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Copy);
    MARLIN_REQUIRE(list != nullptr);
    const RhiTextureBarrier toCopyDest = TextureBarrier(texture, RhiLayout::Undefined, RhiLayout::CopyDest,
                                                        RhiSync::None, RhiAccess::None, RhiSync::Copy,
                                                        RhiAccess::CopyDest);
    Barrier(*list, {}, Span<const RhiTextureBarrier>(&toCopyDest, 1));
    RhiTextureRegion slice1;
    slice1.Slice = 1;
    list->CopyBufferToTexture(texture, slice1, upload, {});
    const RhiTextureBarrier toCopySource = TextureBarrier(texture, RhiLayout::CopyDest, RhiLayout::CopySource,
                                                          RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy,
                                                          RhiAccess::CopySource);
    Barrier(*list, {}, Span<const RhiTextureBarrier>(&toCopySource, 1));
    list->CopyTextureToBuffer(readback, {}, texture, slice1);
    HostBarrier(*list);
    SubmitAndWait(list);

    const auto *result = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback));
    uint32 wrong = 0;
    for (uint32 i = 0; i < Texels; ++i)
    {
        wrong += result[i] != 0x10000 + i ? 1 : 0;
    }
    MARLIN_EXPECTF(wrong == 0, "{0} of {1} texels differ", wrong, Texels);
    Device->Destroy(texture);
    Device->Destroy(upload);
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, QueuesWaitOnFences, Gpu)
{
    const RhiBuffer shared = CreateBuffer(4096, RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest,
                                          RhiMemoryLocation::Device);
    const RhiBuffer readback = CreateBuffer(4096, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    Result<RhiFence, RhiError> fence = Device->CreateFence(0, "CrossQueue");
    MARLIN_REQUIRE(fence.IsOk());

    RhiCommandList *copy = Device->BeginCommandList(RhiQueueType::Copy);
    copy->FillBuffer(shared, 0, 4096, 0x5A5A5A5Au);
    RhiBufferBarrier release =
        BufferBarrier(shared, RhiSync::Copy, RhiAccess::CopyDest, RhiSync::None, RhiAccess::None);
    release.TransferQueue = true;
    release.Transfer = {RhiQueueType::Copy, RhiQueueType::Graphics};
    Barrier(*copy, Span<const RhiBufferBarrier>(&release, 1));
    RhiCommandList *copyLists[] = {copy};
    const RhiFenceSignal signal{fence.Value(), 1};
    RhiSubmitDesc copySubmit;
    copySubmit.CommandLists = copyLists;
    copySubmit.Signals = Span<const RhiFenceSignal>(&signal, 1);
    MARLIN_REQUIRE(Device->Submit(RhiQueueType::Copy, copySubmit).IsOk());

    RhiCommandList *graphics = Device->BeginCommandList(RhiQueueType::Graphics);
    RhiBufferBarrier acquire =
        BufferBarrier(shared, RhiSync::None, RhiAccess::None, RhiSync::Copy, RhiAccess::CopySource);
    acquire.TransferQueue = true;
    acquire.Transfer = {RhiQueueType::Copy, RhiQueueType::Graphics};
    Barrier(*graphics, Span<const RhiBufferBarrier>(&acquire, 1));
    graphics->CopyBuffer(readback, 0, shared, 0, 4096);
    HostBarrier(*graphics);
    RhiCommandList *graphicsLists[] = {graphics};
    const RhiFenceWait wait{fence.Value(), 1};
    RhiSubmitDesc graphicsSubmit;
    graphicsSubmit.CommandLists = graphicsLists;
    graphicsSubmit.Waits = Span<const RhiFenceWait>(&wait, 1);
    Result<uint64, RhiError> value = Device->Submit(RhiQueueType::Graphics, graphicsSubmit);
    MARLIN_REQUIRE(value.IsOk());
    MARLIN_REQUIRE(Device->Wait(Device->GetQueueFence(RhiQueueType::Graphics), value.Value(), 10000));
    MARLIN_EXPECT(Device->GetCompletedValue(fence.Value()) == 1);

    const auto *result = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback));
    MARLIN_EXPECT(result[0] == 0x5A5A5A5Au && result[1023] == 0x5A5A5A5Au);
    Device->Destroy(fence.Value());
    Device->Destroy(shared);
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, HostSignalsAndWaitsOnFences, Gpu)
{
    Result<RhiFence, RhiError> fence = Device->CreateFence(5, "Host");
    MARLIN_REQUIRE(fence.IsOk());
    MARLIN_EXPECT(Device->GetCompletedValue(fence.Value()) == 5);
    MARLIN_EXPECT(!Device->Wait(fence.Value(), 6, 1));
    Device->Signal(fence.Value(), 9);
    MARLIN_EXPECT(Device->Wait(fence.Value(), 9, 1000));
    MARLIN_EXPECT(Device->GetCompletedValue(fence.Value()) == 9);
    Device->Destroy(fence.Value());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, TimestampsAdvance, Gpu)
{
    RhiQueryPoolDesc desc;
    desc.Kind = RhiQueryKind::Timestamp;
    desc.Count = 2;
    Result<RhiQueryPool, RhiError> pool = Device->CreateQueryPool(desc);
    MARLIN_REQUIRE(pool.IsOk());
    const RhiBuffer readback = CreateBuffer(16, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    const RhiBuffer scratch = CreateBuffer(1 << 20, RhiBufferUsage::CopyDest, RhiMemoryLocation::Device);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    list->WriteTimestamp(pool.Value(), 0);
    list->FillBuffer(scratch, 0, 1 << 20, 7);
    list->WriteTimestamp(pool.Value(), 1);
    list->ResolveQueries(pool.Value(), 0, 2, readback, 0);
    HostBarrier(*list);
    SubmitAndWait(list);
    const auto *ticks = reinterpret_cast<const uint64 *>(Device->GetMappedData(readback));
    MARLIN_EXPECTF(ticks[0] != 0 && ticks[1] >= ticks[0], "timestamps {0} and {1}", ticks[0], ticks[1]);
    uint64 cpu = 0;
    uint64 gpu = 0;
    Device->GetCalibratedTimestamps(RhiQueueType::Graphics, cpu, gpu);
    MARLIN_EXPECT(cpu != 0 && gpu != 0);
    Device->Destroy(pool.Value());
    Device->Destroy(readback);
    Device->Destroy(scratch);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, DestroyedHandlesGoStale, Gpu)
{
    const RhiBuffer buffer = CreateBuffer(256, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    MARLIN_EXPECT(Device->GetMappedData(buffer) != nullptr);
    Device->Destroy(buffer);
    MARLIN_EXPECT(Device->GetMappedData(buffer) != nullptr);
    Device->ProcessCompletions();
    MARLIN_EXPECT(Device->GetMappedData(buffer) == nullptr);
    const RhiBuffer reused = CreateBuffer(256, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    MARLIN_EXPECT(reused.Index == buffer.Index && reused.Generation != buffer.Generation);
    Device->Destroy(reused);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, ViewsAndSamplersTakeHeapSlots, Gpu)
{
    RhiTextureDesc desc;
    desc.Format = RhiFormat::RGBA16Float;
    desc.Width = 32;
    desc.Height = 32;
    desc.MipCount = 6;
    desc.Usage = RhiTextureUsage::ShaderResource | RhiTextureUsage::UnorderedAccess;
    Result<RhiTexture, RhiError> texture = Device->CreateTexture(desc);
    MARLIN_REQUIRE(texture.IsOk());
    RhiTextureViewDesc srv;
    srv.Texture = texture.Value();
    Result<RhiResourceView, RhiError> a = Device->CreateView(srv);
    RhiTextureViewDesc uav = srv;
    uav.Access = RhiViewAccess::UnorderedAccess;
    uav.Range.FirstMip = 2;
    uav.Range.MipCount = 1;
    Result<RhiResourceView, RhiError> b = Device->CreateView(uav);
    const RhiBuffer buffer = CreateBuffer(4096, RhiBufferUsage::ShaderResource, RhiMemoryLocation::Device);
    RhiBufferViewDesc structured;
    structured.Buffer = buffer;
    structured.Kind = RhiBufferViewKind::Structured;
    structured.Stride = 16;
    Result<RhiResourceView, RhiError> c = Device->CreateView(structured);
    MARLIN_REQUIRE(a.IsOk() && b.IsOk() && c.IsOk());
    MARLIN_EXPECT(a.Value().Index != b.Value().Index && b.Value().Index != c.Value().Index);
    MARLIN_EXPECT(a.Value().Index < Device->GetLimits().MaxResourceDescriptors);

    RhiSamplerDesc linear;
    RhiSamplerDesc point;
    point.MinFilter = point.MagFilter = point.MipFilter = RhiFilter::Nearest;
    Result<RhiSampler, RhiError> s1 = Device->CreateSampler(linear);
    Result<RhiSampler, RhiError> s2 = Device->CreateSampler(linear);
    Result<RhiSampler, RhiError> s3 = Device->CreateSampler(point);
    MARLIN_REQUIRE(s1.IsOk() && s2.IsOk() && s3.IsOk());
    MARLIN_EXPECT(s1.Value() == s2.Value() && s1.Value() != s3.Value());
    Device->Destroy(s1.Value());
    Device->Destroy(s2.Value());
    Device->Destroy(s3.Value());
    Device->Destroy(a.Value());
    Device->Destroy(b.Value());
    Device->Destroy(c.Value());
    Device->Destroy(texture.Value());
    Device->Destroy(buffer);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, UploadAllocatorFeedsCopies, Gpu)
{
    RhiUploadAllocator uploads(*Device, 64 * 1024);
    const RhiBuffer readback = CreateBuffer(3 * 1024, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    for (uint32 part = 0; part < 3; ++part)
    {
        const RhiUploadAllocation allocation = uploads.Allocate(1024, 256);
        MARLIN_REQUIRE(allocation.Data != nullptr && allocation.Address != 0);
        for (uint32 i = 0; i < 1024; ++i)
        {
            allocation.Data[i] = static_cast<uint8>(part * 50 + i);
        }
        list->CopyBuffer(readback, part * 1024, allocation.Buffer, allocation.Offset, 1024);
    }
    const RhiUploadAllocation large = uploads.Allocate(128 * 1024);
    MARLIN_EXPECT(large.Data != nullptr && large.Size == 128 * 1024);
    HostBarrier(*list);
    SubmitAndWait(list);
    uploads.EndFrame();
    const uint8 *result = Device->GetMappedData(readback);
    MARLIN_EXPECT(result[0] == 0 && result[1024] == 50 && result[2047] == static_cast<uint8>(50 + 1023));
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, UploadQueueStreamsOnTheCopyQueue, Gpu)
{
    RhiUploadQueue uploads(*Device, 1024 * 1024);
    const RhiBuffer buffer =
        CreateBuffer(4096, RhiBufferUsage::CopyDest | RhiBufferUsage::CopySource, RhiMemoryLocation::Device);
    RhiTextureDesc desc;
    desc.Format = RhiFormat::R32Uint;
    desc.Width = 8;
    desc.Height = 8;
    desc.Usage = RhiTextureUsage::CopyDest | RhiTextureUsage::CopySource;
    Result<RhiTexture, RhiError> texture = Device->CreateTexture(desc);
    MARLIN_REQUIRE(texture.IsOk());

    uint32 words[1024];
    for (uint32 i = 0; i < 1024; ++i)
    {
        words[i] = 0xC0DE0000u + i;
    }
    RhiBufferUpload bufferUpload;
    bufferUpload.Buffer = buffer;
    bufferUpload.Data = AsBytes(Span<const uint32>(words));
    const RhiUploadTicket first = uploads.Upload(bufferUpload);
    RhiTextureUpload textureUpload;
    textureUpload.Texture = texture.Value();
    textureUpload.Data = AsBytes(Span<const uint32>(words, 64));
    textureUpload.FinalLayout = RhiLayout::CopySource;
    const RhiUploadTicket second = uploads.Upload(textureUpload);
    MARLIN_REQUIRE(first.IsValid() && second.Value == first.Value);
    MARLIN_REQUIRE(uploads.Flush().IsOk());

    const RhiBuffer readback = CreateBuffer(4096 + 256, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    RhiBufferBarrier bufferAcquire = RhiUploadQueue::GetAcquireBarrier(bufferUpload);
    bufferAcquire.SyncAfter = RhiSync::Copy;
    bufferAcquire.AccessAfter = RhiAccess::CopySource;
    RhiTextureBarrier textureAcquire = RhiUploadQueue::GetAcquireBarrier(textureUpload);
    textureAcquire.SyncAfter = RhiSync::Copy;
    textureAcquire.AccessAfter = RhiAccess::CopySource;
    Barrier(*list, Span<const RhiBufferBarrier>(&bufferAcquire, 1), Span<const RhiTextureBarrier>(&textureAcquire, 1));
    list->CopyBuffer(readback, 0, buffer, 0, 4096);
    RhiBufferTextureLayout layout;
    layout.Offset = 4096;
    list->CopyTextureToBuffer(readback, layout, texture.Value(), {});
    HostBarrier(*list);
    RhiCommandList *lists[] = {list};
    const RhiFenceWait wait = uploads.GetWait(first);
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    submit.Waits = Span<const RhiFenceWait>(&wait, 1);
    Result<uint64, RhiError> value = Device->Submit(RhiQueueType::Graphics, submit);
    MARLIN_REQUIRE(value.IsOk());
    MARLIN_REQUIRE(Device->Wait(Device->GetQueueFence(RhiQueueType::Graphics), value.Value(), 10000));
    MARLIN_EXPECT(uploads.IsComplete(first));
    uploads.ProcessCompletions();

    const auto *result = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback));
    MARLIN_EXPECT(result[0] == 0xC0DE0000u && result[1023] == 0xC0DE0000u + 1023);
    MARLIN_EXPECT(result[1024] == 0xC0DE0000u && result[1024 + 63] == 0xC0DE0000u + 63);
    Device->Destroy(readback);
    Device->Destroy(buffer);
    Device->Destroy(texture.Value());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, UploadQueueWaitsForAFullRingAndBatchesMips, Gpu)
{
    // 24 KiB uploads into a 64 KiB ring: the third finds it full and waits for the first batch itself.
    constexpr uint32 Words = 6 * 1024;
    RhiUploadQueue uploads(*Device, 64 * 1024);
    const RhiBuffer buffer =
        CreateBuffer(3 * Words * 4, RhiBufferUsage::CopyDest | RhiBufferUsage::CopySource, RhiMemoryLocation::Device);
    static uint32 words[3][Words];
    RhiBufferUpload bufferUploads[3];
    RhiUploadTicket last;
    for (uint32 part = 0; part < 3; ++part)
    {
        for (uint32 i = 0; i < Words; ++i)
        {
            words[part][i] = (part << 24) | i;
        }
        bufferUploads[part].Buffer = buffer;
        bufferUploads[part].Offset = uint64{part} * Words * 4;
        bufferUploads[part].Data = AsBytes(Span<const uint32>(words[part]));
        last = uploads.Upload(bufferUploads[part]);
        MARLIN_REQUIRE(last.IsValid());
        MARLIN_REQUIRE(uploads.Flush().IsOk());
    }

    // Two mips of one texture in one batch share the batch's barriers.
    RhiTextureDesc desc;
    desc.Format = RhiFormat::R32Uint;
    desc.Width = 8;
    desc.Height = 8;
    desc.MipCount = 2;
    desc.Usage = RhiTextureUsage::CopyDest | RhiTextureUsage::CopySource;
    Result<RhiTexture, RhiError> texture = Device->CreateTexture(desc);
    MARLIN_REQUIRE(texture.IsOk());
    RhiTextureUpload mipUploads[2];
    for (uint32 mip = 0; mip < 2; ++mip)
    {
        mipUploads[mip].Texture = texture.Value();
        mipUploads[mip].Region.Mip = mip;
        mipUploads[mip].Data = AsBytes(Span<const uint32>(words[mip], mip == 0 ? 64 : 16));
        mipUploads[mip].FinalLayout = RhiLayout::CopySource;
        last = uploads.Upload(mipUploads[mip]);
        MARLIN_REQUIRE(last.IsValid());
    }
    MARLIN_REQUIRE(uploads.Flush().IsOk());

    const RhiBuffer readback =
        CreateBuffer(3 * Words * 4 + 512, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    RhiBufferBarrier bufferAcquires[3];
    for (uint32 part = 0; part < 3; ++part)
    {
        bufferAcquires[part] = RhiUploadQueue::GetAcquireBarrier(bufferUploads[part]);
        bufferAcquires[part].SyncAfter = RhiSync::Copy;
        bufferAcquires[part].AccessAfter = RhiAccess::CopySource;
    }
    RhiTextureBarrier textureAcquires[2];
    for (uint32 mip = 0; mip < 2; ++mip)
    {
        textureAcquires[mip] = RhiUploadQueue::GetAcquireBarrier(mipUploads[mip]);
        textureAcquires[mip].SyncAfter = RhiSync::Copy;
        textureAcquires[mip].AccessAfter = RhiAccess::CopySource;
    }
    Barrier(*list, bufferAcquires, textureAcquires);
    list->CopyBuffer(readback, 0, buffer, 0, uint64{3} * Words * 4);
    RhiBufferTextureLayout layout;
    layout.Offset = uint64{3} * Words * 4;
    list->CopyTextureToBuffer(readback, layout, texture.Value(), {});
    layout.Offset += 256;
    RhiTextureRegion mip1;
    mip1.Mip = 1;
    list->CopyTextureToBuffer(readback, layout, texture.Value(), mip1);
    HostBarrier(*list);
    RhiCommandList *lists[] = {list};
    const RhiFenceWait wait = uploads.GetWait(last);
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    submit.Waits = Span<const RhiFenceWait>(&wait, 1);
    Result<uint64, RhiError> value = Device->Submit(RhiQueueType::Graphics, submit);
    MARLIN_REQUIRE(value.IsOk());
    MARLIN_REQUIRE(Device->Wait(Device->GetQueueFence(RhiQueueType::Graphics), value.Value(), 10000));
    uploads.ProcessCompletions();

    const auto *result = reinterpret_cast<const uint32 *>(Device->GetMappedData(readback));
    for (uint32 part = 0; part < 3; ++part)
    {
        MARLIN_EXPECT(result[part * Words] == part << 24 &&
                      result[part * Words + Words - 1] == ((part << 24) | (Words - 1)));
    }
    const uint32 *mips = result + 3 * Words;
    MARLIN_EXPECT(mips[0] == 0 && mips[63] == 63);
    MARLIN_EXPECT(mips[64] == (1u << 24) && mips[64 + 15] == ((1u << 24) | 15));
    Device->Destroy(readback);
    Device->Destroy(buffer);
    Device->Destroy(texture.Value());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, BuildsAccelerationStructures, Gpu)
{
    const float32 vertices[] = {0, 0, 0, 1, 0, 0, 0, 1, 0};
    const RhiBuffer input =
        CreateBuffer(sizeof(vertices), RhiBufferUsage::AccelerationStructureBuildInput, RhiMemoryLocation::Upload);
    __builtin_memcpy(Device->GetMappedData(input), vertices, sizeof(vertices));
    RhiGeometry triangle;
    triangle.VertexAddress = Device->GetBufferAddress(input);
    triangle.VertexStride = 12;
    triangle.VertexCount = 3;
    RhiAccelerationStructureInputs inputs;
    inputs.Geometries = Span<const RhiGeometry>(&triangle, 1);
    inputs.Flags = RhiBuildFlags::PreferFastTrace | RhiBuildFlags::AllowCompaction;
    const RhiAccelerationStructureSizes sizes = Device->GetAccelerationStructureSizes(inputs);
    MARLIN_REQUIRE(sizes.StructureSize > 0 && sizes.BuildScratchSize > 0);

    const RhiBuffer storage = CreateBuffer(sizes.StructureSize, RhiBufferUsage::AccelerationStructure,
                                           RhiMemoryLocation::Device);
    const RhiBuffer scratch = CreateBuffer(sizes.BuildScratchSize + 256, RhiBufferUsage::AccelerationStructure,
                                           RhiMemoryLocation::Device);
    const RhiBuffer readback = CreateBuffer(8, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiAccelerationStructureDesc desc;
    desc.Buffer = storage;
    desc.Size = sizes.StructureSize;
    Result<RhiAccelerationStructure, RhiError> structure = Device->CreateAccelerationStructure(desc);
    MARLIN_REQUIRE(structure.IsOk());
    MARLIN_EXPECT(Device->GetAccelerationStructureAddress(structure.Value()) != 0);
    Result<RhiResourceView, RhiError> view = Device->CreateView(structure.Value());
    MARLIN_EXPECT(view.IsOk());

    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Compute);
    RhiAccelerationStructureBuild build;
    build.Destination = structure.Value();
    build.Inputs = inputs;
    build.ScratchAddress = (Device->GetBufferAddress(scratch) + 255) & ~uint64{255};
    list->BuildAccelerationStructures(Span<const RhiAccelerationStructureBuild>(&build, 1));
    RhiGlobalBarrier built;
    built.SyncBefore = RhiSync::AccelerationStructureBuild;
    built.AccessBefore = RhiAccess::AccelerationStructureWrite;
    built.SyncAfter = RhiSync::AccelerationStructureCopy;
    built.AccessAfter = RhiAccess::AccelerationStructureRead;
    RhiBarriers barriers;
    barriers.Global = Span<const RhiGlobalBarrier>(&built, 1);
    list->Barrier(barriers);
    const RhiAccelerationStructure structures[] = {structure.Value()};
    list->WriteAccelerationStructureProperties(structures, RhiAccelerationStructureProperty::CompactedSize, readback,
                                               0);
    HostBarrier(*list);
    SubmitAndWait(list);
    const uint64 compacted = *reinterpret_cast<const uint64 *>(Device->GetMappedData(readback));
    MARLIN_EXPECTF(compacted > 0 && compacted <= sizes.StructureSize, "compacted size {0} of {1}", compacted,
                   sizes.StructureSize);
    if (view.IsOk())
    {
        Device->Destroy(view.Value());
    }
    Device->Destroy(structure.Value());
    Device->Destroy(storage);
    Device->Destroy(scratch);
    Device->Destroy(input);
    Device->Destroy(readback);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, ProfilerTimesZonesOnTheCpuClock, Gpu)
{
    const RhiBuffer scratch = CreateBuffer(16 << 20, RhiBufferUsage::CopyDest, RhiMemoryLocation::Device);
    List<RhiMemoryPoolStats> pools;
    Device->GetMemoryStats(pools);
    uint64 used = 0;
    for (const RhiMemoryPoolStats &pool : pools)
    {
        used += pool.UsedBytes;
    }
    MARLIN_EXPECTF(used >= 16 << 20, "the allocator reports {0} bytes in use", used);

    TimeFill(*Device, RhiQueueType::Graphics, scratch);
    Device->ProcessCompletions();
    Device->Destroy(scratch);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, ProfilerTimesZonesOnEveryQueue, Gpu)
{
    const RhiBuffer scratch = CreateBuffer(16 << 20, RhiBufferUsage::CopyDest, RhiMemoryLocation::Device);
    for (uint32 queue = 0; queue < RhiQueueTypeCount; ++queue)
    {
        if (Device->GetLimits().TimestampFrequency[queue] != 0)
        {
            TimeFill(*Device, static_cast<RhiQueueType>(queue), scratch);
        }
    }
    Device->ProcessCompletions();
    Device->Destroy(scratch);
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, HandsNativeObjectsToSdks, Gpu)
{
    RhiTextureDesc desc;
    desc.Format = RhiFormat::D32Float;
    desc.Width = 64;
    desc.Height = 64;
    desc.Usage = RhiTextureUsage::DepthStencil | RhiTextureUsage::ShaderResource;
    Result<RhiTexture, RhiError> texture = Device->CreateTexture(desc);
    MARLIN_REQUIRE(texture.IsOk());
    const RhiNativeTexture native = Device->GetNativeTexture(texture.Value());
    MARLIN_EXPECT(native.Resource != nullptr && native.View != nullptr && native.Format != 0);
    const RhiNativeTexture again = Device->GetNativeTexture(texture.Value());
    MARLIN_EXPECT(again.View == native.View);

    const RhiBuffer buffer = CreateBuffer(1024, RhiBufferUsage::CopyDest | RhiBufferUsage::CopySource,
                                          RhiMemoryLocation::Device);
    const RhiNativeBuffer nativeBuffer = Device->GetNativeBuffer(buffer);
    MARLIN_EXPECT(nativeBuffer.Resource != nullptr && nativeBuffer.Size == 1024 && nativeBuffer.Address != 0);

    const RhiBuffer readback = CreateBuffer(1024, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    MARLIN_EXPECT(list->BeginNativeCommands() != nullptr);
    list->EndNativeCommands();
    list->FillBuffer(buffer, 0, 1024, 0x0BADCAFEu);
    const RhiBufferBarrier filled =
        BufferBarrier(buffer, RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy, RhiAccess::CopySource);
    Barrier(*list, Span<const RhiBufferBarrier>(&filled, 1));
    list->CopyBuffer(readback, 0, buffer, 0, 1024);
    HostBarrier(*list);
    SubmitAndWait(list);
    MARLIN_EXPECT(*reinterpret_cast<const uint32 *>(Device->GetMappedData(readback)) == 0x0BADCAFEu);
    Device->Destroy(readback);
    Device->Destroy(buffer);
    Device->Destroy(texture.Value());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, MakesGeneratedCommandSignatures, Gpu)
{
    const RhiCommandSignature plain = Device->GetCommandSignature(RhiIndirectCommandKind::Dispatch);
    MARLIN_EXPECT(plain.IsValid());
    if (!Device->HasFeature(RhiFeatures::IndirectPipelineSwitch))
    {
        Testing::Print("IndirectPipelineSwitch is not supported; generated signatures skipped");
        return;
    }
    const RhiIndirectCommand commands[] = {
        {RhiIndirectCommandKind::ShaderArguments, 0, 8},
        {RhiIndirectCommandKind::Dispatch, 0, 0},
    };
    RhiCommandSignatureDesc desc;
    desc.Commands = commands;
    desc.PipelineKind = RhiPipelineKind::Compute;
    desc.DebugName = "ConstantsAndDispatch";
    Result<RhiCommandSignature, RhiError> signature = Device->CreateCommandSignature(desc);
    MARLIN_REQUIRE(signature.IsOk());
    Device->Destroy(signature.Value());
}

MARLIN_TEST_F(RhiDeviceFixture, Rhi, ReservedBuffersMapTiles, Gpu)
{
    if (!Device->HasFeature(RhiFeatures::SparseResources))
    {
        Testing::Print("SparseResources is not supported; skipped");
        return;
    }
    constexpr uint64 Tile = 64 * 1024;
    RhiBufferDesc desc;
    desc.Size = Tile * 4;
    desc.Usage = RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest;
    desc.Flags = RhiResourceFlags::Reserved;
    Result<RhiBuffer, RhiError> reserved = Device->CreateBuffer(desc);
    MARLIN_REQUIRE(reserved.IsOk());
    RhiHeapDesc heapDesc;
    heapDesc.Size = Tile;
    Result<RhiHeap, RhiError> heap = Device->CreateHeap(heapDesc);
    MARLIN_REQUIRE(heap.IsOk());
    RhiTileMapping mapping;
    mapping.Buffer = reserved.Value();
    mapping.BufferOffset = Tile * 2;
    mapping.BufferSize = Tile;
    mapping.Heap = heap.Value();
    MARLIN_REQUIRE(Device->UpdateTileMappings(RhiQueueType::Graphics, Span<const RhiTileMapping>(&mapping, 1)).IsOk());

    const RhiBuffer readback = CreateBuffer(256, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);
    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    list->FillBuffer(reserved.Value(), Tile * 2, 256, 0x600DF00Du);
    const RhiBufferBarrier filled =
        BufferBarrier(reserved.Value(), RhiSync::Copy, RhiAccess::CopyDest, RhiSync::Copy, RhiAccess::CopySource);
    Barrier(*list, Span<const RhiBufferBarrier>(&filled, 1));
    list->CopyBuffer(readback, 0, reserved.Value(), Tile * 2, 256);
    HostBarrier(*list);
    SubmitAndWait(list);
    MARLIN_EXPECT(*reinterpret_cast<const uint32 *>(Device->GetMappedData(readback)) == 0x600DF00Du);
    Device->Destroy(readback);
    Device->Destroy(reserved.Value());
    Device->Destroy(heap.Value());
}
} // namespace Marlin::Tests
