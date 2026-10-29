#include "LanguageIR/MirTarget.h"

#include "Hash/Xxh3.h"
#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPrinter.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
MirOperandClass GetOperandClass(const MirType *type)
{
    if (type->Is(MirTypeKind::Matrix))
    {
        return MirOperandClass::Matrix;
    }
    const MirType *scalar = type->GetScalar();
    if (scalar == nullptr)
    {
        return MirOperandClass::Other;
    }
    switch (scalar->GetKind())
    {
    case MirTypeKind::Bool:
        return MirOperandClass::Bool;
    case MirTypeKind::Int:
        return MirOperandClass::Integer;
    default:
        return MirOperandClass::Float;
    }
}

MirLegality MirTarget::GetLegality(MirOp op, MirIntrinsic intrinsic, MirOperandClass operandClass) const
{
    const MirLegality *found = _legality.Find(Key{OperationOf(op, intrinsic), static_cast<uint8>(operandClass)});
    return found != nullptr ? *found : MirLegality::Legal;
}

MirLegality MirTarget::GetLegality(const MirInstruction &instruction) const
{
    const MirType *type =
        instruction.GetOperands().IsEmpty() ? instruction.GetType() : instruction.GetOperand(0)->GetType();
    return GetLegality(instruction.GetOp(), instruction.GetIntrinsic(), GetOperandClass(type));
}

bool MirTarget::IsTypeLegal(const MirType *type) const
{
    switch (type->GetKind())
    {
    case MirTypeKind::Int:
        return _integerWidths.Contains(type->GetWidth());
    case MirTypeKind::Float:
        return _floatWidths.Contains(type->GetWidth());
    case MirTypeKind::Vector:
    case MirTypeKind::Array:
    case MirTypeKind::RuntimeArray:
    case MirTypeKind::Pointer:
        return type->GetElement() == nullptr || IsTypeLegal(type->GetElement());
    case MirTypeKind::Matrix:
        return (_integerMatrices || type->GetScalar()->Is(MirTypeKind::Float)) && IsTypeLegal(type->GetScalar());
    case MirTypeKind::Struct:
        for (const MirStructField &field : type->GetFields())
        {
            if (!IsTypeLegal(field.Type))
            {
                return false;
            }
        }
        return true;
    default:
        return true;
    }
}

uint32 MirDebugFormatHash(StringView format)
{
    return static_cast<uint32>(Xxh3Hash64(format.Data(), format.Length()));
}

void MirTarget::AddLegalizationPasses(MirModulePassManager &passes, List<String> &errors) const
{
    if (GetLegality(MirOp::Intrinsic, MirIntrinsic::DebugPrint, MirOperandClass::Other) == MirLegality::Expand)
    {
        passes.AddPass(MakeUnique<MirExpandDebugPrintPass>());
    }
    passes.AddPass(MakeUnique<MirLegalizePass>(*this, errors));
}

void MirTarget::SetLegality(MirOp op, MirOperandClass operandClass, MirLegality legality)
{
    _legality.Set(Key{OperationOf(op, MirIntrinsic::None), static_cast<uint8>(operandClass)}, legality);
}

void MirTarget::SetLegality(MirIntrinsic intrinsic, MirOperandClass operandClass, MirLegality legality)
{
    _legality.Set(Key{OperationOf(MirOp::Intrinsic, intrinsic), static_cast<uint8>(operandClass)}, legality);
}

void MirTarget::SetLegality(MirOp op, MirLegality legality)
{
    for (uint8 i = 0; i <= static_cast<uint8>(MirOperandClass::Other); ++i)
    {
        SetLegality(op, static_cast<MirOperandClass>(i), legality);
    }
}

void MirTarget::SetLegality(MirIntrinsic intrinsic, MirLegality legality)
{
    for (uint8 i = 0; i <= static_cast<uint8>(MirOperandClass::Other); ++i)
    {
        SetLegality(intrinsic, static_cast<MirOperandClass>(i), legality);
    }
}

