#pragma once

#include "Concurrency/Atomic.h"
#include "CoreTypes.h"
#include "RHI/RhiPipeline.h"
#include "Reflection/TypeOf.h"
#include "Shader/GlobalShader.h"
#include "String/StringView.h"
#include "Templates/TypeTraits.h"

namespace Marlin
{
/// ShaderLibrary's entry for a declaration, so a dispatch finds it without hashing the name or taking a lock. The
/// entry belongs to the library of *Generation*, a number no other library instance uses.
struct ShaderDeclarationCache
{
    Atomic<uint64> Generation{0};
    Atomic<void *> Entry{nullptr};
};

/// A global shader as C++ names it: its entry point (MarlinLanguage.md §1.6) and the [Uniform] arguments C++ passes it.
struct ShaderDeclaration
{
    /// The entry point's qualified name, such as `Marlin.Passes.Ui.Raster`.
    StringView EntryPoint;
    ShaderStage Stage = ShaderStage::Compute;
    /// The reflected C++ struct of the entry point's `[Uniform]` parameters; null for an entry point without them.
    TypeGetter Arguments = nullptr;
    uint32 ArgumentsSize = 0;
    /// Written by ShaderLibrary; declarations are constexpr, so it is mutable.
    mutable ShaderDeclarationCache Cache{};
};

/// A compute entry point of a global shader, declared once beside the pass that dispatches it:
///
///     MSTRUCT() struct UiRasterArguments { MGENERATED_BODY() MPROPERTY() uint32 Commands = 0; };
///     inline constexpr ComputeShader<UiRasterArguments> UiRasterShader{"Marlin.Passes.Ui.Raster"};
///
///     shaders.Dispatch(list, UiRasterShader, arguments, groups);
///
/// TArgs must lay out like the entry point's `[Uniform]` parameter block: the same members by name, offset and size
/// (MarlinLanguage.md §10); ShaderLibrary checks that when the shader loads and refuses to dispatch a mismatch. A block
/// larger than the RhiShaderArgumentsSize bytes passed directly is copied into upload memory, and the shader reads it
/// through its address (§9.3); the caller writes TArgs either way.
template <typename TArgs> struct ComputeShader final : ShaderDeclaration
{
    constexpr explicit ComputeShader(StringView entryPoint)
        : ShaderDeclaration{entryPoint, ShaderStage::Compute, &TypeOf<TArgs>, static_cast<uint32>(sizeof(TArgs))}
    {
    }
};

/// A compute entry point that takes no [Uniform] arguments.
struct ComputeShaderNoArguments final : ShaderDeclaration
{
    constexpr explicit ComputeShaderNoArguments(StringView entryPoint)
        : ShaderDeclaration{entryPoint, ShaderStage::Compute, nullptr, 0}
    {
    }
};

/// The arguments of graphics stages that take no [Uniform] arguments, such as `PixelShader<NoShaderArguments>`.
struct NoShaderArguments
{
};

/// A stage of a graphics pipeline of global shaders. The stages of one pipeline share its [Uniform] arguments: each that
/// reads them declares the same TArgs, and a stage that reads none may leave its `[Uniform]` parameters out.
///
///     inline constexpr MeshShader<VisBufferArguments> VisBufferMesh{"Marlin.Passes.Scene.VisBufferMesh"};
///     inline constexpr PixelShader<VisBufferArguments> VisBufferPixel{"Marlin.Passes.Scene.VisBufferPixel"};
template <ShaderStage TStage, typename TArgs> struct GraphicsStageShader final : ShaderDeclaration
{
    constexpr explicit GraphicsStageShader(StringView entryPoint)
        : ShaderDeclaration{entryPoint, TStage, GetArguments(), GetArgumentsSize()}
    {
    }

private:
    static constexpr TypeGetter GetArguments()
    {
        if constexpr (IsSame<TArgs, NoShaderArguments>)
        {
            return nullptr;
        }
        else
        {
            return &TypeOf<TArgs>;
        }
    }

    static constexpr uint32 GetArgumentsSize()
    {
        return IsSame<TArgs, NoShaderArguments> ? 0 : static_cast<uint32>(sizeof(TArgs));
    }
};

template <typename TArgs> using TaskShader = GraphicsStageShader<ShaderStage::Task, TArgs>;
template <typename TArgs> using MeshShader = GraphicsStageShader<ShaderStage::Mesh, TArgs>;
template <typename TArgs> using VertexShader = GraphicsStageShader<ShaderStage::Vertex, TArgs>;
template <typename TArgs> using PixelShader = GraphicsStageShader<ShaderStage::Pixel, TArgs>;

/// A graphics pipeline of global shaders: Task (optional) and Mesh, or Vertex; then Pixel (optional); with the fixed
/// state and target formats it is made for. The pass that draws with it fills one in, typically as a constant, and
/// ShaderLibrary makes one pipeline per distinct desc and code of its stages.
struct ShaderGraphicsPipelineDesc
{
    const ShaderDeclaration *Task = nullptr;
    const ShaderDeclaration *Mesh = nullptr;
    const ShaderDeclaration *Vertex = nullptr;
    const ShaderDeclaration *Pixel = nullptr;
    RhiPrimitiveTopology Topology = RhiPrimitiveTopology::TriangleList;
    RhiRasterState Raster;
    RhiDepthStencilState DepthStencil;
    RhiBlendTarget Blend[RhiMaxRenderTargets];
    bool AlphaToCoverage = false;
    RhiFormat RenderTargetFormats[RhiMaxRenderTargets] = {};
    uint32 RenderTargetCount = 0;
    RhiFormat DepthStencilFormat = RhiFormat::Unknown;
    uint32 SampleCount = 1;
    uint32 ViewMask = 0;
};
} // namespace Marlin
