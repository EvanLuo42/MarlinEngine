#include "LanguageIR/MirPrinter.h"

#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
constexpr StringView MirOpNames[] = {
    "Add",
    "Sub",
    "Mul",
    "Div",
    "Rem",
    "Negate",
    "BitAnd",
    "BitOr",
    "BitXor",
    "BitNot",
    "ShiftLeft",
    "ShiftRight",
    "LogicalAnd",
    "LogicalOr",
    "LogicalNot",
    "Equal",
    "NotEqual",
    "Less",
    "LessEqual",
    "Greater",
    "GreaterEqual",
    "MatrixTimesVector",
    "VectorTimesMatrix",
    "MatrixTimesMatrix",
    "MatrixTimesScalar",
    "VectorTimesScalar",
    "Convert",
    "Bitcast",
    "CompositeConstruct",
    "CompositeExtract",
    "CompositeInsert",
    "VectorExtractDynamic",
    "VectorInsertDynamic",
    "VectorShuffle",
    "Select",
    "Variable",
    "Load",
    "Store",
    "AccessChain",
    "ArrayLength",
    "BufferPointer",
    "Call",
    "Phi",
    "Intrinsic",
    "Branch",
    "CondBranch",
    "Switch",
    "Return",
    "Discard",
    "Unreachable",
    "IgnoreHit",
    "AcceptHitAndEndSearch",
    "EmitMeshTasks",
    "New",
    "NewArray",
    "FieldAddress",
    "ElementAddress",
    "ObjectLength",
    "Box",
    "Unbox",
    "IsInstance",
    "Cast",
    "TypeOf",
    "NullCheck",
    "CallVirtual",
    "CallInterface",
    "CallDelegate",
    "MakeDelegate",
    "CombineDelegates",
    "StringConstant",
    "Await",
    "Yield",
    "Fault",
};

constexpr StringView MirIntrinsicNames[] = {
    "None",
    "Abs",
    "Sign",
    "Floor",
    "Ceil",
    "Round",
    "Trunc",
    "Fract",
    "Sqrt",
    "InverseSqrt",
    "Exp",
    "Exp2",
    "Log",
    "Log2",
    "Pow",
    "Sin",
    "Cos",
    "Tan",
    "Asin",
    "Acos",
    "Atan",
    "Atan2",
    "Sinh",
    "Cosh",
    "Tanh",
    "Min",
    "Max",
    "Clamp",
    "Saturate",
    "Lerp",
    "Step",
    "SmoothStep",
    "Fma",
    "Ldexp",
    "Dot",
    "Cross",
    "Length",
    "Distance",
    "Normalize",
    "Reflect",
    "Refract",
    "FaceForward",
    "Determinant",
    "Inverse",
    "Transpose",
    "IsNan",
    "IsInf",
    "Any",
    "All",
    "CountBits",
    "FirstBitHigh",
    "FirstBitLow",
    "ReverseBits",
    "PackHalf2x16",
    "UnpackHalf2x16",
    "Ddx",
    "Ddy",
    "DdxFine",
    "DdyFine",
    "DdxCoarse",
    "DdyCoarse",
    "Fwidth",
    "Sample",
    "SampleBias",
    "SampleLevel",
    "SampleGrad",
    "SampleCmp",
    "SampleCmpLevel",
    "Gather",
    "GatherCmp",
    "TextureLoad",
    "TextureStore",
    "TextureDimensions",
    "TextureLevels",
    "TextureSamples",
    "CalculateLod",
    "AtomicLoad",
    "AtomicStore",
    "AtomicExchange",
    "AtomicCompareExchange",
    "AtomicAdd",
    "AtomicMin",
    "AtomicMax",
    "AtomicAnd",
    "AtomicOr",
    "AtomicXor",
    "GroupBarrier",
    "GroupMemoryBarrier",
    "DeviceMemoryBarrier",
    "AllMemoryBarrier",
    "WaveIsFirstLane",
    "WaveActiveAllTrue",
    "WaveActiveAnyTrue",
    "WaveActiveAllEqual",
    "WaveActiveBallot",
    "WaveReadLaneAt",
    "WaveReadFirstLane",
    "WaveActiveSum",
    "WaveActiveProduct",
    "WaveActiveMin",
    "WaveActiveMax",
    "WaveActiveBitAnd",
    "WaveActiveBitOr",
    "WaveActiveBitXor",
    "WaveActiveCountBits",
    "WavePrefixSum",
    "WavePrefixProduct",
    "WavePrefixCountBits",
    "SetMeshOutputCounts",
    "TraceRay",
    "ReportHit",
    "CallShader",
    "RayQueryInitialize",
    "RayQueryProceed",
    "RayQueryAbort",
    "RayQueryConfirmIntersection",
    "RayQueryGenerateIntersection",
    "RayQueryGet",
    "HitObjectTraceRay",
    "HitObjectFromRayQuery",
    "HitObjectMakeMiss",
    "HitObjectMakeNop",
    "HitObjectInvoke",
    "HitObjectGet",
    "ReorderThread",
    "CoopVectorLoad",
    "CoopVectorStore",
    "CoopVectorMatMul",
    "CoopVectorMatMulAdd",
    "CoopVectorOuterProductAccumulate",
    "CoopVectorReduceSumAccumulate",
    "CoopMatrixLoad",
    "CoopMatrixStore",
    "CoopMatrixMulAdd",
    "CoopMatrixLength",
    "DebugPrint",
    "DebugDeclare",
    "ForwardDerivative",
    "BackwardDerivative",
    "Detach",
    "Require",
};

