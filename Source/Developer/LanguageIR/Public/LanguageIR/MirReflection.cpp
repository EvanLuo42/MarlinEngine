#include "LanguageIR/MirReflection.h"

#include "Hash/RapidHash.h"
#include "LanguageIR/MirPrinter.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// Writes what the layout of *type* consists of, so equal layouts give equal text whatever the struct names.
void AppendLayout(StringWriter &output, const MirType *type)
{
    if (!type->Is(MirTypeKind::Struct))
    {
        type->Print(output);
        return;
    }
    FormatTo(output, "{{size {0} align {1}", type->GetSize(), type->GetAlignment());
    for (const MirStructField &field : type->GetFields())
    {
        FormatTo(output, "; {0}: ", field.Name);
        if (field.Type->Is(MirTypeKind::Array))
        {
            FormatTo(output, "{0}x", field.Type->GetCount());
            AppendLayout(output, field.Type->GetElement());
        }
        else
        {
            AppendLayout(output, field.Type);
        }
        FormatTo(output, " @{0}", field.Offset);
    }
    output.Append('}');
}

MirReflectedStageType StageType(const MirType *type)
{
    StringBuilder<256> layout;
    AppendLayout(layout, type);
    MirReflectedStageType reflected;
    reflected.Name = type->Is(MirTypeKind::Struct) ? type->GetName() : StringView();
    reflected.LayoutHash = RapidHash(layout.AsView().Data(), layout.AsView().Length());
    reflected.Size = type->GetSize();
    return reflected;
}

/// The members of the block of *type*: each field of a struct, or the whole value.
void Fields(const MirType *type, List<MirReflectedField> &fields)
{
    if (!type->Is(MirTypeKind::Struct))
    {
        fields.Add({StringView(), type, 0, type->GetSize()});
        return;
    }
    for (const MirStructField &field : type->GetFields())
    {
        fields.Add({field.Name, field.Type, field.Offset, field.Type->GetSize()});
    }
}

/// Adds each cooperative matrix shape *type* is or holds that *matrices* lacks.
void CollectCoopMatrices(const MirType *type, List<MirReflectedCoopMatrix> &matrices)
{
    if (type == nullptr)
    {
        return;
    }
    if (type->Is(MirTypeKind::CoopMatrix))
    {
        const MirReflectedCoopMatrix matrix{type->GetElement(), type->GetRows(), type->GetCount(),
                                            type->GetMatrixUse()};
        for (const MirReflectedCoopMatrix &existing : matrices)
        {
            if (existing.Scalar == matrix.Scalar && existing.Rows == matrix.Rows &&
                existing.Columns == matrix.Columns && existing.Use == matrix.Use)
            {
                return;
            }
        }
        matrices.Add(matrix);
        return;
    }
    if (type->Is(MirTypeKind::Pointer) || type->Is(MirTypeKind::Array) || type->Is(MirTypeKind::RuntimeArray))
    {
        CollectCoopMatrices(type->GetElement(), matrices);
    }
    else if (type->Is(MirTypeKind::Struct))
    {
        for (const MirStructField &field : type->GetFields())
        {
            CollectCoopMatrices(field.Type, matrices);
        }
    }
}
} // namespace

void ReflectMirEntryPoint(const MirModule &module, const MirEntryPoint &entry, MirReflection &reflection)
{
    reflection.Name = entry.Name;
    reflection.Stage = entry.Stage;
    for (uint32 i = 0; i < 3; ++i)
    {
        reflection.GroupSize[i] = entry.GroupSize[i];
    }
    reflection.MaxVertices = entry.MaxVertices;
    reflection.MaxPrimitives = entry.MaxPrimitives;
    reflection.Topology = entry.Topology;
    reflection.Features.AddRange(entry.Features.AsSpan());
    for (const MirGlobal *global : entry.Interface)
    {
        const MirType *type = global->GetValueType();
        switch (global->GetAddressSpace())
        {
        case MirAddressSpace::Uniform:
        {
            // With debug output the global is the arguments, if any, then the debug buffer's address (§9.11).
            const MirType *arguments = type;
            if (entry.DebugBuffer)
            {
                const Span<const MirStructField> fields = type->GetFields();
                reflection.DebugBufferOffset = fields[fields.Count() - 1].Offset;
                arguments = fields.Count() > 1 ? fields[0].Type : nullptr;
            }
            if (arguments == nullptr)
            {
                break;
            }
            // An indirect block is reflected as the struct its Ptr points at, which is what C++ fills (§9.3).
            const MirType *block = entry.ArgumentsIndirect ? arguments->GetElement() : arguments;
            reflection.ArgumentsIndirect = entry.ArgumentsIndirect;
            Fields(block, reflection.Arguments);
            reflection.ArgumentsSize += block->GetSize();
            break;
        }
        case MirAddressSpace::ShaderRecord:
            Fields(type, reflection.ShaderRecord);
            break;
        case MirAddressSpace::Input:
        case MirAddressSpace::Output:
            (global->GetAddressSpace() == MirAddressSpace::Input ? reflection.Inputs : reflection.Outputs)
                .Add({global->GetName(), type, global->Builtin, global->Location, global->PerPrimitive});
            break;
        case MirAddressSpace::GroupShared:
            reflection.GroupSharedBytes += type->GetSize();
            break;
        case MirAddressSpace::TaskPayload:
            reflection.TaskPayloadBytes += type->GetSize();
            break;
        case MirAddressSpace::Specialization:
            reflection.Specializations.Add({global->GetName(), global->Location, type,
                                            global->Initializer != nullptr ? global->Initializer->GetBits() : 0});
            break;
        case MirAddressSpace::RayPayload:
        case MirAddressSpace::IncomingRayPayload:
            reflection.RayPayload = StageType(type);
            break;
        case MirAddressSpace::HitAttribute:
            reflection.HitAttributes = StageType(type);
            break;
        case MirAddressSpace::CallableData:
        case MirAddressSpace::IncomingCallableData:
            reflection.CallableData = StageType(type);
            break;
        default:
            break;
        }
        CollectCoopMatrices(type, reflection.CoopMatrices);
    }
    reflection.DebugFormats.AddRange(module.GetStrings());

    List<const MirFunction *> functions;
    if (entry.Function != nullptr)
    {
        functions.Add(entry.Function);
    }
    for (usize f = 0; f < functions.Count(); ++f)
    {
        for (const MirBlock *block : functions[f]->GetBlocks())
        {
            for (const MirInstruction *instruction : *block)
            {
                CollectCoopMatrices(instruction->GetType(), reflection.CoopMatrices);
                if (instruction->GetOp() == MirOp::Intrinsic &&
                    (instruction->GetIntrinsic() == MirIntrinsic::TraceRay ||
                     instruction->GetIntrinsic() == MirIntrinsic::HitObjectTraceRay))
                {
                    reflection.TracesRays = true;
                }
                if (instruction->GetOp() == MirOp::Call)
                {
                    const auto *callee = static_cast<const MirFunction *>(instruction->GetOperand(0));
                    if (!functions.Contains(callee))
                    {
                        functions.Add(callee);
                    }
                }
            }
        }
    }
}