void MirTarget::AddStage(MirStage stage)
{
    _stages |= 1u << static_cast<uint32>(stage);
}

void MirTarget::AddFeature(StringView feature)
{
    _features.Add(String(feature.Data(), feature.Length()));
}

void MirTarget::SetScalarWidths(Span<const uint32> integers, Span<const uint32> floats)
{
    _integerWidths.Clear();
    _integerWidths.AddRange(integers);
    _floatWidths.Clear();
    _floatWidths.AddRange(floats);
}

void MirTargetRegistry::Register(UniquePtr<MirTarget> target)
{
    MARLIN_ASSERT(Find(target->GetName()) == nullptr);
    _targets.Add(Move(target));
}

const MirTarget *MirTargetRegistry::Find(StringView name) const
{
    for (const UniquePtr<MirTarget> &target : _targets)
    {
        if (target->GetName() == name)
        {
            return target.Get();
        }
    }
    return nullptr;
}

namespace
{
String WhereOf(const MirModule &module, const MirInstruction &instruction)
{
    const MirLocation &location = instruction.Location;
    if (location.File == 0 || location.File > module.GetFiles().Count())
    {
        return String(instruction.GetParent()->GetParent()->GetName());
    }
    return Format("{0}:{1}:{2}", module.GetFiles()[location.File - 1], location.Line, location.Column);
}

String TypeText(const MirType *type)
{
    StringBuilder<64> text;
    type->Print(text);
    return text.ToString();
}

/// Applies *op* to each row of the matrix operands and builds the matrix result.
MirValue *ExpandByRows(MirBuilder &builder, MirOp op, const MirType *resultType, Span<MirValue *const> operands,
                       MirIntrinsic intrinsic = MirIntrinsic::None)
{
    const uint32 rows = operands[0]->GetType()->GetRows();
    const MirType *rowType = resultType->GetElement();
    List<MirValue *> results;
    for (uint32 row = 0; row < rows; ++row)
    {
        const uint32 index[] = {row};
        List<MirValue *> parts;
        for (MirValue *operand : operands)
        {
            if (operand->GetType()->Is(MirTypeKind::Matrix))
            {
                parts.Add(builder.Extract(operand, index));
            }
            else
            {
                List<MirValue *> splat;
                splat.Resize(operand->GetType()->Is(MirTypeKind::Vector) ? 1 : rowType->GetCount(), operand);
                parts.Add(operand->GetType()->Is(MirTypeKind::Vector) ? operand : builder.Construct(rowType, splat.AsSpan()));
            }
        }
        if (op == MirOp::Convert)
        {
            results.Add(builder.Convert(parts[0], rowType));
        }
        else if (op == MirOp::Intrinsic)
        {
            results.Add(builder.Intrinsic(intrinsic, rowType, parts.AsSpan()));
        }
        else
        {
            results.Add(builder.Create(op, rowType, parts.AsSpan()));
        }
    }
    return builder.Construct(resultType, results.AsSpan());
}

MirValue *IntegerDot(MirBuilder &builder, MirValue *left, MirValue *right)
{
    MirValue *product = builder.Binary(MirOp::Mul, left, right);
    MirValue *sum = nullptr;
    for (uint32 i = 0; i < left->GetType()->GetCount(); ++i)
    {
        const uint32 index[] = {i};
        MirValue *element = builder.Extract(product, index);
        sum = sum == nullptr ? element : builder.Binary(MirOp::Add, sum, element);
    }
    return sum;
}

bool IsIntegerMatrix(const MirType *type)
{
    return type->Is(MirTypeKind::Matrix) && type->GetScalar()->Is(MirTypeKind::Int);
}

/// Rewrites integer matrices to arrays of their rows for targets without them (§11.4): matrix operations become row
/// operations, then every type, value and constant that holds one is retyped. The layouts agree, so buffers holding
/// them read the same.
class IntegerMatrixRewriter
{
public:
    explicit IntegerMatrixRewriter(MirModule &module) : _module(&module), _types(&module.GetTypes()), _builder(module)
    {
    }