constexpr StringView MirBuiltinNames[] = {
    "None",
    "Position",
    "VertexIndex",
    "InstanceIndex",
    "DispatchThreadId",
    "GroupThreadId",
    "GroupId",
    "GroupIndex",
    "Depth",
    "Coverage",
    "FrontFace",
    "SampleIndex",
    "Barycentrics",
    "PrimitiveId",
    "ViewIndex",
    "ShadingRate",
    "PrimitiveShadingRate",
    "CullPrimitive",
    "RenderTargetIndex",
    "ViewportIndex",
    "PrimitiveTriangleIndices",
    "PrimitiveLineIndices",
    "PrimitivePointIndices",
    "LaunchIndex",
    "LaunchSize",
    "WorldRayOrigin",
    "WorldRayDirection",
    "ObjectRayOrigin",
    "ObjectRayDirection",
    "RayTMin",
    "RayTCurrent",
    "IncomingRayFlags",
    "HitKind",
    "InstanceIndexRt",
    "InstanceId",
    "GeometryIndex",
    "PrimitiveIndexRt",
    "ObjectToWorld",
    "WorldToObject",
    "WaveLaneIndex",
    "WaveLaneCount",
    "HelperLane",
};

constexpr StringView MirStageNames[] = {
    "Compute",
    "Amplification",
    "Mesh",
    "Vertex",
    "Pixel",
    "RayGeneration",
    "ClosestHit",
    "AnyHit",
    "Miss",
    "Intersection",
    "Callable",
};

template <typename TEnum, usize TCount> StringView NameOf(const StringView (&names)[TCount], TEnum value)
{
    const auto index = static_cast<usize>(value);
    return index < TCount ? names[index] : StringView("?");
}

class MirPrinter
{
public:
    explicit MirPrinter(StringWriter &output) : _output(&output)
    {
    }

    void PrintModule(const MirModule &module)
    {
        const Span<MirScriptType *const> scriptTypes = module.GetScriptTypes();
        for (usize i = 0; i < scriptTypes.Count(); ++i)
        {
            PrintScriptType(*scriptTypes[i], static_cast<uint32>(i));
        }
        for (const MirGlobal *global : module.GetGlobals())
        {
            PrintGlobal(*global);
        }
        for (const MirFunction *function : module.GetFunctions())
        {
            _output->Append('\n');
            PrintFunction(*function);
        }
        for (const MirEntryPoint *entryPoint : module.GetEntryPoints())
        {
            PrintEntryPoint(*entryPoint);
        }
    }

