#include "ScriptBackend/ScriptTarget.h"

#include "../../Private/ScriptModuleBuilder.h"

namespace Marlin
{
namespace
{
class ScriptTarget final : public MirTarget
{
public:
    ScriptTarget()
    {
        const uint32 integers[] = {8, 16, 32, 64};
        const uint32 floats[] = {16, 32, 64};
        SetScalarWidths(integers, floats);
        SetIntegerMatrices(true);
        SetLegality(MirIntrinsic::Saturate, MirLegality::Expand);
        SetLegality(MirIntrinsic::Ldexp, MirLegality::Expand);
        for (const MirOp op : {MirOp::Discard, MirOp::IgnoreHit, MirOp::AcceptHitAndEndSearch, MirOp::EmitMeshTasks,
                               MirOp::BufferPointer, MirOp::ArrayLength})
        {
            SetLegality(op, MirLegality::Unsupported);
        }
        for (uint32 i = static_cast<uint32>(MirIntrinsic::Ddx); i <= static_cast<uint32>(MirIntrinsic::DebugPrint); ++i)
        {
            SetLegality(static_cast<MirIntrinsic>(i), MirLegality::Unsupported);
        }
    }

    [[nodiscard]] StringView GetName() const override
    {
        return ScriptTargetName;
    }

    bool Emit(MirModule &module, const MirTargetOptions &options, List<uint8> &output,
              List<String> &errors) const override
    {
        ScriptModuleData data;
        if (!CompileScriptModule(module, options, data, errors))
        {
            return false;
        }
        WriteScriptModule(data, output);
        return true;
    }
};
} // namespace

void RegisterScriptTarget(MirTargetRegistry &registry)
{
    registry.Register(MakeUnique<ScriptTarget>());
}

bool CompileScriptModule(MirModule &module, const MirTargetOptions &options, ScriptModuleData &result,
                         List<String> &errors)
{
    ScriptModuleBuilder builder(module, options, result, errors);
    return builder.Build();
}
} // namespace Marlin