    bool Run(List<String> &errors)
    {
        bool found = false;
        for (MirFunction *function : _module->GetFunctions())
        {
            for (MirBlock *block : function->GetBlocks())
            {
                MirInstruction *instruction = block->GetFirst();
                while (instruction != nullptr)
                {
                    MirInstruction *next = instruction->GetNext();
                    if (UsesIntegerMatrix(*instruction))
                    {
                        found = true;
                        _builder.SetInsertPoint(instruction);
                        _builder.SetLocation(instruction->Location);
                        if (MirValue *replacement = ExpandOperation(*instruction, errors))
                        {
                            instruction->ReplaceAllUsesWith(replacement);
                            instruction->Erase();
                        }
                    }
                    instruction = next;
                }
            }
        }
        for (MirGlobal *global : _module->GetGlobals())
        {
            found = Retype(global) || found;
            if (global->Initializer != nullptr)
            {
                global->Initializer = MapConstant(global->Initializer);
            }
        }
        for (MirFunction *function : _module->GetFunctions())
        {
            found = Retype(function) || found;
            for (MirArgument *argument : function->GetArguments())
            {
                found = Retype(argument) || found;
            }
            for (MirBlock *block : function->GetBlocks())
            {
                for (MirInstruction *instruction : *block)
                {
                    found = Retype(instruction) || found;
                    for (usize i = 0; i < instruction->GetOperands().Count(); ++i)
                    {
                        MirValue *operand = instruction->GetOperand(i);
                        if (operand->GetValueKind() == MirValueKind::Constant)
                        {
                            MirConstant *mapped = MapConstant(static_cast<MirConstant *>(operand));
                            if (mapped != operand)
                            {
                                instruction->SetOperand(i, mapped);
                                found = true;
                            }
                        }
                    }
                }
            }
        }
        return found;
    }

private:
    static bool UsesIntegerMatrix(const MirInstruction &instruction)
    {
        if (IsIntegerMatrix(instruction.GetType()))
        {
            return true;
        }
        for (const MirValue *operand : instruction.GetOperands())
        {
            if (IsIntegerMatrix(operand->GetType()))
            {
                return true;
            }
        }
        return false;
    }

    MirValue *Row(MirValue *matrix, uint32 row)
    {
        const uint32 index[] = {row};
        return _builder.Extract(matrix, index);
    }

    MirValue *Splat(MirValue *scalar, const MirType *vector)
    {
        List<MirValue *> elements;
        elements.Resize(vector->GetCount(), scalar);
        return _builder.Construct(vector, elements.AsSpan());
    }

