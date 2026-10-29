#include "Testing/Testing.h"

#include "ShaderPipeline/ShaderCompiler.h"
#include "spirv-tools/libspirv.h"

namespace Marlin::Tests
{
namespace
{
String Validate(Span<const uint8> code)
{
    spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_4);
    spv_diagnostic diagnostic = nullptr;
    spv_const_binary_t binary{reinterpret_cast<const uint32 *>(code.Data()), code.Count() / sizeof(uint32)};
    const spv_result_t result = spvValidate(context, &binary, &diagnostic);
    String message;
    if (result != SPV_SUCCESS)
    {
        message = diagnostic != nullptr && diagnostic->error != nullptr ? String(diagnostic->error)
                                                                        : String("spirv-val failed");
    }
    spvDiagnosticDestroy(diagnostic);
    spvContextDestroy(context);
    return message;
}
} // namespace

MARLIN_TEST(ShaderPipeline, UiPainterShadersCompile)
{
    ShaderProfile profile;
    profile.Name = "Vulkan.Baseline";
    profile.Rhi = "Vulkan";
    profile.Target = "spirv";
    constexpr StringView EntryPoints[] = {
        "Marlin.Passes.Ui.Count",         "Marlin.Passes.Ui.Scan",        "Marlin.Passes.Ui.Scatter",
        "Marlin.Passes.Ui.RasterSurface", "Marlin.Passes.Ui.RasterLayer", "Marlin.Passes.Ui.RasterGlyphs",
        "Marlin.Passes.Ui.Blur",
    };
    for (const StringView entryPoint : EntryPoints)
    {
        const ShaderCompileResult result = CompileShader(entryPoint, profile, {});
        MARLIN_EXPECTF(result.Succeeded, "{0}:\n{1}", entryPoint, result.Errors);
        if (result.Succeeded && !result.Unavailable)
        {
            const String error = Validate(result.Variant.Code.AsSpan());
            MARLIN_EXPECTF(error.IsEmpty(), "{0}: {1}", entryPoint, error);
            MARLIN_EXPECT(!result.Dependencies.IsEmpty());
        }
    }
}

MARLIN_TEST(ShaderPipeline, MaterialPassesCompilePerMaterial)
{
    ShaderProfile profile;
    profile.Name = "Vulkan.Baseline";
    profile.Rhi = "Vulkan";
    profile.Target = "spirv";
    // A material pass compiles for a material named in its options, and is no global shader.
    ShaderCompileOptions options;
    options.Material = "Marlin.Materials.Default";
    const ShaderCompileResult result = CompileShader("Marlin.Tests.EvaluateMaterial", profile, options);
    MARLIN_REQUIREF(result.Succeeded, "{0}", result.Errors);
    MARLIN_EXPECT(Validate(result.Variant.Code.AsSpan()).IsEmpty());
    bool usesMaterial = false;
    for (const ShaderSourceDependency &dependency : result.Dependencies)
    {
        usesMaterial = usesMaterial || dependency.Path.AsView().EndsWith("Materials/Default.mshader");
    }
    MARLIN_EXPECT(usesMaterial);
    const ShaderCompileResult missing = CompileShader("Marlin.Tests.EvaluateMaterial", profile, {});
    MARLIN_EXPECT(!missing.Succeeded);
    List<String> entryPoints;
    String errors;
    MARLIN_REQUIREF(GetGlobalShaderEntryPoints(entryPoints, errors), "{0}", errors);
    MARLIN_EXPECT(!entryPoints.Contains(String("Marlin.Tests.EvaluateMaterial")));

    // The key of a material pass names its entry point, material and sorted variant, and reads back.
    const ShaderVariantValue variant[] = {{String("UseTint"), 1}, {String("Mode"), 2}};
    const String key = MakeMaterialShaderKey("Marlin.Tests.EvaluateMaterial", "Project.Materials.Rock", variant);
    MARLIN_EXPECTF(key == "Marlin.Tests.EvaluateMaterial|Project.Materials.Rock|Mode=2;UseTint=1;", "{0}", key);
    MARLIN_EXPECT(GetShaderKeyEntryPoint(key.AsView()) == "Marlin.Tests.EvaluateMaterial");
    StringView material;
    List<ShaderVariantValue> parsed;
    MARLIN_REQUIRE(ParseShaderKey(key.AsView(), material, parsed));
    MARLIN_EXPECT(material == "Project.Materials.Rock" && parsed.Count() == 2 && parsed[0].Name == "Mode" &&
                  parsed[0].Value == 2 && parsed[1].Value == 1);
    MARLIN_EXPECT(ParseShaderKey("Marlin.Passes.Ui.Blur", material, parsed) && material.IsEmpty() && parsed.IsEmpty());
    MARLIN_EXPECT(GetGlobalShaderGuid(key.AsView()) != GetGlobalShaderGuid("Marlin.Tests.EvaluateMaterial"));
}

