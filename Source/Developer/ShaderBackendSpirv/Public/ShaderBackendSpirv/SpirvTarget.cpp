#include "ShaderBackendSpirv/SpirvTarget.h"

#include "Memory/MemoryOps.h"
#include "SpirvEmitter.h"

namespace Marlin
{
namespace
{
class SpirvTarget final : public MirTarget
{
public:
    SpirvTarget()
    {
        AddStage(MirStage::Compute);
        AddStage(MirStage::Vertex);
        AddStage(MirStage::Pixel);
        AddStage(MirStage::Amplification);
        AddStage(MirStage::Mesh);
        for (const MirStage stage : {MirStage::RayGeneration, MirStage::ClosestHit, MirStage::AnyHit, MirStage::Miss,
                                     MirStage::Intersection, MirStage::Callable})
        {
            AddStage(stage);
        }

        const uint32 integers[] = {8, 16, 32, 64};
        const uint32 floats[] = {16, 32, 64};
        SetScalarWidths(integers, floats);
        SetIntegerMatrices(false);

        for (const MirOp op : {MirOp::Add, MirOp::Sub, MirOp::Mul, MirOp::Div, MirOp::Rem, MirOp::Negate, MirOp::Convert})
        {
            SetLegality(op, MirOperandClass::Matrix, MirLegality::Expand);
        }
        SetLegality(MirIntrinsic::Saturate, MirLegality::Expand);
        SetLegality(MirIntrinsic::Ldexp, MirLegality::Expand);
        SetLegality(MirIntrinsic::Dot, MirOperandClass::Integer, MirLegality::Expand);
        // NonSemantic.DebugPrintf reaches the CPU only through validation layers and capture tools; the debug output
        // buffer always does (§9.11).
        SetLegality(MirIntrinsic::DebugPrint, MirLegality::Expand);

        for (const MirIntrinsic intrinsic : {MirIntrinsic::Sample, MirIntrinsic::SampleBias, MirIntrinsic::SampleLevel,
                                             MirIntrinsic::SampleGrad, MirIntrinsic::TextureLoad, MirIntrinsic::TextureStore})
        {
            SetLegality(intrinsic, MirLegality::Custom);
        }

        for (const StringView feature :
             {StringView("ShaderExecutionReorder"), StringView("CooperativeVector"),
              StringView("CooperativeVectorTraining"), StringView("CooperativeMatrix")})
        {
            AddFeature(feature);
        }
    }

    [[nodiscard]] StringView GetName() const override
    {
        return SpirvTargetName;
    }

    bool Emit(MirModule &module, const MirTargetOptions &options, List<uint8> &output,
              List<String> &errors) const override
    {
        List<uint32> words;
        if (!EmitSpirvModule(module, options, words, errors))
        {
            return false;
        }
        const usize offset = output.Count();
        output.ResizeUninitialized(offset + words.Count() * sizeof(uint32));
        MemCopy(output.Data() + offset, words.Data(), words.Count() * sizeof(uint32));
        return true;
    }
};
} // namespace

void RegisterSpirvTarget(MirTargetRegistry &registry)
{
    registry.Register(MakeUnique<SpirvTarget>());
}
} // namespace Marlin