    /// The row-wise form of a matrix operation; null for the operations that only move whole matrices.
    MirValue *ExpandOperation(MirInstruction &instruction, List<String> &errors)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const MirType *type = instruction.GetType();
        const MirOp op = instruction.GetOp();
        if (op <= MirOp::ShiftRight || op == MirOp::Convert || op == MirOp::Bitcast)
        {
            return IsIntegerMatrix(operands[0]->GetType()) || IsIntegerMatrix(type)
                       ? ExpandByRows(_builder, op, type, operands)
                       : nullptr;
        }
        switch (op)
        {
        case MirOp::MatrixTimesScalar:
        {
            List<MirValue *> rows;
            const MirType *rowType = type->GetElement();
            MirValue *scale = Splat(operands[1], rowType);
            for (uint32 i = 0; i < type->GetRows(); ++i)
            {
                rows.Add(_builder.Binary(MirOp::Mul, Row(operands[0], i), scale));
            }
            return _builder.Construct(type, rows.AsSpan());
        }
        case MirOp::MatrixTimesVector:
        {
            List<MirValue *> elements;
            for (uint32 i = 0; i < operands[0]->GetType()->GetRows(); ++i)
            {
                elements.Add(IntegerDot(_builder, Row(operands[0], i), operands[1]));
            }
            return _builder.Construct(type, elements.AsSpan());
        }
        case MirOp::VectorTimesMatrix:
            return Combine(operands[0], operands[1], type);
        case MirOp::MatrixTimesMatrix:
        {
            List<MirValue *> rows;
            for (uint32 i = 0; i < type->GetRows(); ++i)
            {
                rows.Add(Combine(Row(operands[0], i), operands[1], type->GetElement()));
            }
            return _builder.Construct(type, rows.AsSpan());
        }
        case MirOp::Intrinsic:
            switch (instruction.GetIntrinsic())
            {
            case MirIntrinsic::Transpose:
            {
                const MirType *source = operands[0]->GetType();
                List<MirValue *> rows;
                for (uint32 column = 0; column < source->GetCount(); ++column)
                {
                    List<MirValue *> elements;
                    for (uint32 row = 0; row < source->GetRows(); ++row)
                    {
                        const uint32 index[] = {row, column};
                        elements.Add(_builder.Extract(operands[0], index));
                    }
                    rows.Add(_builder.Construct(type->GetElement(), elements.AsSpan()));
                }
                return _builder.Construct(type, rows.AsSpan());
            }
            case MirIntrinsic::Determinant:
            case MirIntrinsic::Inverse:
                errors.Add(Format("{0}: {1} takes floating-point matrices", WhereOf(*_module, instruction).AsView(),
                                  GetMirIntrinsicName(instruction.GetIntrinsic())));
                return nullptr;
            default:
                return ExpandByRows(_builder, op, type, operands, instruction.GetIntrinsic());
            }
        default:
            return nullptr;
        }
    }

    /// `v * M`: the rows of *matrix* weighted by the components of *vector* and summed.
    MirValue *Combine(MirValue *vector, MirValue *matrix, const MirType *resultType)
    {
        MirValue *sum = nullptr;
        for (uint32 i = 0; i < matrix->GetType()->GetRows(); ++i)
        {
            const uint32 index[] = {i};
            MirValue *weight = Splat(_builder.Extract(vector, index), resultType);
            MirValue *term = _builder.Binary(MirOp::Mul, Row(matrix, i), weight);
            sum = sum == nullptr ? term : _builder.Binary(MirOp::Add, sum, term);
        }
        return sum;
    }

    bool Retype(MirValue *value)
    {
        const MirType *mapped = Map(value->GetType());
        if (mapped == value->GetType())
        {
            return false;
        }
        value->SetType(mapped);
        return true;
    }

    const MirType *Map(const MirType *type)
    {
        if (type == nullptr)
        {
            return nullptr;
        }
        if (const MirType *const *found = _mapped.Find(type))
        {
            return *found;
        }
        const MirType *result = type;
        switch (type->GetKind())
        {
        case MirTypeKind::Matrix:
            if (IsIntegerMatrix(type))
            {
                result = _types->GetArray(type->GetElement(), type->GetRows());
            }
            break;
        case MirTypeKind::Array:
            result = _types->GetArray(Map(type->GetElement()), type->GetCount());
            break;
        case MirTypeKind::RuntimeArray:
            result = _types->GetRuntimeArray(Map(type->GetElement()));
            break;
        case MirTypeKind::Pointer:
            result = _types->GetPointer(Map(type->GetElement()), type->GetAddressSpace());
            break;
        case MirTypeKind::Buffer:
            result = _types->GetBuffer(Map(type->GetElement()), type->IsWritable());
            break;
        case MirTypeKind::Function:
        {
            List<const MirType *> parameters;
            for (const MirType *parameter : type->GetParameters())
            {
                parameters.Add(Map(parameter));
            }
            result = _types->GetFunction(Map(type->GetElement()), parameters.AsSpan());
            break;
        }
        case MirTypeKind::Struct:
        {
            List<MirStructField> fields;
            bool changed = false;
            for (const MirStructField &field : type->GetFields())
            {
                fields.Add({field.Name, Map(field.Type), field.Offset});
                changed = changed || fields.Last().Type != field.Type;
            }
            if (changed)
            {
                result = _types->CreateStruct(type->GetName(), fields.AsSpan(), type->GetSize());
            }
            break;
        }
        default:
            break;
        }
        _mapped.Add(type, result);
        return result;
    }

