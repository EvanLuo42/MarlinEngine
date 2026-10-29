#pragma once

#include "CoreTypes.h"
#include "SceneRenderer/SceneViewFeature.h"
#include "SceneRenderer/SceneMaterialFeatures.gen.h"

namespace Marlin
{
/// Material Classification and Material Resolve (Documentation/Architecture/07 §3, §4.2): each pixel's material
/// permutation, the tiles of each, and a material pass per permutation that writes the pixels' closures.
MCLASS(Stage = SceneMaterialStage)
class SCENERENDERER_API SceneMaterialFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// The first lighting (07 §3): the sun and the ambient over the closures, a pass per tile class and one per custom
/// BSDF in use (07 §4.3), into the view's HDR radiance.
MCLASS(Stage = SceneLightingStage)
class SCENERENDERER_API SceneLightingFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// Exposure and a filmic curve from the HDR radiance into the view's scene color, in the UI range.
MCLASS(Stage = ScenePostStage)
class SCENERENDERER_API SceneTonemapFeature final : public SceneViewFeature
{
    MGENERATED_BODY()
public:
    void AddPasses(SceneViewContext &context) override;
};

/// Materials lit by the scene's lights: what views show by default (a view with no mode shows it too).
MCLASS()
class SCENERENDERER_API SceneLitViewMode final : public SceneViewMode
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetDisplayName() const override
    {
        return "Lit";
    }
};
} // namespace Marlin