    void PrintFunction(const MirFunction &function)
    {
        _numbers.Clear();
        _blocks.Clear();
        _next = 0;
        for (const MirArgument *argument : function.GetArguments())
        {
            _numbers.Add(argument, _next++);
        }
        for (usize i = 0; i < function.GetBlocks().Count(); ++i)
        {
            _blocks.Add(function.GetBlocks()[i], static_cast<uint32>(i));
            for (const MirInstruction *instruction : *function.GetBlocks()[i])
            {
                if (!instruction->GetType()->Is(MirTypeKind::Void))
                {
                    _numbers.Add(instruction, _next++);
                }
            }
        }
        _output->Append("fn @").Append(function.GetName()).Append('(');
        for (usize i = 0; i < function.GetArguments().Count(); ++i)
        {
            if (i != 0)
            {
                _output->Append(", ");
            }
            PrintValue(function.GetArguments()[i]);
            _output->Append(": ");
            function.GetArguments()[i]->GetType()->Print(*_output);
        }
        _output->Append(") -> ");
        function.GetReturnType()->Print(*_output);
        if (function.Kind != MirFunctionKind::Normal)
        {
            _output->Append(function.Kind == MirFunctionKind::Async ? " async" : " iterator");
        }
        if (!function.NativeName.IsEmpty())
        {
            _output->Append(" native(\"").Append(function.NativeName).Append("\")");
        }
        _output->Append(function.Imported ? " imported" : "").Append(function.Test ? " test" : "");
        _output->Append("\n{\n");
        for (const MirBlock *block : function.GetBlocks())
        {
            PrintBlock(*block);
        }
        _output->Append("}\n");
    }

private:
    void PrintScriptType(const MirScriptType &type, uint32 index)
    {
        constexpr StringView kinds[] = {"class", "interface", "delegate", "array", "string", "value"};
        FormatTo(*_output, "type #{0} {1} {2}", index, NameOf(kinds, type.Kind), type.Name);
        // RECOVERY-DECISION lang-ir-D5: base, flags and layout follow the type line, in the printer's attribute style.
        if (type.Base != ~0u)
        {
            FormatTo(*_output, " : #{0}", type.Base);
        }
        _output->Append(type.Abstract ? " abstract" : "").Append(type.Sealed ? " sealed" : "");
        _output->Append(type.Imported ? " imported" : "");
        if (type.Layout != nullptr)
        {
            _output->Append(' ');
            type.Layout->Print(*_output);
        }
        _output->Append('\n');
        for (usize i = 0; i < type.VirtualSlots.Count(); ++i)
        {
            FormatTo(*_output, "    slot {0}: @{1}\n", i, type.VirtualSlots[i]->GetName());
        }
        for (const MirInterfaceImpl &implementation : type.Interfaces)
        {
            FormatTo(*_output, "    implements #{0}:", implementation.Interface);
            for (const MirFunction *method : implementation.Methods)
            {
                _output->Append(" @").Append(method->GetName());
            }
            _output->Append('\n');
        }
    }

    void PrintGlobal(const MirGlobal &global)
    {
        _output->Append("global @").Append(global.GetName()).Append(": ");
        global.GetType()->Print(*_output);
        if (global.Builtin != MirBuiltin::None)
        {
            _output->Append(" builtin(").Append(GetMirBuiltinName(global.Builtin)).Append(')');
        }
        if (global.Location != ~0u)
        {
            FormatTo(*_output, " location({0})", global.Location);
        }
        if (global.Interpolation != MirInterpolation::Perspective)
        {
            _output->Append(global.Interpolation == MirInterpolation::Linear ? " linear" : " nointerpolation");
        }
        if (global.Sampling != MirSampling::Center)
        {
            _output->Append(global.Sampling == MirSampling::Centroid ? " centroid" : " sample");
        }
        if (global.PerPrimitive)
        {
            _output->Append(" perprimitive");
        }
        if (global.Initializer != nullptr)
        {
            _output->Append(" = ");
            PrintConstant(*global.Initializer);
        }
        _output->Append('\n');
    }

