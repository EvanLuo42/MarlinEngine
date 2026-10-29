#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirReflection.h"
#include "LanguageIR/MirTarget.h"
#include "Memory/MemoryOps.h"
#include "RHI/RhiTestDevice.h"
#include "Reflection/TypeInfo.h"
#include "SceneRenderer/SceneGpuTypes.h"
#include "SceneRenderer/SceneRendererSubsystem.h"
#include "SceneRenderer/SceneShaders.h"
#include "ShaderBackendSpirv/SpirvTarget.h"
#include "String/StringBuilder.h"
#include "spirv-tools/libspirv.h"

namespace Marlin::Tests
{
namespace
{
/// A scene pass entry point of the mounted engine's source set compiled the way the shader pipeline compiles it and
/// validated for Vulkan 1.4; empty Errors if all went well. Problems lists how the C++ arguments differ from the
/// entry point's [Uniform] parameter block.
struct CompiledSceneShader
{
    explicit CompiledSceneShader(const ShaderDeclaration &shader)
    {
        static constexpr StringView Roots[] = {"/Engine"};
        VfsSourceProvider sources{Roots};
        // As MarlinConverter compiles global shaders for development builds.
        LanguageCompileOptions options;
        options.DebugOutput = true;
        options.CheckSpans = true;
        options.DebugInfo = true;
        LanguageCompilation compilation(sources, options);
        MirModule module;
        if (!compilation.Load() || !compilation.CompileEntryPoint(shader.EntryPoint, module))
        {
            Errors = compilation.GetDiagnostics().ToString();
            return;
        }
        // The [Uniform] block as the shader pipeline reflects it, without the debug buffer's address (§9.11).
        MirReflection reflection;
        ReflectMirEntryPoint(module, *module.GetEntryPoints()[0], reflection);
        StringBuilder<512> problems;
        if (shader.Arguments != nullptr && reflection.ArgumentsSize != 0)
        {
            const TypeInfo &type = shader.Arguments();
            if (reflection.ArgumentsSize != shader.ArgumentsSize)
            {
                FormatTo(problems, "{0} bytes against {1}; ", reflection.ArgumentsSize, shader.ArgumentsSize);
            }
            for (const FieldInfo *field : type.GetFields())
            {
                const MirReflectedField *match = nullptr;
                for (const MirReflectedField &candidate : reflection.Arguments)
                {
                    match = candidate.Name == field->GetName() ? &candidate : match;
                }
                if (match == nullptr || match->Offset != field->GetOffset() ||
                    match->Size != field->GetType().GetSize())
                {
                    FormatTo(problems, "{0}; ", field->GetName());
                }
            }
        }
        Problems = String(problems.AsView());
        MirTargetRegistry targets;
        RegisterSpirvTarget(targets);
        const MirTarget *target = targets.Find(SpirvTargetName);
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        MirModulePassManager passes({.VerifyEach = true});
        String error;
        registry.ParsePipeline(MirShaderPipeline, passes, error, {.VerifyEach = true});
        List<String> errors;
        target->AddLegalizationPasses(passes, errors);
        MirModuleAnalysisManager analyses;
        passes.Run(module, analyses);
        if (!passes.GetErrors().IsEmpty() || !errors.IsEmpty())
        {
            Errors = String(!errors.IsEmpty() ? errors[0].AsView() : passes.GetErrors()[0].AsView());
            return;
        }
        if (!target->Emit(module, {}, Code, errors))
        {
            Errors = String(errors.IsEmpty() ? StringView("emission failed") : errors[0].AsView());
            return;
        }
        spv_context context = spvContextCreate(SPV_ENV_VULKAN_1_4);
        spv_diagnostic diagnostic = nullptr;
        spv_const_binary_t binary{reinterpret_cast<const uint32 *>(Code.Data()), Code.Count() / sizeof(uint32)};
        if (spvValidate(context, &binary, &diagnostic) != SPV_SUCCESS)
        {
            Errors = diagnostic != nullptr && diagnostic->error != nullptr ? String(diagnostic->error)
                                                                           : String("spirv-val failed");
        }
        spvDiagnosticDestroy(diagnostic);
        spvContextDestroy(context);
    }

