#include "LanguageIR/MirPasses.h"

#include "Containers/HashSet.h"
#include "LanguageIR/MirBuilder.h"
#include "String/Format.h"

namespace Marlin
{
MirAnalysisKey MirControlFlowAnalysis::Key{"controlflow"};
MirAnalysisKey MirUniformityAnalysis::Key{"uniformity"};

MirControlFlowAnalysis::Result MirControlFlowAnalysis::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    return MirControlFlow(function);
}

bool HasSideEffects(const MirInstruction &instruction)
{
    if (instruction.IsTerminator() || instruction.Checked)
    {
        return true;
    }
    switch (instruction.GetOp())
    {
    case MirOp::Store:
    case MirOp::Call:
    case MirOp::CallVirtual:
    case MirOp::CallInterface:
    case MirOp::CallDelegate:
    case MirOp::NullCheck:
    case MirOp::Unbox:
    case MirOp::Await:
    case MirOp::Yield:
        return true;
    case MirOp::Cast:
        return instruction.GetLiterals()[1] == 0;
    case MirOp::Intrinsic:
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::TextureStore:
        case MirIntrinsic::DebugPrint:
        case MirIntrinsic::DebugDeclare:
        case MirIntrinsic::Require:
        case MirIntrinsic::BackwardDerivative:
        case MirIntrinsic::AtomicLoad:
        case MirIntrinsic::AtomicStore:
        case MirIntrinsic::AtomicExchange:
        case MirIntrinsic::AtomicCompareExchange:
        case MirIntrinsic::AtomicAdd:
        case MirIntrinsic::AtomicMin:
        case MirIntrinsic::AtomicMax:
        case MirIntrinsic::AtomicAnd:
        case MirIntrinsic::AtomicOr:
        case MirIntrinsic::AtomicXor:
        case MirIntrinsic::GroupBarrier:
        case MirIntrinsic::GroupMemoryBarrier:
        case MirIntrinsic::DeviceMemoryBarrier:
        case MirIntrinsic::AllMemoryBarrier:
        case MirIntrinsic::SetMeshOutputCounts:
        case MirIntrinsic::TraceRay:
        case MirIntrinsic::ReportHit:
        case MirIntrinsic::CallShader:
        case MirIntrinsic::RayQueryInitialize:
        case MirIntrinsic::RayQueryProceed:
        case MirIntrinsic::RayQueryAbort:
        case MirIntrinsic::RayQueryConfirmIntersection:
        case MirIntrinsic::RayQueryGenerateIntersection:
        case MirIntrinsic::HitObjectTraceRay:
        case MirIntrinsic::HitObjectFromRayQuery:
        case MirIntrinsic::HitObjectMakeMiss:
        case MirIntrinsic::HitObjectMakeNop:
        case MirIntrinsic::HitObjectInvoke:
        case MirIntrinsic::ReorderThread:
        case MirIntrinsic::CoopVectorStore:
        case MirIntrinsic::CoopVectorOuterProductAccumulate:
        case MirIntrinsic::CoopVectorReduceSumAccumulate:
        case MirIntrinsic::CoopMatrixStore:
            return true;
        default:
            return false;
        }
    default:
        return false;
    }
}

MirPassRegistry MirPassRegistry::CreateDefault()
{
    MirPassRegistry registry;
    registry.RegisterFunctionPass("mergereturn", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirMergeReturnPass>()); });
    registry.RegisterModulePass("inline", [] { return UniquePtr<IMirModulePass>(MakeUnique<MirInlinePass>()); });
    registry.RegisterFunctionPass("sroa", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirSroaPass>()); });
    registry.RegisterFunctionPass("sroa-keepvectors",
                                  [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirSroaPass>(true)); });
    registry.RegisterFunctionPass("mem2reg", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirMem2RegPass>()); });
    registry.RegisterFunctionPass("constfold", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirConstantFoldPass>()); });
    registry.RegisterFunctionPass("deadbranch", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirDeadBranchPass>()); });
    registry.RegisterFunctionPass("cse", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirCsePass>()); });
    registry.RegisterFunctionPass("dce", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirDcePass>()); });
    registry.RegisterFunctionPass("simplifycfg", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirSimplifyCfgPass>()); });
    registry.RegisterFunctionPass("unroll", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirLoopUnrollPass>()); });
    registry.RegisterFunctionPass("nonuniform", [] { return UniquePtr<IMirFunctionPass>(MakeUnique<MirNonUniformPass>()); });
    registry.RegisterModulePass("autodiff", [] { return UniquePtr<IMirModulePass>(MakeUnique<MirAutodiffPass>()); });
    registry.RegisterModulePass("require", [] { return UniquePtr<IMirModulePass>(MakeUnique<MirRequirePass>()); });
    return registry;
}