    MirConstant *MapConstant(MirConstant *constant)
    {
        const MirType *mapped = Map(constant->GetType());
        if (mapped == constant->GetType())
        {
            return constant;
        }
        switch (constant->GetConstantKind())
        {
        case MirConstantKind::Null:
            return _module->GetNull(mapped);
        case MirConstantKind::Undef:
            return _module->GetUndef(mapped);
        case MirConstantKind::Composite:
        {
            List<MirConstant *> elements;
            for (MirConstant *element : constant->GetElements())
            {
                elements.Add(MapConstant(element));
            }
            return _module->GetComposite(mapped, elements.AsSpan());
        }
        default:
            return constant;
        }
    }

    MirModule *_module;
    MirTypeContext *_types;
    MirBuilder _builder;
    HashMap<const MirType *, const MirType *> _mapped;
};

/// Rewrites *instruction* into more basic MIR; null if there is no generic expansion for it.
MirValue *Expand(MirBuilder &builder, MirInstruction &instruction)
{
    MirModule &module = builder.GetModule();
    const Span<MirValue *const> operands = instruction.GetOperands();
    const MirType *type = instruction.GetType();
    switch (instruction.GetOp())
    {
    case MirOp::Add:
    case MirOp::Sub:
    case MirOp::Mul:
    case MirOp::Div:
    case MirOp::Rem:
    case MirOp::Negate:
    case MirOp::Convert:
        if (operands[0]->GetType()->Is(MirTypeKind::Matrix))
        {
            return ExpandByRows(builder, instruction.GetOp(), type, operands);
        }
        return nullptr;
    case MirOp::Intrinsic:
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::Saturate:
        {
            const MirType *scalar = type->GetScalar();
            MirConstant *zero = module.GetFloat(scalar, 0);
            MirConstant *one = module.GetFloat(scalar, 1);
            if (type->Is(MirTypeKind::Vector))
            {
                zero = module.GetSplat(type, zero);
                one = module.GetSplat(type, one);
            }
            MirValue *arguments[] = {operands[0], zero, one};
            return builder.Intrinsic(MirIntrinsic::Clamp, type, arguments);
        }
        case MirIntrinsic::Ldexp:
        {
            MirValue *scale[] = {operands[1]};
            return builder.Binary(MirOp::Mul, operands[0], builder.Intrinsic(MirIntrinsic::Exp2, type, scale));
        }
        case MirIntrinsic::Dot:
        {
            if (!operands[0]->GetType()->GetScalar()->Is(MirTypeKind::Int))
            {
                return nullptr;
            }
            return IntegerDot(builder, operands[0], operands[1]);
        }
        default:
            return nullptr;
        }
    default:
        return nullptr;
    }
}
/// Appends the words of *value*, a scalar, to *words* as the debug output buffer holds them (§9.11).
void AppendDebugWords(MirBuilder &builder, MirValue *value, List<MirValue *> &words)
{
    MirModule &module = builder.GetModule();
    MirTypeContext &types = module.GetTypes();
    const MirType *u32 = types.GetInt(32, false);
    const MirType *type = value->GetType();
    if (type->Is(MirTypeKind::Bool))
    {
        words.Add(builder.Select(value, module.GetInt(u32, 1), module.GetInt(u32, 0)));
        return;
    }
    if (type->GetWidth() == 64)
    {
        const MirType *u64 = types.GetInt(64, false);
        MirValue *bits = type == u64 ? value : builder.Bitcast(value, u64);
        words.Add(builder.Convert(bits, u32));
        words.Add(builder.Convert(builder.Binary(MirOp::ShiftRight, bits, module.GetInt(u64, 32)), u32));
        return;
    }
    if (type->Is(MirTypeKind::Float))
    {
        const MirType *f32 = types.GetFloat(32);
        words.Add(builder.Bitcast(type == f32 ? value : builder.Convert(value, f32), u32));
        return;
    }
    const MirType *wide = types.GetInt(32, type->IsSigned());
    MirValue *widened = type == wide ? value : builder.Convert(value, wide);
    words.Add(widened->GetType() == u32 ? widened : builder.Bitcast(widened, u32));
}

