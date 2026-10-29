#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "Misc/CommandLine.h"
#include "RHI/RhiBackend.h"
#include "RHI/RhiDevice.h"
#include "Reflection/Reflection.h"
#include "String/String.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
/// Fails the running test for every error the RHI or the graphics API's validation logs.
class RhiErrorSink final : public ILogSink
{
public:
    void Write(const LogRecord &record) override
    {
        if (record.Level >= LogLevel::Error && record.Category != nullptr &&
            StringView(record.Category->GetName()).StartsWith("LogRhi"))
        {
            Errors.Add(String(record.Message));
        }
    }

    List<String> Errors;
};

/// For test modules only: a device of the backend `-Rhi=` names, or of the available one with the highest priority,
/// with API and synchronization validation, so the RHI tests check the contract every backend shares.
MARLIN_TEST_FIXTURE(RhiDeviceFixture)
{
    void SetUp() override
    {
        Log::AddSink(&Sink);
        StringView wanted;
        const bool named = CommandLine::TryGetValue("Rhi", wanted);
        List<const TypeInfo *> types;
        Reflection::GetDerivedTypes(TypeOf<IRhiBackend>(), types);
        List<IRhiBackend *> backends;
        for (const TypeInfo *type : types)
        {
            if (type->GetKind() == TypeKind::Class && !type->IsAbstract())
            {
                if (void *object = type->New())
                {
                    backends.Add(static_cast<IRhiBackend *>(type->CastTo(object, TypeOf<IRhiBackend>())));
                }
            }
        }
        backends.Sort([](const IRhiBackend *a, const IRhiBackend *b) { return a->GetPriority() > b->GetPriority(); });
        for (IRhiBackend *backend : backends)
        {
            const bool wantedOne = !named || backend->GetName().ToView() == wanted;
            if (Backend == nullptr && wantedOne && backend->Initialize(RhiApiValidation::Api | RhiApiValidation::Sync))
            {
                Backend = backend;
            }
            else
            {
                delete backend;
            }
        }
        MARLIN_REQUIREF(Backend != nullptr, "no RHI backend is available{0}", named ? " under that name" : "");
        RhiDeviceDesc desc;
        Result<RhiDevice *, RhiError> created = Backend->CreateDevice(desc);
        MARLIN_REQUIREF(created.IsOk(), "CreateDevice failed: {0}",
                        created.IsOk() ? "" : GetErrorName(created.Error()));
        Device = created.Value();
    }

    void TearDown() override
    {
        if (Device != nullptr)
        {
            Device->WaitIdle();
            Device->ProcessCompletions();
            Backend->DestroyDevice(Device);
            Device = nullptr;
        }
        if (Backend != nullptr)
        {
            Backend->Shutdown();
            delete Backend;
            Backend = nullptr;
        }
        Log::Flush();
        Log::RemoveSink(&Sink);
        for (const String &error : Sink.Errors)
        {
            Testing::Fail("no RHI errors", __FILE__, __LINE__, error.AsView());
        }
    }

    /// Submits *list* to its queue and waits for it.
    void SubmitAndWait(RhiCommandList *list)
    {
        const RhiQueueType queue = list->GetQueueType();
        RhiCommandList *lists[] = {list};
        RhiSubmitDesc submit;
        submit.CommandLists = lists;
        Result<uint64, RhiError> value = Device->Submit(queue, submit);
        MARLIN_REQUIRE(value.IsOk());
        MARLIN_REQUIRE(Device->Wait(Device->GetQueueFence(queue), value.Value(), 10000));
        Device->ProcessCompletions();
    }

    RhiBuffer CreateBuffer(uint64 size, RhiBufferUsage usage, RhiMemoryLocation location)
    {
        RhiBufferDesc desc;
        desc.Size = size;
        desc.Usage = usage;
        desc.Location = location;
        desc.DebugName = "TestBuffer";
        Result<RhiBuffer, RhiError> buffer = Device->CreateBuffer(desc);
        MARLIN_REQUIRE(buffer.IsOk());
        return buffer.Value();
    }

    RhiErrorSink Sink;
    IRhiBackend *Backend = nullptr;
    RhiDevice *Device = nullptr;
};
} // namespace Marlin::Tests
