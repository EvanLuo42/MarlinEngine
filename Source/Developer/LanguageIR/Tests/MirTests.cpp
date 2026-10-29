#include "Testing/Testing.h"

#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirVerifier.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
/// `fn sum(n) { i = 0; s = 0; while (i < n) { s += i; i += 1; } return s; }` in SSA form with a structured loop.
MirFunction *BuildSumLoop(MirModule &module)
{
    MirTypeContext &types = module.GetTypes();
    const MirType *u32 = types.GetInt(32, false);
    const MirType *parameters[] = {u32};
    MirFunction *function = module.AddFunction(types.GetFunction(u32, parameters), "Sum");
    MirBlock *entry = function->AddBlock("entry");
    MirBlock *header = function->AddBlock("header");
    MirBlock *body = function->AddBlock("body");
    MirBlock *latch = function->AddBlock("continue");
    MirBlock *exit = function->AddBlock("exit");
    header->Merge = {MirMergeKind::Loop, exit, latch};

    MirBuilder builder(module);
    builder.SetInsertPoint(entry);
    builder.Branch(header);

    builder.SetInsertPoint(header);
    MirInstruction *i = builder.Phi(u32);
    MirInstruction *sum = builder.Phi(u32);
    MirInstruction *condition = builder.Compare(MirOp::Less, i, function->GetArguments()[0]);
    builder.CondBranch(condition, body, exit);

    builder.SetInsertPoint(body);
    MirInstruction *nextSum = builder.Binary(MirOp::Add, sum, i);
    builder.Branch(latch);

    builder.SetInsertPoint(latch);
    MirInstruction *nextI = builder.Binary(MirOp::Add, i, module.GetInt(u32, 1));
    builder.Branch(header);

    i->AddOperand(module.GetInt(u32, 0));
    i->AddTarget(entry);
    i->AddOperand(nextI);
    i->AddTarget(latch);
    sum->AddOperand(module.GetInt(u32, 0));
    sum->AddTarget(entry);
    sum->AddOperand(nextSum);
    sum->AddTarget(latch);

    builder.SetInsertPoint(exit);
    builder.Return(sum);
    return function;
}
} // namespace

MARLIN_TEST(Mir, TypesAreUnique)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    MARLIN_EXPECT(types.GetFloat(32) == f32);
    MARLIN_EXPECT(types.GetVector(f32, 3) == types.GetVector(types.GetFloat(32), 3));
    MARLIN_EXPECT(types.GetVector(f32, 3) != types.GetVector(f32, 4));
    MARLIN_EXPECT(types.GetInt(32, true) != types.GetInt(32, false));
    const MirType *parameters[] = {f32, types.GetInt(32, true)};
    MARLIN_EXPECT(types.GetFunction(f32, parameters) == types.GetFunction(f32, parameters));
    MARLIN_EXPECT(types.GetPointer(f32, MirAddressSpace::Function) != types.GetPointer(f32, MirAddressSpace::Buffer));
}

MARLIN_TEST(Mir, LayoutFollowsTheLanguageRules)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *f16 = types.GetFloat(16);
    const MirType *float3 = types.GetVector(f32, 3);
    MARLIN_EXPECT(float3->GetSize() == 12 && float3->GetAlignment() == 16);

    const MirStructField fields[] = {
        {"Position", float3, MirTypeContext::AutoOffset},
        {"Radius", f32, MirTypeContext::AutoOffset},
        {"Tint", types.GetVector(f16, 3), MirTypeContext::AutoOffset},
        {"Transform", types.GetMatrix(f32, 3, 4), MirTypeContext::AutoOffset},
    };
    const MirType *light = types.CreateStruct("Light", fields);
    MARLIN_REQUIRE(light != nullptr);
    MARLIN_EXPECT(light->GetFields()[1].Offset == 12);
    MARLIN_EXPECT(light->GetFields()[2].Offset == 16);
    MARLIN_EXPECT(light->GetFields()[3].Offset == 32);
    MARLIN_EXPECT(light->GetFields()[3].Type->GetStride() == 16);
    MARLIN_EXPECT(light->GetSize() == 80 && light->GetAlignment() == 16);

    const MirType *array = types.GetArray(float3, 4);
    MARLIN_EXPECT(array->GetStride() == 16 && array->GetSize() == 64);

    const MirStructField misaligned[] = {{"A", f32, 0}, {"B", float3, 4}};
    MARLIN_EXPECT(types.CreateStruct("Bad", misaligned) == nullptr);
}