/// Replaces *print* with the code that appends its record to the debug output buffer whose address is the last
/// field of *uniform*; the builder inserts before it.
void AppendDebugRecord(MirBuilder &builder, MirGlobal *uniform, const MirInstruction &print)
{
    MirModule &module = builder.GetModule();
    const MirType *u32 = module.GetTypes().GetInt(32, false);
    const StringView format = module.GetStrings()[static_cast<usize>(print.GetLiterals()[0])];
    List<MirValue *> words;
    words.Add(nullptr);
    words.Add(module.GetInt(u32, MirDebugFormatHash(format)));
    for (MirValue *operand : print.GetOperands())
    {
        const MirType *type = operand->GetType();
        const MirType *scalar = type->GetScalar();
        const uint32 components = type->Is(MirTypeKind::Vector) ? type->GetCount() : 1;
        const MirDebugArgument kind = scalar->Is(MirTypeKind::Bool)    ? MirDebugArgument::Bool
                                      : scalar->Is(MirTypeKind::Float) ? MirDebugArgument::Float
                                      : scalar->IsSigned()             ? MirDebugArgument::Signed
                                                                       : MirDebugArgument::Unsigned;
        const uint32 bits = scalar->Is(MirTypeKind::Bool) ? 32 : scalar->GetWidth();
        words.Add(module.GetInt(u32, static_cast<uint32>(kind) | (bits << 8) | (components << 16)));
        for (uint32 component = 0; component < components; ++component)
        {
            const uint32 index[] = {component};
            AppendDebugWords(builder, type->Is(MirTypeKind::Vector) ? builder.Extract(operand, index) : operand,
                             words);
        }
    }
    const auto count = static_cast<uint32>(words.Count());
    words[0] = module.GetInt(u32, count);

    const Span<const MirStructField> fields = uniform->GetValueType()->GetFields();
    const uint32 last[] = {static_cast<uint32>(fields.Count() - 1)};
    MirInstruction *buffer = builder.BufferPointer(builder.Extract(builder.Load(uniform), last));
    const auto word = [&](MirValue *index) {
        MirValue *indices[] = {index};
        return builder.AccessChain(buffer, indices);
    };
    MirValue *add[] = {word(module.GetInt(u32, MirDebugBufferCount)), module.GetInt(u32, count)};
    MirValue *first = builder.Intrinsic(MirIntrinsic::AtomicAdd, u32, add);
    MirValue *capacity = builder.Load(word(module.GetInt(u32, MirDebugBufferCapacity)));
    MirValue *fits =
        builder.Compare(MirOp::LessEqual, builder.Binary(MirOp::Add, first, module.GetInt(u32, count)), capacity);
    // A record that does not fit is written to the scratch words, which nothing reads, rather than branched around.
    MirValue *start = builder.Binary(MirOp::Add, builder.Select(fits, first, capacity),
                                     module.GetInt(u32, MirDebugBufferHeaderWords));
    for (uint32 i = 0; i < count; ++i)
    {
        builder.Store(word(builder.Binary(MirOp::Add, start, module.GetInt(u32, i))), words[i]);
    }
}
} // namespace

