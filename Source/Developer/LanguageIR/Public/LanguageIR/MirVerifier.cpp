#include "LanguageIR/MirVerifier.h"

#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirPrinter.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
class MirVerifier
{
public:
    MirVerifier(const MirFunction &function, List<String> &errors)
        : _function(&function), _errors(&errors), _flow(function)
    {
    }

    void Run()
    {
        if (_function->GetBlocks().IsEmpty())
        {
            return;
        }
        for (const MirBlock *block : _function->GetBlocks())
        {
            for (MirBlock *successor : MirControlFlow::GetSuccessors(block))
            {
                List<const MirBlock *> &predecessors = _predecessors.FindOrAdd(successor);
                if (!predecessors.Contains(block))
                {
                    predecessors.Add(block);
                }
            }
        }
        for (const MirBlock *block : _function->GetBlocks())
        {
            uint32 position = 0;
            for (const MirInstruction *instruction : *block)
            {
                _positions.Add(instruction, {block, position++});
            }
        }
        for (const MirBlock *block : _function->GetBlocks())
        {
            VerifyBlock(*block);
        }
    }

private:
    struct Position
    {
        const MirBlock *Block;
        uint32 Index;
    };

    void Fail(const MirInstruction *instruction, StringView message)
    {
        if (instruction != nullptr)
        {
            _errors->Add(Format("@{0}: {1}: {2}", _function->GetName(),
                                instruction->GetOp() == MirOp::Intrinsic
                                    ? GetMirIntrinsicName(instruction->GetIntrinsic())
                                    : GetMirOpName(instruction->GetOp()),
                                message));
        }
        else
        {
            _errors->Add(Format("@{0}: {1}", _function->GetName(), message));
        }
    }

    bool OwnsBlock(const MirBlock *block) const
    {
        return block != nullptr && block->GetParent() == _function;
    }

    void VerifyBlock(const MirBlock &block)
    {
        if (block.GetTerminator() == nullptr)
        {
            Fail(block.GetLast(), Format("block '{0}' does not end in a terminator", block.GetName()));
        }
        if (block.Merge.Kind != MirMergeKind::None)
        {
            if (!OwnsBlock(block.Merge.MergeBlock))
            {
                Fail(nullptr, "a merge block is missing or belongs to another function");
            }
            if (block.Merge.Kind == MirMergeKind::Loop && !OwnsBlock(block.Merge.ContinueBlock))
            {
                Fail(nullptr, "a continue block is missing or belongs to another function");
            }
        }
        bool pastPhis = false;
        for (const MirInstruction *instruction : block)
        {
            if (instruction->IsTerminator() && instruction != block.GetLast())
            {
                Fail(instruction, "a terminator is not the last instruction of its block");
            }
            if (instruction->GetOp() == MirOp::Phi)
            {
                if (pastPhis)
                {
                    Fail(instruction, "a phi follows other instructions");
                }
            }
            else
            {
                pastPhis = true;
            }
            if (instruction->GetOp() == MirOp::Variable &&
                (&block != _function->GetEntry() ||
                 (instruction->GetPrevious() != nullptr && instruction->GetPrevious()->GetOp() != MirOp::Variable)))
            {
                Fail(instruction, "variables must come first in the entry block");
            }
            for (const MirBlock *target : instruction->GetTargets())
            {
                if (!OwnsBlock(target))
                {
                    Fail(instruction, "a target block belongs to another function");
                }
            }
            VerifyOperands(*instruction);
            VerifyTypes(*instruction);
        }
    }

    void VerifyOperands(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        for (usize i = 0; i < operands.Count(); ++i)
        {
            const MirValue *operand = operands[i];
            if (operand->GetValueKind() == MirValueKind::Argument &&
                static_cast<const MirArgument *>(operand)->GetParent() != _function)
            {
                Fail(&instruction, "uses an argument of another function");
            }
            if (operand->GetValueKind() != MirValueKind::Instruction)
            {
                continue;
            }
            const auto *definition = static_cast<const MirInstruction *>(operand);
            const Position *defined = _positions.Find(definition);
            if (defined == nullptr)
            {
                Fail(&instruction, "uses a value that is not in this function");
                continue;
            }
            const MirBlock *useBlock = instruction.GetParent();
            if (instruction.GetOp() == MirOp::Phi)
            {
                if (i < instruction.GetTargets().Count())
                {
                    useBlock = instruction.GetTargets()[i];
                }
                if (_flow.IsReachable(useBlock) && !_flow.Dominates(defined->Block, useBlock))
                {
                    Fail(&instruction, "an incoming value does not dominate its predecessor");
                }
                continue;
            }
            if (!_flow.IsReachable(useBlock))
            {
                continue;
            }
            if (defined->Block == useBlock)
            {
                if (defined->Index >= _positions.Find(&instruction)->Index)
                {
                    Fail(&instruction, "uses a value before it is defined");
                }
            }
            else if (!_flow.Dominates(defined->Block, useBlock))
            {
                Fail(&instruction, "uses a value whose definition does not dominate it");
            }
        }
    }