namespace
{
/// Whether every use of *variable* is the pointer of a Load or Store, so it can live in SSA values.
bool IsPromotable(const MirInstruction *variable)
{
    const MirType *type = variable->GetType()->GetElement();
    if (type->Is(MirTypeKind::RayQuery) || type->Is(MirTypeKind::HitObject))
    {
        return false;
    }
    for (const MirUse &use : variable->GetUses())
    {
        const MirOp op = use.User->GetOp();
        if (!(op == MirOp::Load || (op == MirOp::Store && use.Index == 0)))
        {
            return false;
        }
    }
    return true;
}

uint32 ElementCount(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Struct:
        return static_cast<uint32>(type->GetFields().Count());
    case MirTypeKind::Array:
    case MirTypeKind::Vector:
        return type->GetCount();
    case MirTypeKind::Matrix:
        return type->GetRows();
    default:
        return 0;
    }
}

const MirType *ElementType(const MirType *type, uint32 index)
{
    return type->Is(MirTypeKind::Struct) ? type->GetFields()[index].Type : type->GetElement();
}
} // namespace

MirPreservedAnalyses MirSroaPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    (void)analyses;
    MirModule &module = function.GetModule();
    MirBuilder builder(module);
    bool changed = false;
    bool progress = true;
    while (progress)
    {
        progress = false;
        MirBlock *entry = function.GetEntry();
        List<MirInstruction *> variables;
        for (MirInstruction *instruction : *entry)
        {
            if (instruction->GetOp() == MirOp::Variable)
            {
                variables.Add(instruction);
            }
        }
        for (MirInstruction *variable : variables)
        {
            const MirType *type = variable->GetType()->GetElement();
            const uint32 count = ElementCount(type);
            if (count == 0 || count > 64)
            {
                continue;
            }
            bool splittable = true;
            bool onlyWhole = true;
            for (const MirUse &use : variable->GetUses())
            {
                const MirInstruction *user = use.User;
                const bool constantChain = user->GetOp() == MirOp::AccessChain && use.Index == 0 &&
                                           user->GetOperand(1)->GetValueKind() == MirValueKind::Constant;
                const bool whole = user->GetOp() == MirOp::Load || (user->GetOp() == MirOp::Store && use.Index == 0);
                splittable = splittable && (constantChain || whole);
                onlyWhole = onlyWhole && whole;
            }
            if (_keepWholeVectors && onlyWhole && type->Is(MirTypeKind::Vector))
            {
                continue;
            }
            if (!splittable || !variable->HasUses())
            {
                continue;
            }
            builder.SetInsertPoint(variable);
            List<MirInstruction *> parts;
            for (uint32 i = 0; i < count; ++i)
            {
                parts.Add(builder.Variable(ElementType(type, i), variable->GetName()));
            }
            List<MirInstruction *> users;
            for (const MirUse &use : variable->GetUses())
            {
                if (!users.Contains(use.User))
                {
                    users.Add(use.User);
                }
            }
            for (MirInstruction *user : users)
            {
                builder.SetInsertPoint(user);
                if (user->GetOp() == MirOp::AccessChain)
                {
                    const auto index = static_cast<uint32>(static_cast<MirConstant *>(user->GetOperand(1))->GetBits());
                    MirValue *replacement = parts[index];
                    if (user->GetOperands().Count() > 2)
                    {
                        List<MirValue *> rest;
                        rest.AddRange(user->GetOperands().Slice(2));
                        replacement = builder.AccessChain(parts[index], rest.AsSpan());
                    }
                    user->ReplaceAllUsesWith(replacement);
                    user->Erase();
                }
                else if (user->GetOp() == MirOp::Load)
                {
                    List<MirValue *> elements;
                    for (uint32 i = 0; i < count; ++i)
                    {
                        elements.Add(builder.Load(parts[i]));
                    }
                    user->ReplaceAllUsesWith(builder.Construct(type, elements.AsSpan()));
                    user->Erase();
                }
                else
                {
                    MirValue *value = user->GetOperand(1);
                    for (uint32 i = 0; i < count; ++i)
                    {
                        const uint32 index[] = {i};
                        builder.Store(parts[i], builder.Extract(value, index));
                    }
                    user->Erase();
                }
            }
            variable->Erase();
            progress = true;
            changed = true;
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

MirPreservedAnalyses MirMem2RegPass::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    MirBlock *entry = function.GetEntry();
    if (entry == nullptr)
    {
        return MirPreservedAnalyses::All();
    }
    List<MirInstruction *> variables;
    for (MirInstruction *instruction : *entry)
    {
        if (instruction->GetOp() == MirOp::Variable && IsPromotable(instruction))
        {
            variables.Add(instruction);
        }
    }
    if (variables.IsEmpty())
    {
        return MirPreservedAnalyses::All();
    }
    MirModule &module = function.GetModule();
    MirBuilder builder(module);
    const MirControlFlow &flow = analyses.GetResult<MirControlFlowAnalysis>(function);

    HashMap<const MirInstruction *, usize> index;
    for (usize i = 0; i < variables.Count(); ++i)
    {
        index.Add(variables[i], i);
    }
    HashMap<const MirInstruction *, usize> phiVariable;
    for (usize v = 0; v < variables.Count(); ++v)
    {
        List<MirBlock *> work;
        HashSet<const MirBlock *> hasPhi;
        HashSet<const MirBlock *> queued;
        for (const MirUse &use : variables[v]->GetUses())
        {
            MirBlock *block = use.User->GetParent();
            if (use.User->GetOp() == MirOp::Store && flow.IsReachable(block) && queued.Add(block))
            {
                work.Add(block);
            }
        }
        while (!work.IsEmpty())
        {
            MirBlock *block = work.Pop();
            for (MirBlock *frontier : flow.GetDominanceFrontier(block))
            {
                if (!hasPhi.Add(frontier))
                {
                    continue;
                }
                builder.SetInsertPoint(frontier);
                MirInstruction *phi = builder.Phi(variables[v]->GetType()->GetElement());
                phiVariable.Add(phi, v);
                if (queued.Add(frontier))
                {
                    work.Add(frontier);
                }
            }
        }
    }

    struct Frame
    {
        MirBlock *Block;
        List<MirValue *> Values;
        usize NextChild;
        bool Processed;
    };
    List<MirValue *> initial;
    for (MirInstruction *variable : variables)
    {
        initial.Add(module.GetUndef(variable->GetType()->GetElement()));
    }
    List<Frame> stack;
    stack.Add({entry, Move(initial), 0, false});
    while (!stack.IsEmpty())
    {
        Frame &frame = stack.Last();
        if (!frame.Processed)
        {
            frame.Processed = true;
            MirInstruction *instruction = frame.Block->GetFirst();
            while (instruction != nullptr)
            {
                MirInstruction *next = instruction->GetNext();
                if (instruction->GetOp() == MirOp::Phi)
                {
                    if (const usize *v = phiVariable.Find(instruction))
                    {
                        frame.Values[*v] = instruction;
                    }
                }
                else if (instruction->GetOp() == MirOp::Load || instruction->GetOp() == MirOp::Store)
                {
                    MirValue *pointer = instruction->GetOperand(0);
                    const usize *v = pointer->GetValueKind() == MirValueKind::Instruction
                                         ? index.Find(static_cast<MirInstruction *>(pointer))
                                         : nullptr;
                    if (v != nullptr)
                    {
                        if (instruction->GetOp() == MirOp::Load)
                        {
                            instruction->ReplaceAllUsesWith(frame.Values[*v]);
                        }
                        else
                        {
                            frame.Values[*v] = instruction->GetOperand(1);
                        }
                        instruction->Erase();
                    }
                }
                instruction = next;
            }
            for (MirBlock *successor : MirControlFlow::GetSuccessors(frame.Block))
            {
                for (MirInstruction *phi : *successor)
                {
                    if (phi->GetOp() != MirOp::Phi)
                    {
                        break;
                    }
                    if (const usize *v = phiVariable.Find(phi))
                    {
                        bool already = false;
                        for (const MirBlock *incoming : phi->GetTargets())
                        {
                            already = already || incoming == frame.Block;
                        }
                        if (!already)
                        {
                            phi->AddOperand(frame.Values[*v]);
                            phi->AddTarget(frame.Block);
                        }
                    }
                }
            }
        }
        const Span<MirBlock *const> children = flow.GetDominatedChildren(frame.Block);
        if (frame.NextChild < children.Count())
        {
            MirBlock *child = children[frame.NextChild++];
            List<MirValue *> values;
            values.AddRange(frame.Values.AsSpan());
            stack.Add({child, Move(values), 0, false});
            continue;
        }
        stack.Pop();
    }

    for (MirBlock *block : function.GetBlocks())
    {
        for (MirBlock *successor : MirControlFlow::GetSuccessors(block))
        {
            for (MirInstruction *phi : *successor)
            {
                if (phi->GetOp() != MirOp::Phi)
                {
                    break;
                }
                bool present = false;
                for (const MirBlock *incoming : phi->GetTargets())
                {
                    present = present || incoming == block;
                }
                if (!present && phiVariable.Contains(phi))
                {
                    phi->AddOperand(module.GetUndef(phi->GetType()));
                    phi->AddTarget(block);
                }
            }
        }
    }
    for (MirBlock *block : function.GetBlocks())
    {
        if (flow.IsReachable(block))
        {
            continue;
        }
        MirInstruction *instruction = block->GetFirst();
        while (instruction != nullptr)
        {
            MirInstruction *next = instruction->GetNext();
            if (instruction->GetOp() == MirOp::Load || instruction->GetOp() == MirOp::Store)
            {
                MirValue *pointer = instruction->GetOperand(0);
                const usize *v = pointer->GetValueKind() == MirValueKind::Instruction
                                     ? index.Find(static_cast<MirInstruction *>(pointer))
                                     : nullptr;
                if (v != nullptr)
                {
                    if (instruction->GetOp() == MirOp::Load)
                    {
                        instruction->ReplaceAllUsesWith(module.GetUndef(instruction->GetType()));
                    }
                    instruction->Erase();
                }
            }
            instruction = next;
        }
    }
    for (MirInstruction *variable : variables)
    {
        variable->Erase();
    }
    MirPreservedAnalyses preserved;
    preserved.Preserve(MirCfgAnalyses::Key);
    return preserved;
}
namespace
{
/// The integer or boolean value of *value* when it is computed from constants by integer arithmetic, comparisons and
/// logic, overflow-checked operations included, which constant folding leaves alone; false otherwise.
bool EvaluateConstant(const MirValue *value, uint64 &result, uint32 depth = 0)
{
    if (value->GetValueKind() == MirValueKind::Constant)
    {
        const auto *constant = static_cast<const MirConstant *>(value);
        if (!value->GetType()->Is(MirTypeKind::Int) && !value->GetType()->Is(MirTypeKind::Bool))
        {
            return false;
        }
        result = static_cast<uint64>(constant->GetInt());
        return true;
    }
    if (value->GetValueKind() != MirValueKind::Instruction || depth > 64)
    {
        return false;
    }
    const auto *instruction = static_cast<const MirInstruction *>(value);
    const Span<MirValue *const> operands = instruction->GetOperands();
    uint64 x = 0;
    uint64 y = 0;
    switch (instruction->GetOp())
    {
    case MirOp::Add:
    case MirOp::Sub:
    case MirOp::Mul:
    case MirOp::Equal:
    case MirOp::NotEqual:
    case MirOp::Less:
    case MirOp::LessEqual:
    case MirOp::Greater:
    case MirOp::GreaterEqual:
    case MirOp::LogicalAnd:
    case MirOp::LogicalOr:
        break;
    case MirOp::LogicalNot:
        if (!EvaluateConstant(operands[0], x, depth + 1))
        {
            return false;
        }
        result = x == 0 ? 1 : 0;
        return true;
    default:
        return false;
    }
    if (!operands[0]->GetType()->Is(MirTypeKind::Int) && !operands[0]->GetType()->Is(MirTypeKind::Bool))
    {
        return false;
    }
    if (!EvaluateConstant(operands[0], x, depth + 1) || !EvaluateConstant(operands[1], y, depth + 1))
    {
        return false;
    }
    const bool isSigned = operands[0]->GetType()->IsSigned();
    const auto sx = static_cast<int64>(x);
    const auto sy = static_cast<int64>(y);
    switch (instruction->GetOp())
    {
    case MirOp::Add:
        result = x + y;
        break;
    case MirOp::Sub:
        result = x - y;
        break;
    case MirOp::Mul:
        result = x * y;
        break;
    case MirOp::Equal:
        result = x == y ? 1 : 0;
        break;
    case MirOp::NotEqual:
        result = x != y ? 1 : 0;
        break;
    case MirOp::Less:
        result = (isSigned ? sx < sy : x < y) ? 1 : 0;
        break;
    case MirOp::LessEqual:
        result = (isSigned ? sx <= sy : x <= y) ? 1 : 0;
        break;
    case MirOp::Greater:
        result = (isSigned ? sx > sy : x > y) ? 1 : 0;
        break;
    case MirOp::GreaterEqual:
        result = (isSigned ? sx >= sy : x >= y) ? 1 : 0;
        break;
    case MirOp::LogicalAnd:
        result = x != 0 && y != 0 ? 1 : 0;
        break;
    default:
        result = x != 0 || y != 0 ? 1 : 0;
        break;
    }
    return true;
}
} // namespace

MirPreservedAnalyses MirRequirePass::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    // Only code an entry point reaches has its properties settled: a library function nothing inlined keeps its
    // parameters, so its requirements are removed without being checked.
    HashSet<const MirFunction *> reached;
    List<const MirFunction *> pending;
    for (const MirEntryPoint *entry : module.GetEntryPoints())
    {
        if (entry->Function != nullptr && !reached.Contains(entry->Function))
        {
            reached.Add(entry->Function);
            pending.Add(entry->Function);
        }
    }
    while (!pending.IsEmpty())
    {
        const MirFunction *function = pending.Pop();
        for (const MirBlock *block : function->GetBlocks())
        {
            for (const MirInstruction *instruction : *block)
            {
                if (instruction->GetOp() == MirOp::Call)
                {
                    const auto *callee = static_cast<const MirFunction *>(instruction->GetOperand(0));
                    if (!reached.Contains(callee))
                    {
                        reached.Add(callee);
                        pending.Add(callee);
                    }
                }
            }
        }
    }
    List<MirInstruction *> checks;
    List<MirInstruction *> unchecked;
    for (MirFunction *function : module.GetFunctions())
    {
        for (MirBlock *block : function->GetBlocks())
        {
            for (MirInstruction *instruction : *block)
            {
                if (instruction->GetOp() == MirOp::Intrinsic && instruction->GetIntrinsic() == MirIntrinsic::Require)
                {
                    (reached.Contains(function) ? checks : unchecked).Add(instruction);
                }
            }
        }
    }
    for (MirInstruction *require : unchecked)
    {
        analyses.GetFunctionAnalyses().Clear(*require->GetParent()->GetParent());
        require->Erase();
    }
    if (checks.IsEmpty() && unchecked.IsEmpty())
    {
        return MirPreservedAnalyses::All();
    }
    const Span<const StringView> files = module.GetFiles();
    const Span<const StringView> strings = module.GetStrings();
    for (MirInstruction *require : checks)
    {
        uint64 value = 0;
        const bool constant = EvaluateConstant(require->GetOperand(0), value);
        const bool holds = constant && value != 0;
        if (!holds)
        {
            const MirLocation location = require->Location;
            const StringView file = location.File != 0 && location.File <= files.Count()
                                        ? files[location.File - 1]
                                        : require->GetParent()->GetParent()->GetName();
            const auto index = static_cast<usize>(require->GetLiterals()[0]);
            module.ReportError(Format("{0}:{1}:{2}: {3}{4}", file, location.Line, location.Column,
                                      index < strings.Count() ? strings[index] : StringView("requirement failed"),
                                      StringView(constant ? "" : " (it depends on a value only known at run time)")));
        }
        analyses.GetFunctionAnalyses().Clear(*require->GetParent()->GetParent());
        require->Erase();
    }
    return MirPreservedAnalyses::None();
}
} // namespace Marlin