    void PrintEntryPoint(const MirEntryPoint &entryPoint)
    {
        FormatTo(*_output, "entry {0} \"{1}\" @{2}", GetMirStageName(entryPoint.Stage), entryPoint.Name,
                 entryPoint.Function->GetName());
        if (entryPoint.Stage == MirStage::Compute || entryPoint.Stage == MirStage::Amplification ||
            entryPoint.Stage == MirStage::Mesh)
        {
            FormatTo(*_output, " group({0}, {1}, {2})", entryPoint.GroupSize[0], entryPoint.GroupSize[1],
                     entryPoint.GroupSize[2]);
        }
        if (entryPoint.Stage == MirStage::Mesh)
        {
            FormatTo(*_output, " vertices({0}) primitives({1}) topology({2})", entryPoint.MaxVertices,
                     entryPoint.MaxPrimitives, static_cast<uint32>(entryPoint.Topology));
        }
        for (const StringView feature : entryPoint.Features)
        {
            // RECOVERY-DECISION lang-ir-D5: features print like the other entry attributes.
            _output->Append(" requires(").Append(feature).Append(')');
        }
        for (const MirGlobal *global : entryPoint.Interface)
        {
            _output->Append(" @").Append(global->GetName());
        }
        _output->Append('\n');
    }

    void PrintBlock(const MirBlock &block)
    {
        FormatTo(*_output, "block{0}:", _blocks.Find(&block) != nullptr ? *_blocks.Find(&block) : ~0u);
        if (!block.GetName().IsEmpty())
        {
            _output->Append(" // ").Append(block.GetName());
        }
        if (block.Merge.Kind != MirMergeKind::None)
        {
            _output->Append(block.Merge.Kind == MirMergeKind::Loop ? " loop merge(" : " selection merge(");
            PrintBlockReference(block.Merge.MergeBlock);
            if (block.Merge.Kind == MirMergeKind::Loop)
            {
                _output->Append(") continue(");
                PrintBlockReference(block.Merge.ContinueBlock);
            }
            _output->Append(')');
            if (block.Merge.Control == MirLoopControl::Unroll)
            {
                _output->Append(" unroll");
                if (block.Merge.UnrollCount != 0)
                {
                    FormatTo(*_output, "({0})", block.Merge.UnrollCount);
                }
            }
            if (block.Merge.MaxIterations != 0)
            {
                FormatTo(*_output, " max_iterations({0})", block.Merge.MaxIterations);
            }
        }
        _output->Append('\n');
        for (const MirInstruction *instruction : block)
        {
            PrintInstruction(*instruction);
        }
    }

