#pragma once

#include "Asset/Assets.h"
#include "Asset/BulkStreamer.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"
#include "Asset/AssetSubsystem.gen.h"

namespace Marlin
{
/// Runs the asset table: completion callbacks at the start of each frame, freeing unreferenced assets at the end. A
/// packaged engine's packages become its stores. Owns the BulkStreamer that renderers stream mips and mesh pages
/// through.
MCLASS(Config = "Engine.Assets", Dependencies = "JobSubsystem")
class ASSET_API AssetSubsystem final : public EngineSubsystem
{
    MGENERATED_BODY()
public:
    void Initialize(SubsystemContext &context) override;
    /// Every subsystem has initialized and added its stores: the loads asked for until now start.
    void Start() override;
    void Deinitialize() override;
    void BeginFrame(const FrameTime &time) override;
    void EndFrame(const FrameTime &time) override;

    /// The in-flight budget of the bulk streamer.
    MPROPERTY()
    static uint64 StreamingBytes;
    MPROPERTY()
    static uint32 StreamingRequests;
    /// A virtual path, such as /Saved/LoadOrder.txt, to write the Guids of the assets loaded in this run to, in the
    /// order they started loading, for MarlinPackager -LoadOrder; empty records nothing.
    MPROPERTY()
    static String RecordLoadOrder;

    [[nodiscard]] BulkStreamer &GetStreamer()
    {
        return *_streamer;
    }

private:
    List<AssetStoreId> _stores;
    UniquePtr<BulkStreamer> _streamer;
};
} // namespace Marlin