    void Expect(const MirInstruction &instruction, bool condition, StringView message)
    {
        if (!condition)
        {
            Fail(&instruction, message);
        }
    }

    void VerifyTypes(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const MirType *type = instruction.GetType();
        const Span<const int64> literals = instruction.GetLiterals();
        auto operandType = [&](usize index) { return operands[index]->GetType(); };
        auto isBoolLike = [](const MirType *t) {
            return t->Is(MirTypeKind::Bool) || (t->Is(MirTypeKind::Vector) && t->GetElement()->Is(MirTypeKind::Bool));
        };
        switch (instruction.GetOp())
        {
        case MirOp::Add:
        case MirOp::Sub:
        case MirOp::Mul:
        case MirOp::Div:
        case MirOp::Rem:
        case MirOp::BitAnd:
        case MirOp::BitOr:
        case MirOp::BitXor:
        case MirOp::ShiftLeft:
        case MirOp::ShiftRight:
        case MirOp::LogicalAnd:
        case MirOp::LogicalOr:
            Expect(instruction, operands.Count() == 2, "takes two operands");
            if (operands.Count() == 2)
            {
                Expect(instruction, operandType(0) == type && operandType(1) == type,
                       "operands and result must have the same type");
            }
            break;
        case MirOp::Negate:
        case MirOp::BitNot:
        case MirOp::LogicalNot:
            Expect(instruction, operands.Count() == 1 && operandType(0) == type, "operand and result must match");
            break;
        case MirOp::Equal:
        case MirOp::NotEqual:
        case MirOp::Less:
        case MirOp::LessEqual:
        case MirOp::Greater:
        case MirOp::GreaterEqual:
            Expect(instruction, operands.Count() == 2 && operandType(0) == operandType(1),
                   "compares two values of the same type");
            Expect(instruction, isBoolLike(type), "the result must be bool");
            break;
        case MirOp::Select:
            Expect(instruction, operands.Count() == 3 && isBoolLike(operandType(0)) && operandType(1) == type &&
                                    operandType(2) == type,
                   "takes a bool condition and two values of the result type");
            break;
        case MirOp::Load:
            Expect(instruction,
                   operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Pointer) &&
                       operandType(0)->GetElement() == type,
                   "loads the pointee type of a pointer");
            break;
        case MirOp::Store:
            Expect(instruction,
                   operands.Count() == 2 && operandType(0)->Is(MirTypeKind::Pointer) &&
                       operandType(0)->GetElement() == operandType(1),
                   "stores the pointee type of a pointer");
            break;
        case MirOp::AccessChain:
            Expect(instruction, !operands.IsEmpty() && operandType(0)->Is(MirTypeKind::Pointer) &&
                                    type->Is(MirTypeKind::Pointer),
                   "indexes a pointer");
            break;
        case MirOp::Phi:
            Expect(instruction, operands.Count() == instruction.GetTargets().Count(),
                   "needs one incoming block per value");
            for (const MirValue *operand : operands)
            {
                Expect(instruction, operand->GetType() == type, "incoming values must have the phi's type");
            }
            if (_flow.IsReachable(instruction.GetParent()))
            {
                const List<const MirBlock *> *found = _predecessors.Find(instruction.GetParent());
                const Span<const MirBlock *const> predecessors =
                    found != nullptr ? found->AsSpan() : Span<const MirBlock *const>();
                Expect(instruction, predecessors.Count() == instruction.GetTargets().Count(),
                       "needs one incoming value per predecessor");
                for (const MirBlock *incoming : instruction.GetTargets())
                {
                    bool found = false;
                    for (const MirBlock *predecessor : predecessors)
                    {
                        found = found || predecessor == incoming;
                    }
                    Expect(instruction, found, "an incoming block is not a predecessor");
                }
            }
            break;
        case MirOp::Call:
        {
            if (operands.IsEmpty() || operands[0]->GetValueKind() != MirValueKind::Function)
            {
                Fail(&instruction, "calls a function");
                break;
            }
            const Span<const MirType *const> parameters = operandType(0)->GetParameters();
            Expect(instruction, parameters.Count() + 1 == operands.Count(), "passes one argument per parameter");
            for (usize i = 0; i < parameters.Count() && i + 1 < operands.Count(); ++i)
            {
                Expect(instruction, parameters[i] == operandType(i + 1), "an argument has the wrong type");
            }
            Expect(instruction, operandType(0)->GetElement() == type, "the result is the return type");
            break;
        }
        case MirOp::Branch:
            Expect(instruction, instruction.GetTargets().Count() == 1, "has one target");
            break;
        case MirOp::CondBranch:
            Expect(instruction, operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Bool),
                   "branches on a bool");
            Expect(instruction, instruction.GetTargets().Count() == 2, "has two targets");
            break;
        case MirOp::Switch:
            Expect(instruction, operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Int),
                   "switches on an integer");
            Expect(instruction, instruction.GetTargets().Count() == instruction.GetLiterals().Count() + 1,
                   "needs a default and one target per case");
            break;
        case MirOp::Return:
        {
            const MirType *returnType = _function->GetReturnType();
            if (_function->Kind != MirFunctionKind::Normal)
            {
                returnType = _function->ResultType != nullptr && _function->Kind == MirFunctionKind::Async
                                 ? _function->ResultType
                                 : _function->GetModule().GetTypes().GetVoid();
            }
            if (returnType->Is(MirTypeKind::Void))
            {
                Expect(instruction, operands.IsEmpty(), "returns nothing from a void function");
            }
            else
            {
                Expect(instruction, operands.Count() == 1 && operandType(0) == returnType,
                       "returns a value of the return type");
            }
            break;
        }
        case MirOp::Convert:
        case MirOp::Bitcast:
            Expect(instruction, operands.Count() == 1, "takes one operand");
            break;
        case MirOp::New:
        case MirOp::Box:
            Expect(instruction, type->Is(MirTypeKind::Reference) && !literals.IsEmpty(), "makes a reference");
            Expect(instruction, operands.Count() == (instruction.GetOp() == MirOp::Box ? 1u : 0u), "takes one operand");
            break;
        case MirOp::NewArray:
            Expect(instruction,
                   operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Int) && type->Is(MirTypeKind::Reference),
                   "makes an array of a length");
            break;
        case MirOp::TypeOf:
        case MirOp::StringConstant:
            Expect(instruction, operands.IsEmpty() && literals.Count() == 1 && type->Is(MirTypeKind::Reference),
                   "makes a reference from its literal");
            if (instruction.GetOp() == MirOp::StringConstant && literals.Count() == 1)
            {
                Expect(instruction,
                       literals[0] >= 0 &&
                           static_cast<usize>(literals[0]) < _function->GetModule().GetStrings().Count(),
                       "makes a reference from its literal");
            }
            break;
        case MirOp::FieldAddress:
        {
            const bool shaped = operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Reference) &&
                                type->Is(MirTypeKind::Pointer) &&
                                type->GetAddressSpace() == MirAddressSpace::Managed && literals.Count() == 2;
            Expect(instruction, shaped, "points at a field of a class's layout");
            const Span<MirScriptType *const> scriptTypes = _function->GetModule().GetScriptTypes();
            if (shaped && literals[1] >= 0 && static_cast<usize>(literals[1]) < scriptTypes.Count())
            {
                const MirType *layout = scriptTypes[static_cast<usize>(literals[1])]->Layout;
                Expect(instruction,
                       layout != nullptr && layout->Is(MirTypeKind::Struct) && literals[0] >= 0 &&
                           static_cast<usize>(literals[0]) < layout->GetFields().Count() &&
                           layout->GetFields()[static_cast<usize>(literals[0])].Type == type->GetElement(),
                       "points at a field of a class's layout");
            }
            break;
        }
        case MirOp::ElementAddress:
        {
            const MirType *array = operands.Count() == 2 && operandType(0)->Is(MirTypeKind::Reference)
                                       ? operandType(0)->GetElement()
                                       : nullptr;
            Expect(instruction,
                   array != nullptr && (array->Is(MirTypeKind::RuntimeArray) || array->Is(MirTypeKind::Array)) &&
                       operandType(1)->Is(MirTypeKind::Int) && type->Is(MirTypeKind::Pointer) &&
                       type->GetAddressSpace() == MirAddressSpace::Managed && type->GetElement() == array->GetElement(),
                   "points at an element of an array");
            break;
        }
        case MirOp::ObjectLength:
        case MirOp::Unbox:
        case MirOp::IsInstance:
        case MirOp::Cast:
        case MirOp::NullCheck:
            Expect(instruction, operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Reference),
                   "takes a reference");
            if (instruction.GetOp() == MirOp::Cast || instruction.GetOp() == MirOp::NullCheck)
            {
                Expect(instruction, type->Is(MirTypeKind::Reference), "makes a reference");
            }
            else if (instruction.GetOp() == MirOp::IsInstance)
            {
                Expect(instruction, type->Is(MirTypeKind::Bool), "the result must be bool");
            }
            break;
        case MirOp::CallVirtual:
        case MirOp::CallInterface:
        {
            if (operands.Count() < 2 || operands[0]->GetValueKind() != MirValueKind::Function)
            {
                Fail(&instruction, "names the function it dispatches to and a receiver");
                break;
            }
            Expect(instruction, operandType(1)->Is(MirTypeKind::Reference), "dispatches on a reference");
            const Span<const MirType *const> parameters = operandType(0)->GetParameters();
            Expect(instruction, parameters.Count() + 1 == operands.Count(), "passes one argument per parameter");
            Expect(instruction, operandType(0)->GetElement() == type, "the result is the return type");
            break;
        }
        case MirOp::CallDelegate:
            Expect(instruction, !operands.IsEmpty() && operandType(0)->Is(MirTypeKind::Reference), "takes a delegate");
            break;
        case MirOp::MakeDelegate:
            Expect(instruction, !operands.IsEmpty() && operands[0]->GetValueKind() == MirValueKind::Function,
                   "makes a delegate of a function");
            Expect(instruction, type->Is(MirTypeKind::Reference), "makes a reference");
            break;
        case MirOp::CombineDelegates:
            Expect(instruction, operands.Count() == 2 && operandType(0) == type && operandType(1) == type,
                   "operands and result must have the same type");
            break;
        case MirOp::Await:
            Expect(instruction, _function->Kind == MirFunctionKind::Async, "awaits a task in an async function");
            Expect(instruction, operands.Count() == 1 && operandType(0)->Is(MirTypeKind::Reference),
                   "takes a reference");
            break;
        case MirOp::Yield:
            Expect(instruction, _function->Kind == MirFunctionKind::Iterator, "yields a value in an iterator");
            Expect(instruction, operands.Count() == 1, "takes one operand");
            break;
        case MirOp::Fault:
            Expect(instruction, operands.IsEmpty() && literals.Count() == 2, "names a fault and a message");
            break;
        default:
            break;
        }
    }

    const MirFunction *_function;
    List<String> *_errors;
    MirControlFlow _flow;
    HashMap<const MirInstruction *, Position> _positions;
    HashMap<const MirBlock *, List<const MirBlock *>> _predecessors;
};
} // namespace

bool VerifyMirFunction(const MirFunction &function, List<String> &errors)
{
    const usize before = errors.Count();
    MirVerifier(function, errors).Run();
    return errors.Count() == before;
}

bool VerifyMirModule(const MirModule &module, List<String> &errors)
{
    const usize before = errors.Count();
    for (const MirFunction *function : module.GetFunctions())
    {
        VerifyMirFunction(*function, errors);
    }
    for (const MirEntryPoint *entryPoint : module.GetEntryPoints())
    {
        if (entryPoint->Function == nullptr || !entryPoint->Function->GetReturnType()->Is(MirTypeKind::Void) ||
            !entryPoint->Function->GetArguments().IsEmpty())
        {
            errors.Add(Format("entry point '{0}' must be a void function without arguments", entryPoint->Name));
        }
    }
    return errors.Count() == before;
}
} // namespace Marlin