    void PrintInstruction(const MirInstruction &instruction)
    {
        _output->Append("    ");
        if (!instruction.GetType()->Is(MirTypeKind::Void))
        {
            PrintValue(&instruction);
            _output->Append(" = ");
        }
        _output->Append(instruction.GetOp() == MirOp::Intrinsic ? GetMirIntrinsicName(instruction.GetIntrinsic())
                                                                 : GetMirOpName(instruction.GetOp()));
        if (instruction.NonUniform)
        {
            _output->Append(" nonuniform");
        }
        if (instruction.Precise)
        {
            _output->Append(" precise");
        }
        if (instruction.Checked)
        {
            _output->Append(" checked");
        }
        if (!instruction.GetType()->Is(MirTypeKind::Void))
        {
            _output->Append(' ');
            instruction.GetType()->Print(*_output);
        }
        const char *separator = " ";
        for (const MirValue *operand : instruction.GetOperands())
        {
            _output->Append(separator);
            separator = ", ";
            PrintValue(operand);
        }
        for (usize i = 0; i < instruction.GetTargets().Count(); ++i)
        {
            _output->Append(separator);
            separator = ", ";
            if (instruction.GetOp() == MirOp::Switch && i != 0)
            {
                FormatTo(*_output, "{0} -> ", instruction.GetLiterals()[i - 1]);
            }
            PrintBlockReference(instruction.GetTargets()[i]);
        }
        if (instruction.GetOp() != MirOp::Switch && !instruction.GetLiterals().IsEmpty())
        {
            _output->Append(separator).Append('[');
            for (usize i = 0; i < instruction.GetLiterals().Count(); ++i)
            {
                FormatTo(*_output, "{0}{1}", StringView(i == 0 ? "" : ", "), instruction.GetLiterals()[i]);
            }
            _output->Append(']');
        }
        if (!instruction.GetName().IsEmpty())
        {
            _output->Append(" // ").Append(instruction.GetName());
        }
        _output->Append('\n');
    }

    void PrintBlockReference(const MirBlock *block)
    {
        const uint32 *number = _blocks.Find(block);
        FormatTo(*_output, "block{0}", number != nullptr ? *number : ~0u);
    }

    void PrintValue(const MirValue *value)
    {
        switch (value->GetValueKind())
        {
        case MirValueKind::Constant:
            value->GetType()->Print(*_output);
            _output->Append(' ');
            PrintConstant(*static_cast<const MirConstant *>(value));
            break;
        case MirValueKind::Global:
        case MirValueKind::Function:
            _output->Append('@').Append(value->GetName());
            break;
        default:
        {
            const uint32 *number = _numbers.Find(value);
            if (number != nullptr)
            {
                FormatTo(*_output, "%{0}", *number);
            }
            else
            {
                _output->Append("%?");
            }
            break;
        }
        }
    }

    void PrintConstant(const MirConstant &constant)
    {
        switch (constant.GetConstantKind())
        {
        case MirConstantKind::Scalar:
            if (constant.GetType()->Is(MirTypeKind::Float))
            {
                FormatTo(*_output, "{0}", constant.GetFloat());
            }
            else if (constant.GetType()->Is(MirTypeKind::Bool))
            {
                _output->Append(constant.GetBits() != 0 ? "true" : "false");
            }
            else if (constant.GetType()->IsSigned())
            {
                FormatTo(*_output, "{0}", constant.GetInt());
            }
            else
            {
                FormatTo(*_output, "{0}", constant.GetBits());
            }
            break;
        case MirConstantKind::Composite:
            _output->Append('{');
            for (usize i = 0; i < constant.GetElements().Count(); ++i)
            {
                _output->Append(i == 0 ? "" : ", ");
                PrintConstant(*constant.GetElements()[i]);
            }
            _output->Append('}');
            break;
        case MirConstantKind::Null:
            _output->Append("null");
            break;
        case MirConstantKind::Undef:
            _output->Append("undef");
            break;
        }
    }

    StringWriter *_output;
    HashMap<const MirValue *, uint32> _numbers;
    HashMap<const MirBlock *, uint32> _blocks;
    uint32 _next = 0;
};
} // namespace

StringView GetMirOpName(MirOp op)
{
    return NameOf(MirOpNames, op);
}

StringView GetMirIntrinsicName(MirIntrinsic intrinsic)
{
    return NameOf(MirIntrinsicNames, intrinsic);
}

StringView GetMirStageName(MirStage stage)
{
    return NameOf(MirStageNames, stage);
}

StringView GetMirBuiltinName(MirBuiltin builtin)
{
    return NameOf(MirBuiltinNames, builtin);
}

void PrintMirModule(const MirModule &module, StringWriter &output)
{
    MirPrinter(output).PrintModule(module);
}

void PrintMirFunction(const MirFunction &function, StringWriter &output)
{
    MirPrinter(output).PrintFunction(function);
}
} // namespace Marlin
