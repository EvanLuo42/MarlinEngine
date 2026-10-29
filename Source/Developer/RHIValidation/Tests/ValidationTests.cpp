#include "RHI/RhiTestDevice.h"
#include "ValidationDevice.h"

namespace Marlin::Tests
{
namespace
{
/// Takes the errors logged so far: those the test expects, which the fixture would otherwise report as failures.
uint32 TakeValidationErrors(RhiErrorSink &sink, StringView containing)
{
    Log::Flush();
    uint32 matching = 0;
    for (const String &error : sink.Errors)
    {
        matching += error.Contains(containing) ? 1 : 0;
    }
    sink.Errors.Clear();
    return matching;
}
} // namespace

MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, PassesCorrectUse, Gpu)
{
    ValidationDevice device(*Device);
    RhiBufferDesc desc;
    desc.Size = 256;
    desc.Usage = RhiBufferUsage::CopyDest;
    desc.Location = RhiMemoryLocation::Readback;
    Result<RhiBuffer, RhiError> buffer = device.CreateBuffer(desc);
    MARLIN_REQUIRE(buffer.IsOk());
    RhiCommandList *list = device.BeginCommandList(RhiQueueType::Graphics);
    list->FillBuffer(buffer.Value(), 0, 256, 1);
    RhiCommandList *lists[] = {list};
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    Result<uint64, RhiError> value = device.Submit(RhiQueueType::Graphics, submit);
    MARLIN_REQUIRE(value.IsOk());
    MARLIN_EXPECT(device.Wait(device.GetQueueFence(RhiQueueType::Graphics), value.Value(), 10000));
    device.Destroy(buffer.Value());
    device.ProcessCompletions();
    MARLIN_EXPECT(TakeValidationErrors(Sink, "") == 0);
}

MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, CatchesDoubleDestroyAndStaleHandles, Gpu)
{
    ValidationDevice device(*Device);
    RhiBufferDesc desc;
    desc.Size = 256;
    desc.Usage = RhiBufferUsage::CopyDest;
    desc.Location = RhiMemoryLocation::Readback;
    Result<RhiBuffer, RhiError> buffer = device.CreateBuffer(desc);
    MARLIN_REQUIRE(buffer.IsOk());
    device.Destroy(buffer.Value());
    device.Destroy(buffer.Value());
    MARLIN_EXPECT(TakeValidationErrors(Sink, "destroyed already") == 1);
    MARLIN_EXPECT(device.GetMappedData(buffer.Value()) == nullptr);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "stale buffer") == 1);
    device.ProcessCompletions();
}

MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, CatchesBadDescriptions, Gpu)
{
    ValidationDevice device(*Device);
    RhiTextureDesc texture;
    texture.Format = RhiFormat::RGBA8Unorm;
    texture.Width = 16;
    texture.Height = 16;
    texture.MipCount = 9;
    texture.Usage = RhiTextureUsage::ShaderResource;
    Result<RhiTexture, RhiError> created = device.CreateTexture(texture);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "mips") == 1);
    if (created.IsOk())
    {
        device.Destroy(created.Value());
    }
    texture.MipCount = 1;
    created = device.CreateTexture(texture);
    MARLIN_REQUIRE(created.IsOk());
    RhiTextureViewDesc view;
    view.Texture = created.Value();
    view.Access = RhiViewAccess::UnorderedAccess;
    Result<RhiResourceView, RhiError> uav = device.CreateView(view);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "UnorderedAccess usage") == 1);
    if (uav.IsOk())
    {
        device.Destroy(uav.Value());
    }
    device.Destroy(created.Value());
    device.ProcessCompletions();
    TakeValidationErrors(Sink, "");
}

MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, CatchesCommandListMisuse, Gpu)
{
    ValidationDevice device(*Device);
    RhiCommandList *copy = device.BeginCommandList(RhiQueueType::Copy);
    copy->Dispatch(1, 1, 1);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "Dispatch on a Copy command list") == 1);
    copy->BeginMarker(Name("Unclosed"));
    copy->EndMarker();
    copy->EndMarker();
    MARLIN_EXPECT(TakeValidationErrors(Sink, "EndMarker without a BeginMarker") == 1);
    const uint32 tooLong[40] = {};
    copy->SetShaderArguments(AsBytes(Span<const uint32>(tooLong)), 0);
    TakeValidationErrors(Sink, "");
    device.DiscardCommandList(copy);

    RhiCommandList *graphics = device.BeginCommandList(RhiQueueType::Graphics);
    graphics->Draw({});
    MARLIN_EXPECT(TakeValidationErrors(Sink, "Draw outside BeginRendering") == 1);
    graphics->BeginMarker(Name("Open"));
    RhiCommandList *lists[] = {graphics};
    RhiSubmitDesc submit;
    submit.CommandLists = lists;
    Result<uint64, RhiError> value = device.Submit(RhiQueueType::Graphics, submit);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "markers open") == 1);
    if (value.IsOk())
    {
        device.Wait(device.GetQueueFence(RhiQueueType::Graphics), value.Value(), 10000);
    }
    device.ProcessCompletions();
    TakeValidationErrors(Sink, "");
}

MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, CatchesFenceValuesGoingBack, Gpu)
{
    ValidationDevice device(*Device);
    Result<RhiFence, RhiError> fence = device.CreateFence(4, "Validation");
    MARLIN_REQUIRE(fence.IsOk());
    device.Signal(fence.Value(), 3);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "already reaches 4") == 1);
    device.Destroy(fence.Value());
    device.ProcessCompletions();
}
} // namespace Marlin::Tests

namespace Marlin::Tests
{
MARLIN_TEST_F(RhiDeviceFixture, RhiValidation, TakesTheBuiltInCommandSignatures, Gpu)
{
    ValidationDevice device(*Device);
    RhiBufferDesc desc;
    desc.Size = 64;
    desc.Usage = RhiBufferUsage::IndirectArguments | RhiBufferUsage::CopyDest;
    Result<RhiBuffer, RhiError> arguments = device.CreateBuffer(desc);
    MARLIN_REQUIRE(arguments.IsOk());
    RhiCommandList *list = device.BeginCommandList(RhiQueueType::Graphics);
    // Recorded and discarded: the plain Dispatch signature must pass as a known one.
    list->ExecuteIndirect(device.GetCommandSignature(RhiIndirectCommandKind::Dispatch), arguments.Value(), 0, 1);
    MARLIN_EXPECT(TakeValidationErrors(Sink, "stale command signature") == 0);
    device.DiscardCommandList(list);
    device.Destroy(device.GetCommandSignature(RhiIndirectCommandKind::DrawMesh));
    MARLIN_EXPECT(TakeValidationErrors(Sink, "built-in command signature") == 1);
    device.Destroy(arguments.Value());
    device.ProcessCompletions();
    MARLIN_EXPECT(TakeValidationErrors(Sink, "") == 0);
}
} // namespace Marlin::Tests