MARLIN_TEST(Mir, ConstantsAreInterned)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *i16 = types.GetInt(16, true);
    MARLIN_EXPECT(module.GetInt(i16, -1) == module.GetInt(i16, 0xFFFF));
    MARLIN_EXPECT(module.GetInt(i16, -1)->GetInt() == -1);
    const MirType *f16 = types.GetFloat(16);
    MARLIN_EXPECT(module.GetFloat(f16, 0.5)->GetBits() == 0x3800);
    MARLIN_EXPECT(module.GetFloat(f16, 0.5)->GetFloat() == 0.5);
    MARLIN_EXPECT(module.GetNull(f16) == module.GetNull(f16));
}

MARLIN_TEST(Mir, LoopVerifiesAndPrints)
{
    MirModule module;
    MirFunction *function = BuildSumLoop(module);
    List<String> errors;
    MARLIN_REQUIREF(VerifyMirModule(module, errors), "{0}", errors.IsEmpty() ? StringView() : errors[0].AsView());

    StringBuilder<1024> text;
    PrintMirFunction(*function, text);
    constexpr StringView Expected = R"(fn @Sum(%0: u32) -> u32
{
block0: // entry
    Branch block1
block1: // header loop merge(block4) continue(block3)
    %1 = Phi u32 u32 0, %5, block0, block3
    %2 = Phi u32 u32 0, %4, block0, block3
    %3 = Less bool %1, %0
    CondBranch %3, block2, block4
block2: // body
    %4 = Add u32 %2, %1
    Branch block3
block3: // continue
    %5 = Add u32 %1, u32 1
    Branch block1
block4: // exit
    Return %2
}
)";
    MARLIN_EXPECTF(text.AsView() == Expected, "{0}", text.AsView());
}

MARLIN_TEST(Mir, DominatorsOfALoop)
{
    MirModule module;
    MirFunction *function = BuildSumLoop(module);
    const MirControlFlow flow(*function);
    const Span<MirBlock *const> blocks = function->GetBlocks();
    MARLIN_EXPECT(flow.GetReversePostOrder().Count() == 5);
    MARLIN_EXPECT(flow.GetImmediateDominator(blocks[1]) == blocks[0]);
    MARLIN_EXPECT(flow.GetImmediateDominator(blocks[3]) == blocks[2]);
    MARLIN_EXPECT(flow.GetImmediateDominator(blocks[4]) == blocks[1]);
    MARLIN_EXPECT(flow.Dominates(blocks[1], blocks[3]));
    MARLIN_EXPECT(!flow.Dominates(blocks[2], blocks[4]));
    MARLIN_EXPECT(flow.GetPredecessors(blocks[1]).Count() == 2);
    MARLIN_EXPECT(flow.GetDominanceFrontier(blocks[3]).Count() == 1 && flow.GetDominanceFrontier(blocks[3])[0] == blocks[1]);
}

MARLIN_TEST(Mir, VerifierRejectsBrokenFunctions)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    MirFunction *function = module.AddFunction(types.GetFunction(f32, {}), "Broken");
    MirBlock *entry = function->AddBlock();
    MirBlock *other = function->AddBlock();
    MirBuilder builder(module);
    builder.SetInsertPoint(other);
    MirInstruction *late = builder.Binary(MirOp::Add, module.GetFloat(f32, 1), module.GetFloat(f32, 2));
    builder.Return(late);
    builder.SetInsertPoint(entry);
    MirInstruction *early = builder.Binary(MirOp::Mul, late, late);
    builder.Return(early);

    List<String> errors;
    MARLIN_EXPECT(!VerifyMirModule(module, errors));
    MARLIN_EXPECT(!errors.IsEmpty());
}
} // namespace Marlin::Tests
