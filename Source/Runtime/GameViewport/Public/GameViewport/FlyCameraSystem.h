#pragma once

#include "CoreTypes.h"
#include "RenderComponents/RenderComponents.h"
#include "World/EntityQuery.h"
#include "World/Hierarchy.h"
#include "World/System.h"
#include "GameViewport/FlyCameraSystem.gen.h"

namespace Marlin
{
/// Flies the FlyCamera entities from their FlyCameraInput actions, keeping the first one's context active and the
/// pointer locked while its Engage action is down. It writes the WorldTransform along with the LocalTransform, so the
/// view follows in the frame the input came in rather than after the next fixed step's propagation; FlyCamera
/// entities have no Parent for that.
MCLASS(Phase = CameraPhase)
class GAMEVIEWPORT_API FlyCameraSystem final : public ISystem
{
    MGENERATED_BODY()
public:
    ~FlyCameraSystem() override;

    void Configure(SystemBuilder &builder) override;
    void Execute(SystemContext &context) override;

private:
    /// Keeps *context* the one this system has pushed, popping the one before.
    void UseContext(const AssetRef<InputContext> &context);

    /// FlyCameraInput is shared, so it is read per chunk rather than as a column.
    EntityQuery<Write<FlyCamera>, Write<LocalTransform>, Write<WorldTransform>, WithAny<FlyCameraInput>,
                Without<Parent>>
        _cameras;
    AssetRef<InputContext> _pushed;
    bool _locked = false;
};
} // namespace Marlin
