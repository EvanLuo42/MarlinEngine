#pragma once

#include "Asset/AssetRef.h"
#include "CoreTypes.h"
#include "Input/InputAssets.h"
#include "Mesh/Mesh.h"
#include "RenderComponents/RenderComponents.gen.h"

namespace Marlin
{
/// The mesh sections a RenderMesh can draw with other materials than their defaults.
inline constexpr uint32 RenderMeshMaterialCount = 8;

/// A static mesh drawn at the entity's WorldTransform (Documentation/Architecture/07 §5.2): the first primitive of the
/// GPU Scene. Shared, so the entities of one mesh and one material list share chunks.
MSTRUCT(Component, Shared)
struct RenderMesh
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetRef<Marlin::Mesh> Mesh;
    /// Per mesh section (MeshSection, in the mesh's order), the material it draws with instead of its default; null
    /// keeps the default. Plain data, as entity templates hold components; overrides past the mesh's sections are
    /// reported and ignored.
    MPROPERTY()
    SoftAssetRef<Marlin::Material> Materials[RenderMeshMaterialCount];

    bool operator==(const RenderMesh &) const = default;
};

/// A camera at the entity's WorldTransform, looking down its -Z with Y up: what a game viewport copies into its
/// SceneView (Documentation/Architecture/07 §5.1).
MSTRUCT(Component)
struct Camera
{
    MGENERATED_BODY()

    /// The vertical field of view in radians.
    MPROPERTY()
    float32 VerticalFov = 1.0471976f;
    MPROPERTY()
    float32 Near = 0.1f;
};

/// Flies the entity freely from the input actions of its FlyCameraInput: FlyCameraSystem moves its LocalTransform, so
/// it goes on a Camera entity without a Parent. The first camera rig (Documentation/Architecture/07 §5.6): a debug
/// view until gameplay has cameras of its own.
MSTRUCT(Component)
struct FlyCamera
{
    MGENERATED_BODY()

    /// Metres per second; the Speed action changes it while flying.
    MPROPERTY()
    float32 Speed = 5;
    /// How much faster the Boost action flies.
    MPROPERTY()
    float32 BoostScale = 4;
};

/// What flies a FlyCamera (Documentation/Architecture/15 §3): the context FlyCameraSystem keeps active while one
/// exists, and the actions it reads. The engine's are in /Engine/Content/Input/FlyCamera: hold the right mouse
/// button to look and move with WASD, Q and E, Shift and the wheel, or use a gamepad's sticks and triggers. Shared,
/// as every camera of a world flies the same way.
MSTRUCT(Component, Shared)
struct FlyCameraInput
{
    MGENERATED_BODY()

    MPROPERTY()
    AssetRef<InputContext> Context;
    /// Axis3D: right, world up and backward, each from -1 to 1.
    MPROPERTY()
    AssetRef<InputAction> Move;
    /// Accumulate Axis2D: radians to turn right-handedly about world up (yaw) and the camera's right (pitch).
    MPROPERTY()
    AssetRef<InputAction> Look;
    /// Accumulate Axis1D: steps that each scale Speed by 1.2.
    MPROPERTY()
    AssetRef<InputAction> Speed;
    MPROPERTY()
    AssetRef<InputAction> Boost;
    /// Held while flying with the mouse: the pointer is locked meanwhile.
    MPROPERTY()
    AssetRef<InputAction> Engage;

    bool operator==(const FlyCameraInput &) const = default;
};

/// The sun: light from far away along the entity's -Z (Documentation/Architecture/07 §3). The first one of a world
/// lights it; no shadows yet.
MSTRUCT(Component)
struct DirectionalLight
{
    MGENERATED_BODY()

    MPROPERTY()
    Vector3 Color = Vector3::One;
    /// Lux on a surface facing it: 100000 for noon sun.
    MPROPERTY()
    float32 Illuminance = 100000;
};

/// Ambient light from the whole sky, the same from every direction (07 §3): the first one of a world lights it.
MSTRUCT(Component)
struct SkyLight
{
    MGENERATED_BODY()

    MPROPERTY()
    Vector3 Color = Vector3::One;
    /// Lux on an upward surface from the sky alone.
    MPROPERTY()
    float32 Illuminance = 20000;
};

/// The entity's slot in its world's RenderScene, which RenderMeshExtractSystem adds and keeps: the InstanceId the GPU
/// Scene, culling and the visibility buffer know the entity by. It is computed, never authored.
MSTRUCT(Component)
struct RenderInstance
{
    MGENERATED_BODY()

    MPROPERTY()
    uint32 Slot = ~0u;
};
} // namespace Marlin
