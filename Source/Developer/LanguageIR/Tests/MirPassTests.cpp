#include "Testing/Testing.h"

#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPasses.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirTarget.h"
#include "LanguageIR/MirVerifier.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
usize CountOps(const MirFunction &function, MirOp op)
{
    usize count = 0;
    for (const MirBlock *block : function.GetBlocks())
    {
        for (const MirInstruction *instruction : *block)
        {
            count += instruction->GetOp() == op ? 1 : 0;
        }
    }
    return count;
}

/// Runs *pipeline* with verification after every pass.
void Optimize(MirModule &module, StringView pipeline)
{
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes({.VerifyEach = true});
    String error;
    MARLIN_REQUIREF(registry.ParsePipeline(pipeline, passes, error, {.VerifyEach = true}), "{0}", error.AsView());
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    List<String> problems;
    StringBuilder<2048> text;
    PrintMirModule(module, text);
    MARLIN_REQUIREF(VerifyMirModule(module, problems), "{0}\n{1}", problems.IsEmpty() ? StringView() : problems[0].AsView(),
                    text.AsView());
}

/// `sum = 0; for (i = 0; i < n; ++i) sum += i; return sum;` with both locals in variables, as the frontend lowers it.
MirFunction *BuildLoopWithVariables(MirModule &module)
{
    MirTypeContext &types = module.GetTypes();
    const MirType *u32 = types.GetInt(32, false);
    const MirType *parameters[] = {u32};
    MirFunction *function = module.AddFunction(types.GetFunction(u32, parameters), "Sum");
    MirBlock *entry = function->AddBlock("entry");
    MirBlock *header = function->AddBlock("header");
    MirBlock *check = function->AddBlock("check");
    MirBlock *body = function->AddBlock("body");
    MirBlock *latch = function->AddBlock("continue");
    MirBlock *exit = function->AddBlock("exit");
    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    MirInstruction *sum = builder.Variable(u32, "sum");
    MirInstruction *i = builder.Variable(u32, "i");
    builder.Store(sum, module.GetInt(u32, 0));
    builder.Store(i, module.GetInt(u32, 0));
    builder.Branch(header);
    header->Merge = {MirMergeKind::Loop, exit, latch};
    builder.SetInsertPoint(header);
    builder.Branch(check);
    builder.SetInsertPoint(check);
    builder.CondBranch(builder.Compare(MirOp::Less, builder.Load(i), function->GetArguments()[0]), body, exit);
    builder.SetInsertPoint(body);
    builder.Store(sum, builder.Binary(MirOp::Add, builder.Load(sum), builder.Load(i)));
    builder.Branch(latch);
    builder.SetInsertPoint(latch);
    builder.Store(i, builder.Binary(MirOp::Add, builder.Load(i), module.GetInt(u32, 1)));
    builder.Branch(header);
    builder.SetInsertPoint(exit);
    builder.Return(builder.Load(sum));
    return function;
}
} // namespace

MARLIN_TEST(MirPasses, Mem2RegBuildsPhis)
{
    MirModule module;
    MirFunction *function = BuildLoopWithVariables(module);
    Optimize(module, "function(mem2reg,dce)");
    MARLIN_EXPECT(CountOps(*function, MirOp::Variable) == 0);
    MARLIN_EXPECT(CountOps(*function, MirOp::Load) == 0);
    MARLIN_EXPECT(CountOps(*function, MirOp::Phi) == 2);
}