MARLIN_TEST(ShaderPipeline, MaterialAndLightingPassesCompile)
{
    ShaderProfile profile;
    profile.Name = "Vulkan.Baseline";
    profile.Rhi = "Vulkan";
    profile.Target = "spirv";
    constexpr StringView EntryPoints[] = {
        "Marlin.Passes.Material.Classify",        "Marlin.Passes.Material.BuildResolveArguments",
        "Marlin.Passes.Material.BuildTileLists",  "Marlin.Passes.Material.BuildLightArguments",
        "Marlin.Passes.Lighting.LightSimple",     "Marlin.Passes.Lighting.LightSingle",
        "Marlin.Passes.Lighting.LightComplex",    "Marlin.Passes.Post.Tonemap",
    };
    for (const StringView entryPoint : EntryPoints)
    {
        const ShaderCompileResult result = CompileShader(entryPoint, profile, {});
        MARLIN_EXPECTF(result.Succeeded, "{0}:\n{1}", entryPoint, result.Errors);
        if (result.Succeeded)
        {
            const String error = Validate(result.Variant.Code.AsSpan());
            MARLIN_EXPECTF(error.IsEmpty(), "{0}: {1}", entryPoint, error);
        }
    }
    // The material pass for the Default material, and a custom BSDF's lighting for the test BSDF.
    ShaderCompileOptions material;
    material.Material = "Marlin.Materials.Default";
    const ShaderCompileResult resolve = CompileShader("Marlin.Passes.Material.Resolve", profile, material);
    MARLIN_EXPECTF(resolve.Succeeded, "{0}", resolve.Errors);
    MARLIN_EXPECT(!resolve.Succeeded || Validate(resolve.Variant.Code.AsSpan()).IsEmpty());
    ShaderCompileOptions bsdf;
    bsdf.Material = "Marlin.Materials.ToonBsdf";
    const ShaderCompileResult custom = CompileShader("Marlin.Passes.Lighting.LightCustom", profile, bsdf);
    MARLIN_EXPECTF(custom.Succeeded, "{0}", custom.Errors);
    MARLIN_EXPECT(!custom.Succeeded || Validate(custom.Variant.Code.AsSpan()).IsEmpty());
}

MARLIN_TEST(ShaderPipeline, ProfilesShareVariantsByTheFeaturesUsed)
{
    ShaderProfile baseline;
    baseline.Name = "Vulkan.Baseline";
    baseline.Target = "spirv";
    ShaderProfile reorder = baseline;
    reorder.Name = "Vulkan.Reorder";
    reorder.Features.Add(String("ShaderExecutionReorder"));
    ShaderProfile both = reorder;
    both.Name = "Vulkan.Both";
    both.Features.Add(String("CooperativeVector"));
    ShaderProfile dxil = baseline;
    dxil.Target = "dxil";

    MARLIN_EXPECT(SharesVariant(baseline, reorder, {}));
    MARLIN_EXPECT(!SharesVariant(baseline, dxil, {}));
    const String reorderOnly[] = {String("ShaderExecutionReorder")};
    MARLIN_EXPECT(!SharesVariant(baseline, reorder, Span<const String>(reorderOnly)));
    MARLIN_EXPECT(SharesVariant(reorder, both, Span<const String>(reorderOnly)));
    const String cooperative[] = {String("CooperativeVector")};
    MARLIN_EXPECT(SharesVariant(baseline, reorder, Span<const String>(cooperative)));
    MARLIN_EXPECT(!SharesVariant(reorder, both, Span<const String>(cooperative)));

    const ShaderCompileResult blur = CompileShader("Marlin.Passes.Ui.Blur", baseline, {});
    MARLIN_REQUIREF(blur.Succeeded, "{0}", blur.Errors);
    MARLIN_EXPECT(blur.Features.IsEmpty());
    MARLIN_EXPECT(SharesVariant(baseline, both, blur.Features.AsSpan()));
}
} // namespace Marlin::Tests