MirPreservedAnalyses MirExpandDebugPrintPass::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    (void)analyses;
    // A shader module has one entry point; its Uniform global carries the buffer's address if it prints.
    MirGlobal *uniform = nullptr;
    for (const MirEntryPoint *entry : module.GetEntryPoints())
    {
        for (MirGlobal *global : entry->Interface)
        {
            if (entry->DebugBuffer && uniform == nullptr && global->GetAddressSpace() == MirAddressSpace::Uniform)
            {
                uniform = global;
            }
        }
    }
    MirBuilder builder(module);
    bool changed = false;
    for (MirFunction *function : module.GetFunctions())
    {
        for (MirBlock *block : function->GetBlocks())
        {
            MirInstruction *instruction = block->GetFirst();
            while (instruction != nullptr)
            {
                MirInstruction *next = instruction->GetNext();
                if (instruction->GetOp() == MirOp::Intrinsic && instruction->GetIntrinsic() == MirIntrinsic::DebugPrint)
                {
                    if (uniform != nullptr)
                    {
                        builder.SetInsertPoint(instruction);
                        builder.SetLocation(instruction->Location);
                        AppendDebugRecord(builder, uniform, *instruction);
                    }
                    instruction->Erase();
                    changed = true;
                }
                instruction = next;
            }
        }
    }
    return changed ? MirPreservedAnalyses() : MirPreservedAnalyses::All();
}

MirPreservedAnalyses MirLegalizePass::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    (void)analyses;
    const StringView targetName = _target->GetName();
    bool changed = false;
    if (!_target->HasIntegerMatrices())
    {
        changed = IntegerMatrixRewriter(module).Run(*_errors);
    }
    for (const MirEntryPoint *entry : module.GetEntryPoints())
    {
        if (!_target->SupportsStage(entry->Stage))
        {
            _errors->Add(Format("the target `{0}` cannot emit {1} entry points (`{2}`)", targetName,
                                GetMirStageName(entry->Stage), entry->Name));
        }
    }
    for (const MirGlobal *global : module.GetGlobals())
    {
        if (!_target->IsTypeLegal(global->GetValueType()))
        {
            _errors->Add(Format("the target `{0}` has no type `{1}` (`{2}`)", targetName,
                                TypeText(global->GetValueType()).AsView(), global->GetName()));
        }
    }
    MirBuilder builder(module);
    List<const MirType *> reported;
    for (MirFunction *function : module.GetFunctions())
    {
        for (MirBlock *block : function->GetBlocks())
        {
            MirInstruction *instruction = block->GetFirst();
            while (instruction != nullptr)
            {
                MirInstruction *next = instruction->GetNext();
                const MirType *type = instruction->GetType();
                if (!_target->IsTypeLegal(type) && !reported.Contains(type))
                {
                    reported.Add(type);
                    _errors->Add(Format("{0}: the target `{1}` has no type `{2}`", WhereOf(module, *instruction).AsView(),
                                        targetName, TypeText(type).AsView()));
                }
                switch (_target->GetLegality(*instruction))
                {
                case MirLegality::Unsupported:
                    _errors->Add(Format("{0}: the target `{1}` cannot express {2}", WhereOf(module, *instruction).AsView(),
                                        targetName,
                                        instruction->GetOp() == MirOp::Intrinsic ? GetMirIntrinsicName(instruction->GetIntrinsic())
                                                                                  : GetMirOpName(instruction->GetOp())));
                    break;
                case MirLegality::Expand:
                {
                    builder.SetInsertPoint(instruction);
                    builder.SetLocation(instruction->Location);
                    if (MirValue *replacement = Expand(builder, *instruction))
                    {
                        instruction->ReplaceAllUsesWith(replacement);
                        instruction->Erase();
                        changed = true;
                    }
                    else
                    {
                        _errors->Add(Format("{0}: no generic expansion of {1} for the target `{2}`",
                                            WhereOf(module, *instruction).AsView(),
                                            instruction->GetOp() == MirOp::Intrinsic
                                                ? GetMirIntrinsicName(instruction->GetIntrinsic())
                                                : GetMirOpName(instruction->GetOp()),
                                            targetName));
                    }
                    break;
                }
                default:
                    break;
                }
                instruction = next;
            }
        }
    }
    if (!changed)
    {
        return MirPreservedAnalyses::All();
    }
    MirPreservedAnalyses preserved;
    preserved.Preserve(MirCfgAnalyses::Key);
    return preserved;
}
} // namespace Marlin