MARLIN_TEST(MirPasses, FoldsConstantsAndDeadBranches)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    MirFunction *function = module.AddFunction(types.GetFunction(f32, {}), "Pick");
    MirBlock *entry = function->AddBlock("entry");
    MirBlock *whenTrue = function->AddBlock("true");
    MirBlock *whenFalse = function->AddBlock("false");
    MirBlock *merge = function->AddBlock("merge");
    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    MirInstruction *condition = builder.Compare(MirOp::Less, module.GetFloat(f32, 1), module.GetFloat(f32, 2));
    entry->Merge = {MirMergeKind::Selection, merge, nullptr};
    builder.CondBranch(condition, whenTrue, whenFalse);
    builder.SetInsertPoint(whenTrue);
    MirInstruction *a = builder.Binary(MirOp::Mul, module.GetFloat(f32, 3), module.GetFloat(f32, 4));
    builder.Branch(merge);
    builder.SetInsertPoint(whenFalse);
    builder.Branch(merge);
    builder.SetInsertPoint(merge);
    MirInstruction *phi = builder.Phi(f32);
    phi->AddOperand(a);
    phi->AddTarget(whenTrue);
    phi->AddOperand(module.GetFloat(f32, 0));
    phi->AddTarget(whenFalse);
    builder.Return(phi);

    Optimize(module, "function(constfold,deadbranch,simplifycfg,cse,dce)");
    MARLIN_EXPECT(function->GetBlocks().Count() == 3);
    MARLIN_EXPECT(CountOps(*function, MirOp::CondBranch) == 0);
    const MirInstruction *ret = function->GetBlocks().Last()->GetTerminator();
    MARLIN_REQUIRE(ret != nullptr && ret->GetOp() == MirOp::Return);
    const MirValue *value = ret->GetOperand(0);
    MARLIN_EXPECT(value->GetValueKind() == MirValueKind::Constant &&
                  static_cast<const MirConstant *>(value)->GetFloat() == 12.0);
}

MARLIN_TEST(MirPasses, MergesReturnsAndInlines)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *i32 = types.GetInt(32, true);
    const MirType *parameters[] = {i32};
    MirFunction *absolute = module.AddFunction(types.GetFunction(i32, parameters), "Absolute");
    {
        MirBlock *entry = absolute->AddBlock("entry");
        MirBlock *negative = absolute->AddBlock("negative");
        MirBlock *positive = absolute->AddBlock("positive");
        MirBuilder builder(module);
        builder.SetInsertPoint(entry);
        MirValue *x = absolute->GetArguments()[0];
        entry->Merge = {MirMergeKind::Selection, positive, nullptr};
        builder.CondBranch(builder.Compare(MirOp::Less, x, module.GetInt(i32, 0)), negative, positive);
        builder.SetInsertPoint(negative);
        builder.Return(builder.Unary(MirOp::Negate, x));
        builder.SetInsertPoint(positive);
        builder.Return(x);
    }
    MirFunction *main = module.AddFunction(types.GetFunction(i32, parameters), "Main");
    {
        MirBuilder builder(module);
        builder.SetInsertPoint(main->AddBlock("entry"));
        MirValue *arguments[] = {main->GetArguments()[0]};
        MirValue *first = builder.Call(absolute, arguments);
        MirValue *again[] = {first};
        builder.Return(builder.Binary(MirOp::Add, first, builder.Call(absolute, again)));
    }
    MirFunction *entry = module.AddFunction(types.GetFunction(types.GetVoid(), {}), "Entry");
    {
        MirBuilder builder(module);
        builder.SetInsertPoint(entry->AddBlock("entry"));
        MirValue *arguments[] = {module.GetInt(i32, -5)};
        builder.Call(main, arguments);
        builder.Return();
    }
    module.AddEntryPoint("Entry", MirStage::Compute, entry);

    Optimize(module, "function(mergereturn),inline");
    MARLIN_EXPECT(module.GetFunctions().Count() == 1);
    MARLIN_EXPECT(CountOps(*entry, MirOp::Call) == 0);
    MARLIN_EXPECT(CountOps(*entry, MirOp::Return) == 1);
}

MARLIN_TEST(MirPasses, PipelineErrors)
{
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes;
    String error;
    MARLIN_EXPECT(!registry.ParsePipeline("function(mem2reg,nonsense)", passes, error));
    MARLIN_EXPECT(error.AsView().Contains("nonsense"));
    MARLIN_EXPECT(!registry.ParsePipeline("function(dce", passes, error));
    MARLIN_EXPECT(registry.ParsePipeline(MirShaderPipeline, passes, error));
}

