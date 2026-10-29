#include "Asset/AssetSubsystem.h"

#include "Asset/Assets.h"
#include "Asset/PakAssetStore.h"
#include "Engine/Engine.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
uint64 AssetSubsystem::StreamingBytes = 64 * 1024 * 1024; // RECOVERY-DECISION D1: default budget chosen in recovery.
uint32 AssetSubsystem::StreamingRequests = 16;            // RECOVERY-DECISION D1: default budget chosen in recovery.
String AssetSubsystem::RecordLoadOrder;

void AssetSubsystem::Initialize(SubsystemContext & /*context*/)
{
    Assets::Initialize();
    // Other subsystems add their stores (GlobalStore, shaders, scripts) as they initialize; loads they ask for before
    // then wait for all of them.
    Assets::HoldLocates();
    for (const Engine::MountedPackage &package : Engine::GetPackages())
    {
        _stores.Add(Assets::AddStore(MakeUnique<PakAssetStore>(package.Pak), package.Priority));
    }
    BulkStreamerDesc desc;
    desc.MaxInFlightBytes = StreamingBytes;
    desc.MaxInFlightRequests = StreamingRequests;
    _streamer = MakeUnique<BulkStreamer>(desc);
    if (!RecordLoadOrder.IsEmpty())
    {
        Assets::StartRecordingLoadOrder();
    }
}

void AssetSubsystem::Start()
{
    Assets::ReleaseLocates();
}

void AssetSubsystem::Deinitialize()
{
    _streamer->Flush();
    _streamer.Reset();
    if (!RecordLoadOrder.IsEmpty())
    {
        StringBuilder<4096> text;
        for (const Guid &guid : Assets::StopRecordingLoadOrder())
        {
            text.Append(Format("{0}\n", guid));
        }
        if (!Vfs::WriteAll(RecordLoadOrder.AsView(), text.AsView()))
        {
            MARLIN_LOG(LogAsset, Warning, "The load order cannot be written to {0}", RecordLoadOrder);
        }
    }
    for (const AssetStoreId store : _stores)
    {
        Assets::RemoveStore(store);
    }
    _stores.Clear();
    Assets::Shutdown();
}

void AssetSubsystem::BeginFrame(const FrameTime & /*time*/)
{
    Assets::ProcessFrame();
    _streamer->ProcessFrame();
}

void AssetSubsystem::EndFrame(const FrameTime & /*time*/)
{
    Assets::CollectGarbage();
}
} // namespace Marlin
