#pragma once

#include "CoreTypes.h"
#include "Math/Quaternion.h"
#include "Math/WorldPosition.h"
#include "Memory/UniquePtr.h"
#include "Reflection/TypeOf.h"

namespace Marlin
{
class RenderScene;
struct SceneViewState;

/// What a SceneView sees from: a plain camera the owner sets each frame, such as an editor viewport, or a system that
/// copies a Camera component (Documentation/Architecture/07 §5.1). Projection is perspective, reversed-Z, with an
/// infinite far plane.
struct SceneCamera
{
    WorldPosition Position;
    /// Rotates Vector3::Forward (-Z) to where the camera looks; Vector3::Up is its up.
    Quaternion Rotation = Quaternion::Identity;
    /// The vertical field of view in radians.
    float32 VerticalFov = 1.0471976f;
    float32 Near = 0.1f;
};

/// One observer of a RenderScene (Documentation/Architecture/07 §5.1): a camera, a size, a display mode and the frame
/// image slot its output is bound to. The owner (a game viewport, an editor viewport, an asset preview) keeps it
/// alive and updates it on the main thread; it registers with its scene on construction. The UI draws its slot with
/// `DrawImage(UIImage::FromFrameSlot(GetImageSlot()), rect)`; the image is bound from the frame the view was set up
/// in, so a viewport sets the size it draws at in the same frame.
class SCENERENDERER_API SceneView
{
public:
    explicit SceneView(RenderScene &scene);
    ~SceneView();
    MARLIN_NONCOPYABLE(SceneView);

    [[nodiscard]] RenderScene &GetScene() const
    {
        return *_scene;
    }

    void SetCamera(const SceneCamera &camera)
    {
        _camera = camera;
    }

    [[nodiscard]] const SceneCamera &GetCamera() const
    {
        return _camera;
    }

    /// In pixels; a view of zero size renders nothing.
    void SetSize(uint32 width, uint32 height)
    {
        _width = width;
        _height = height;
    }

    [[nodiscard]] uint32 GetWidth() const
    {
        return _width;
    }

    [[nodiscard]] uint32 GetHeight() const
    {
        return _height;
    }

    /// A SceneViewMode type (07 §5.4); null for the default mode.
    void SetMode(const TypeInfo *mode)
    {
        _mode = mode;
    }

    template <typename T> void SetMode()
    {
        _mode = &TypeOf<T>();
    }

    [[nodiscard]] const TypeInfo *GetMode() const
    {
        return _mode;
    }

    /// The frame image slot the view's output is bound to.
    [[nodiscard]] uint32 GetImageSlot() const
    {
        return _slot;
    }

    /// What the render job keeps for the view from frame to frame. Render job only.
    [[nodiscard]] SceneViewState &GetState() const
    {
        return *_state;
    }

private:
    RenderScene *_scene;
    SceneCamera _camera;
    uint32 _width = 0;
    uint32 _height = 0;
    const TypeInfo *_mode = nullptr;
    uint32 _slot = 0;
    UniquePtr<SceneViewState> _state;
};
} // namespace Marlin