    String Errors;
    String Problems;
    List<uint8> Code;
};

const ShaderDeclaration *const SceneShaders[] = {
    &SceneVisBufferMeshShader, &SceneVisBufferPixelShader, &SceneSoftwareRasterShader, &SceneScatterShader,
    &SceneLodInstanceShader,   &SceneTraverseShader,       &SceneGroupArgumentsShader, &SceneCullGroupsShader,
    &SceneCandidateShader,     &SceneHzbFromVisBufferShader, &SceneHzbMipShader,       &SceneRasterArgumentsShader,
    &SceneVisualizeShader};
} // namespace

MARLIN_TEST(SceneRenderer, PassShadersCompileAndMatchTheirArguments)
{
    for (const ShaderDeclaration *shader : SceneShaders)
    {
        const CompiledSceneShader compiled(*shader);
        MARLIN_EXPECTF(compiled.Errors.IsEmpty(), "{0}:\n{1}", shader->EntryPoint, compiled.Errors);
        MARLIN_EXPECTF(compiled.Problems.IsEmpty(), "{0}: {1}", shader->EntryPoint, compiled.Problems);
    }
}

MARLIN_TEST_F(RhiDeviceFixture, SceneRenderer, PassPipelinesCompileOnTheDevice, Gpu)
{
    List<CompiledSceneShader> compiled;
    for (const ShaderDeclaration *shader : SceneShaders)
    {
        compiled.Emplace(*shader);
        MARLIN_REQUIREF(compiled.Last().Errors.IsEmpty(), "{0}", shader->EntryPoint);
    }
    for (usize index = 0; index < compiled.Count(); ++index)
    {
        const ShaderDeclaration &shader = *SceneShaders[index];
        if (shader.Stage != ShaderStage::Compute)
        {
            continue;
        }
        RhiComputePipelineDesc desc;
        desc.Shader = {compiled[index].Code.AsSpan(), shader.EntryPoint, RhiShaderStage::Compute, {}};
        desc.DebugName = shader.EntryPoint;
        const RhiPipeline pipeline = Device->CreatePipeline(desc);
        MARLIN_EXPECTF(Device->WaitForPipeline(pipeline) == RhiPipelineState::Ready, "{0}", shader.EntryPoint);
        Device->Destroy(pipeline);
    }
    const RhiShader stages[] = {
        {compiled[0].Code.AsSpan(), SceneVisBufferMeshShader.EntryPoint, RhiShaderStage::Mesh, {}},
        {compiled[1].Code.AsSpan(), SceneVisBufferPixelShader.EntryPoint, RhiShaderStage::Pixel, {}}};
    RhiGraphicsPipelineDesc desc;
    desc.Shaders = stages;
    desc.Raster.Cull = RhiCullMode::None;
    desc.DebugName = "VisBuffer";
    const RhiPipeline pipeline = Device->CreatePipeline(desc);
    MARLIN_EXPECT(Device->WaitForPipeline(pipeline) == RhiPipelineState::Ready);
    Device->Destroy(pipeline);
}

MARLIN_TEST_F(RhiDeviceFixture, SceneRenderer, ScatterWritesWholeInstances, Gpu)
{
    const CompiledSceneShader compiled(SceneScatterShader);
    MARLIN_REQUIRE(compiled.Errors.IsEmpty());
    RhiComputePipelineDesc pipelineDesc;
    pipelineDesc.Shader = {compiled.Code.AsSpan(), SceneScatterShader.EntryPoint, RhiShaderStage::Compute, {}};
    const RhiPipeline pipeline = Device->CreatePipeline(pipelineDesc);
    MARLIN_REQUIRE(Device->WaitForPipeline(pipeline) == RhiPipelineState::Ready);

    // Two updates into slots 2 and 0 of four; slots 1 and 3 keep their fill.
    SceneGpuInstanceUpdate updates[2];
    for (uint32 index = 0; index < 2; ++index)
    {
        SceneGpuInstance &instance = updates[index].Instance;
        updates[index].Slot = index == 0 ? 2 : 0;
        instance.Tile[0] = -5 - static_cast<int32>(index);
        instance.Tile[1] = 7;
        instance.Tile[2] = 1 << 20;
        instance.Mesh = 40 + index;
        instance.Rotation[0] = 0.25f;
        instance.Rotation[3] = 0.75f + static_cast<float32>(index);
        instance.Offset[1] = 512.5f;
        instance.Flags = 0xABCD;
        instance.Scale[2] = 3;
    }
    const RhiBuffer source = CreateBuffer(sizeof(updates), RhiBufferUsage::ShaderResource, RhiMemoryLocation::Upload);
    MemCopy(Device->GetMappedData(source), updates, sizeof(updates));
    constexpr uint64 Size = 4 * sizeof(SceneGpuInstance);
    const RhiBuffer target =
        CreateBuffer(Size, RhiBufferUsage::UnorderedAccess | RhiBufferUsage::CopySource | RhiBufferUsage::CopyDest,
                     RhiMemoryLocation::Device);
    const RhiBuffer readback = CreateBuffer(Size, RhiBufferUsage::CopyDest, RhiMemoryLocation::Readback);

    RhiCommandList *list = Device->BeginCommandList(RhiQueueType::Graphics);
    list->FillBuffer(target, 0, Size, 0x11111111);
    const auto barrier = [&](RhiSync before, RhiAccess beforeAccess, RhiSync after, RhiAccess afterAccess) {
        RhiGlobalBarrier global;
        global.SyncBefore = before;
        global.AccessBefore = beforeAccess;
        global.SyncAfter = after;
        global.AccessAfter = afterAccess;
        RhiBarriers barriers;
        barriers.Global = Span<const RhiGlobalBarrier>(&global, 1);
        list->Barrier(barriers);
    };
    barrier(RhiSync::Copy, RhiAccess::CopyDest, RhiSync::ComputeShading,
            RhiAccess::ShaderResource | RhiAccess::UnorderedAccess);
    SceneScatterArguments arguments;
    arguments.Updates = {.Pointer = Device->GetBufferAddress(source), .Length = 2};
    arguments.Instances = {.Pointer = Device->GetBufferAddress(target), .Length = 4};
    arguments.UpdateCount = 2;
    list->SetPipeline(pipeline);
    list->SetShaderArguments(Span<const uint8>(reinterpret_cast<const uint8 *>(&arguments), sizeof(arguments)));
    list->Dispatch(1, 1, 1);
    barrier(RhiSync::ComputeShading, RhiAccess::UnorderedAccess, RhiSync::Copy, RhiAccess::CopySource);
    list->CopyBuffer(readback, 0, target, 0, Size);
    SubmitAndWait(list);

    const auto *result = reinterpret_cast<const SceneGpuInstance *>(Device->GetMappedData(readback));
    MARLIN_EXPECT(MemCompare(&result[2], &updates[0].Instance, sizeof(SceneGpuInstance)) == 0);
    MARLIN_EXPECT(MemCompare(&result[0], &updates[1].Instance, sizeof(SceneGpuInstance)) == 0);
    const auto *untouched = reinterpret_cast<const uint32 *>(&result[1]);
    MARLIN_EXPECTF(untouched[0] == 0x11111111u && untouched[15] == 0x11111111u, "slot 1 holds {0:x}", untouched[0]);
    MARLIN_EXPECTF(result[2].Mesh == 40 && result[2].Tile[0] == -5 && result[2].Rotation[3] == 0.75f &&
                       result[2].Offset[1] == 512.5f && result[2].Flags == 0xABCD && result[2].Scale[2] == 3,
                   "slot 2: mesh {0} tile {1} w {2} offset {3} flags {4:x} scale {5}", result[2].Mesh,
                   result[2].Tile[0], result[2].Rotation[3], result[2].Offset[1], result[2].Flags, result[2].Scale[2]);
    Device->Destroy(pipeline);
    for (const RhiBuffer buffer : {source, target, readback})
    {
        Device->Destroy(buffer);
    }
}

MARLIN_TEST(SceneRenderer, SplitsWorldPositionsIntoTiles)
{
    const SceneTilePosition near = SceneTilePosition::FromWorld(WorldPosition(1.5, -0.25, 1023.75));
    MARLIN_EXPECT(near.Tile[0] == 0 && near.Tile[1] == -1 && near.Tile[2] == 0);
    MARLIN_EXPECT(near.Offset[0] == 1.5f && near.Offset[1] == 1023.75f && near.Offset[2] == 1023.75f);
    // Far from the origin the offset keeps what a float near zero keeps.
    const SceneTilePosition far = SceneTilePosition::FromWorld(WorldPosition(1.0e9 + 0.125, 0, -3.0e8 - 0.5));
    MARLIN_EXPECT(static_cast<float64>(far.Tile[0]) * SceneTileSize + far.Offset[0] == 1.0e9 + 0.125);
    MARLIN_EXPECT(static_cast<float64>(far.Tile[2]) * SceneTileSize + far.Offset[2] == -3.0e8 - 0.5);
    MARLIN_EXPECT(far.Offset[0] >= 0 && far.Offset[0] < SceneTileSize);
}

MARLIN_TEST(SceneRenderer, AllocatesAndMergesRanges)
{
    SceneRangeAllocator ranges;
    ranges.Reset(256);
    uint64 a = 0;
    uint64 b = 0;
    uint64 c = 0;
    MARLIN_REQUIRE(ranges.Allocate(10, 16, a) && a == 0);
    MARLIN_REQUIRE(ranges.Allocate(100, 16, b) && b == 16);
    MARLIN_REQUIRE(ranges.Allocate(100, 16, c) && c == 128);
    uint64 none = 0;
    MARLIN_EXPECT(!ranges.Allocate(64, 16, none));
    ranges.Free(b, 100);
    ranges.Free(a, 10);
    // The gap left after the first allocation's alignment merges with both freed ranges.
    uint64 merged = 0;
    MARLIN_EXPECT(ranges.Allocate(116, 1, merged) && merged == 0);
    ranges.Free(merged, 116);
    ranges.Free(c, 100);
    uint64 whole = 0;
    MARLIN_EXPECT(ranges.Allocate(256, 1, whole) && whole == 0);
}
} // namespace Marlin::Tests