MARLIN_TEST(MirPasses, AnalysesAreCachedAndInvalidated)
{
    MirModule module;
    MirFunction *function = BuildLoopWithVariables(module);
    MirFunctionAnalysisManager analyses;
    const MirControlFlow &first = analyses.GetResult<MirControlFlowAnalysis>(*function);
    MARLIN_EXPECT(&analyses.GetResult<MirControlFlowAnalysis>(*function) == &first);
    MirPreservedAnalyses cfgOnly;
    cfgOnly.Preserve(MirCfgAnalyses::Key);
    analyses.Invalidate(*function, cfgOnly);
    MARLIN_EXPECT(analyses.GetCachedResult<MirControlFlowAnalysis>(*function) != nullptr);
    analyses.Invalidate(*function, MirPreservedAnalyses::None());
    MARLIN_EXPECT(analyses.GetCachedResult<MirControlFlowAnalysis>(*function) == nullptr);
}
namespace
{
/// A target that expands matrix arithmetic and saturate, and cannot express wave operations or 64-bit floats.
class TestTarget final : public MirTarget
{
public:
    TestTarget()
    {
        AddStage(MirStage::Compute);
        const uint32 integers[] = {32};
        const uint32 floats[] = {16, 32};
        SetScalarWidths(integers, floats);
        SetLegality(MirOp::Add, MirOperandClass::Matrix, MirLegality::Expand);
        SetLegality(MirIntrinsic::Saturate, MirLegality::Expand);
        SetLegality(MirIntrinsic::Dot, MirOperandClass::Integer, MirLegality::Expand);
        SetLegality(MirIntrinsic::WaveActiveSum, MirLegality::Unsupported);
        AddFeature("Test");
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "test";
    }

    bool Emit(MirModule &, const MirTargetOptions &, List<uint8> &, List<String> &) const override
    {
        return true;
    }
};
} // namespace

MARLIN_TEST(MirTarget, LegalizeExpandsAndReports)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *i32 = types.GetInt(32, true);
    const MirType *matrix = types.GetMatrix(f32, 2, 2);
    const MirType *int2 = types.GetVector(i32, 2);
    const MirType *parameters[] = {matrix, int2, f32};
    MirFunction *function = module.AddFunction(types.GetFunction(f32, parameters), "Shade");
    MirBuilder builder(module);
    builder.SetInsertPoint(function->AddBlock("entry"));
    MirValue *m = function->GetArguments()[0];
    MirValue *v = function->GetArguments()[1];
    MirValue *x = function->GetArguments()[2];
    MirValue *sum = builder.Binary(MirOp::Add, m, m);
    MirValue *dotArguments[] = {v, v};
    MirValue *dot = builder.Intrinsic(MirIntrinsic::Dot, i32, dotArguments);
    MirValue *saturateArguments[] = {x};
    MirValue *saturated = builder.Intrinsic(MirIntrinsic::Saturate, f32, saturateArguments);
    MirValue *waveArguments[] = {saturated};
    MirValue *wave = builder.Intrinsic(MirIntrinsic::WaveActiveSum, f32, waveArguments);
    const uint32 corner[] = {1, 1};
    MirValue *total = builder.Binary(MirOp::Add, builder.Extract(sum, corner), builder.Convert(dot, f32));
    builder.Return(builder.Binary(MirOp::Add, total, wave));
    module.AddGlobal(types.GetFloat(64), MirAddressSpace::GroupShared, "Wide");

    MirModulePassManager passes;
    List<String> errors;
    MirTargetRegistry registry;
    registry.Register(MakeUnique<TestTarget>());
    const MirTarget *target = registry.Find("test");
    MARLIN_REQUIRE(target != nullptr && target->GetFeatures().Count() == 1);
    target->AddLegalizationPasses(passes, errors);
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);

    List<String> problems;
    MARLIN_REQUIREF(VerifyMirModule(module, problems), "{0}", problems.IsEmpty() ? StringView() : problems[0].AsView());
    MARLIN_EXPECT(errors.Count() == 2);
    bool reportedWave = false;
    bool reportedType = false;
    for (const String &error : errors)
    {
        reportedWave = reportedWave || error.AsView().Contains("cannot express WaveActiveSum");
        reportedType = reportedType || error.AsView().Contains("has no type `f64`");
    }
    MARLIN_EXPECT(reportedWave && reportedType);
    StringBuilder<2048> text;
    PrintMirFunction(*function, text);
    MARLIN_EXPECTF(!text.AsView().Contains("Saturate") && text.AsView().Contains("Clamp") &&
                       !text.AsView().Contains("Dot") && !text.AsView().Contains("Add f32x2x2"),
                   "{0}", text.AsView());
}
} // namespace Marlin::Tests
