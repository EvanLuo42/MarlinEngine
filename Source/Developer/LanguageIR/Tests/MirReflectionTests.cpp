#include "Testing/Testing.h"

#include "LanguageIR/MirBuilder.h"
#include "LanguageIR/MirReflection.h"

namespace Marlin::Tests
{
MARLIN_TEST(MirReflection, ReflectsTheBlocksAndVaryingsOfAnEntryPoint)
{
    MirModule module;
    MirTypeContext &types = module.GetTypes();
    const MirType *f32 = types.GetFloat(32);
    const MirType *u32 = types.GetInt(32, false);
    const MirStructField fields[] = {{"Scale", f32, MirTypeContext::AutoOffset},
                                     {"Count", u32, MirTypeContext::AutoOffset}};
    const MirType *args = types.CreateStruct("Args", fields);
    MirGlobal *root = module.AddGlobal(args, MirAddressSpace::Uniform, "args");
    MirGlobal *id = module.AddGlobal(types.GetVector(u32, 3), MirAddressSpace::Input, "id");
    id->Builtin = MirBuiltin::DispatchThreadId;
    MirGlobal *shared = module.AddGlobal(types.GetArray(f32, 64), MirAddressSpace::GroupShared, "tile");
    MirGlobal *quality = module.AddGlobal(u32, MirAddressSpace::Specialization, "Quality");
    quality->Location = 3;
    quality->Initializer = module.GetInt(u32, 2);
    module.AddString("tile {0}");

    MirFunction *main = module.AddFunction(types.GetFunction(types.GetVoid(), {}), "Main");
    MirBuilder builder(module);
    builder.SetInsertPoint(main->AddBlock("entry"));
    builder.Return();
    MirEntryPoint &entry = module.AddEntryPoint("Main", MirStage::Compute, main);
    entry.GroupSize[0] = 8;
    entry.GroupSize[1] = 8;
    entry.Features.Add("WaveOps");
    entry.Interface.Add(root);
    entry.Interface.Add(id);
    entry.Interface.Add(shared);
    entry.Interface.Add(quality);

    MirReflection reflection;
    ReflectMirEntryPoint(module, entry, reflection);
    MARLIN_EXPECT(reflection.Arguments.Count() == 2);
    MARLIN_EXPECT(reflection.ArgumentsSize == 8);
    MARLIN_EXPECT(reflection.Inputs.Count() == 1 && reflection.Inputs[0].Builtin == MirBuiltin::DispatchThreadId);
    MARLIN_EXPECT(reflection.GroupSharedBytes == 256);
    MARLIN_EXPECT(reflection.Specializations.Count() == 1 && reflection.Specializations[0].Id == 3 &&
                  reflection.Specializations[0].DefaultBits == 2);
    MARLIN_EXPECT(!reflection.TracesRays);

    // The lines whose format survived in the strings of the 01:10 build.
    const String text = PrintMirReflection(reflection);
    const StringView lines[] = {"entry Compute Main group(8, 8, 1)\n", "requires WaveOps\n", "uniform Scale: f32 @0 +4\n",
                                "uniform Count: u32 @4 +4\n", " builtin(DispatchThreadId)\n", "groupshared 256\n",
                                "specialization Quality id(3): u32 = 2\n", "format 0: \"tile {0}\"\n"};
    for (const StringView line : lines)
    {
        MARLIN_EXPECTF(text.AsView().Contains(line), "{0} lacks {1}", text.AsView(), line);
    }
}
} // namespace Marlin::Tests