String PrintMirReflection(const MirReflection &reflection)
{
    StringBuilder<1024> output;
    FormatTo(output, "entry {0} {1} group({2}, {3}, {4})\n", GetMirStageName(reflection.Stage), reflection.Name,
             reflection.GroupSize[0], reflection.GroupSize[1], reflection.GroupSize[2]);
    if (reflection.Stage == MirStage::Mesh)
    {
        FormatTo(output, "mesh vertices {0} primitives {1}\n", reflection.MaxVertices, reflection.MaxPrimitives);
    }
    for (const StringView feature : reflection.Features)
    {
        FormatTo(output, "requires {0}\n", feature);
    }
    auto printType = [&](const MirType *type) {
        StringBuilder<128> text;
        type->Print(text);
        return text.ToString();
    };
    for (const MirReflectedField &field : reflection.Arguments)
    {
        FormatTo(output, "uniform {0}: {1} @{2} +{3}\n", field.Name, printType(field.Type).AsView(), field.Offset,
                 field.Size);
    }
    for (const MirReflectedField &field : reflection.ShaderRecord)
    {
        FormatTo(output, "record {0}: {1} @{2} +{3}\n", field.Name, printType(field.Type).AsView(), field.Offset,
                 field.Size);
    }
    auto printVaryings = [&](StringView label, const List<MirReflectedVarying> &varyings) {
        for (const MirReflectedVarying &varying : varyings)
        {
            // RECOVERY-DECISION lang-ir-D4: `input|output name: type` like the uniform lines, then the recovered suffixes.
            FormatTo(output, "{0} {1}: {2}", label, varying.Name, printType(varying.Type).AsView());
            if (varying.Builtin != MirBuiltin::None)
            {
                output.Append(" builtin(").Append(GetMirBuiltinName(varying.Builtin)).Append(')');
            }
            if (varying.Location != ~0u)
            {
                FormatTo(output, " location({0})", varying.Location);
            }
            output.Append(varying.PerPrimitive ? " per-primitive\n" : "\n");
        }
    };
    printVaryings("input", reflection.Inputs);
    printVaryings("output", reflection.Outputs);
    if (reflection.GroupSharedBytes != 0)
    {
        FormatTo(output, "groupshared {0}\n", reflection.GroupSharedBytes);
    }
    if (reflection.TaskPayloadBytes != 0)
    {
        // RECOVERY-DECISION lang-ir-D4: printed like the groupshared line.
        FormatTo(output, "taskpayload {0}\n", reflection.TaskPayloadBytes);
    }
    for (const MirReflectedSpecialization &specialization : reflection.Specializations)
    {
        FormatTo(output, "specialization {0} id({1}): {2} = {3}\n", specialization.Name, specialization.Id,
                 printType(specialization.Type).AsView(), specialization.DefaultBits);
    }
    for (usize i = 0; i < reflection.DebugFormats.Count(); ++i)
    {
        FormatTo(output, "format {0}: \"{1}\"\n", i, reflection.DebugFormats[i]);
    }
    if (reflection.DebugBufferOffset != ~0u)
    {
        FormatTo(output, "debug buffer at {0}\n", reflection.DebugBufferOffset);
    }
    auto printStageType = [&](StringView label, const MirReflectedStageType &type) {
        if (type.Size != 0 || type.LayoutHash != 0)
        {
            FormatTo(output, "{0} {1} size {2} layout {3}\n", label, type.Name, type.Size, type.LayoutHash);
        }
    };
    printStageType("payload", reflection.RayPayload);
    printStageType("attributes", reflection.HitAttributes);
    printStageType("callable", reflection.CallableData);
    if (reflection.TracesRays)
    {
        output.Append("traces rays\n");
    }
    for (const MirReflectedCoopMatrix &matrix : reflection.CoopMatrices)
    {
        constexpr StringView uses[] = {"a", "b", "acc"};
        FormatTo(output, "coopmatrix {0} {1}x{2} use {3}\n", printType(matrix.Scalar).AsView(), matrix.Rows,
                 matrix.Columns, uses[static_cast<usize>(matrix.Use)]);
    }
    return output.ToString();
}
} // namespace Marlin
