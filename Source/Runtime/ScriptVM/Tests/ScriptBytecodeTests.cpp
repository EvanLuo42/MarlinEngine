#include "Testing/Testing.h"

#include "ScriptVM/ScriptBytecode.h"
#include "ScriptVM/ScriptModuleData.h"

namespace Marlin::Tests
{
MARLIN_TEST(ScriptBytecode, DecodesWidePrefixesAndTrailingWords)
{
    auto op = [](ScriptOp value) { return static_cast<uint32>(value); };
    const uint32 words[] = {
        op(ScriptOp::AddI32) | (1u << 8) | (2u << 16) | (3u << 24),
        op(ScriptOp::Wide) | (1u << 8) | (0x0002u << 16),
        op(ScriptOp::MoveN) | (0x04u << 8) | (0x05u << 16) | (6u << 24),
        op(ScriptOp::JumpIf) | (7u << 8) | (0xFFFEu << 16),
        op(ScriptOp::Wide) | (0xFFFFu << 16),
        op(ScriptOp::LoadI) | (0xFFF0u << 16),
        op(ScriptOp::Extract) | (1u << 8) | (2u << 16),
        12,
        MakeScriptAccess(ScriptAccess::Bytes, 24),
    };
    const Span<const uint32> code(words, sizeof(words) / sizeof(words[0]));
    ScriptDecodedInstruction instruction;
    MARLIN_REQUIRE(DecodeScriptInstruction(code, 0, instruction));
    MARLIN_EXPECT(instruction.Op == ScriptOp::AddI32 && instruction.A == 1 && instruction.B == 2 &&
                  instruction.C == 3 && instruction.Words == 1);
    MARLIN_REQUIRE(DecodeScriptInstruction(code, 1, instruction));
    MARLIN_EXPECT(instruction.Op == ScriptOp::MoveN && instruction.IsWide && instruction.A == 0x104 &&
                  instruction.B == 0x205 && instruction.C == 6 && instruction.Words == 2);
    MARLIN_REQUIRE(DecodeScriptInstruction(code, 3, instruction));
    MARLIN_EXPECT(instruction.Op == ScriptOp::JumpIf && instruction.A == 7 && instruction.SBx == -2);
    MARLIN_REQUIRE(DecodeScriptInstruction(code, 4, instruction));
    MARLIN_EXPECT(instruction.Op == ScriptOp::LoadI && instruction.SBx == -16 && instruction.Words == 2);
    MARLIN_REQUIRE(DecodeScriptInstruction(code, 6, instruction));
    MARLIN_EXPECT(instruction.Op == ScriptOp::Extract && instruction.Trailing[0] == 12 &&
                  GetScriptAccess(instruction.Trailing[1]) == ScriptAccess::Bytes &&
                  GetScriptAccessPayload(instruction.Trailing[1]) == 24 && instruction.Words == 3);
    MARLIN_EXPECT(!DecodeScriptInstruction(code.Slice(0, 7), 6, instruction));
    MARLIN_EXPECT(GetScriptOpInfo(ScriptOp::Extract).TrailingWords == 2);
}

MARLIN_TEST(ScriptBytecode, ModulesRoundTrip)
{
    ScriptModuleData module;
    module.Strings = {String("Test"), String("Add(int, int)"), String("/Project/Language/Scripts/Test.mscript")};
    module.Package = 0;
    module.InterfaceHash = 0x1234;
    ScriptShape &integer = module.Shapes.AddDefaulted();
    integer.Kind = ScriptShapeKind::Int;
    integer.Width = 32;
    integer.Signed = true;
    integer.Size = 4;
    integer.Alignment = 4;
    ScriptShape &signature = module.Shapes.AddDefaulted();
    signature.Kind = ScriptShapeKind::Function;
    signature.Element = 0;
    signature.Parameters = {0, 0};
    ScriptFunctionRecord &function = module.Functions.AddDefaulted();
    function.Name = 1;
    function.Signature = 1;
    function.ParameterSlots = 2;
    function.FrameSlots = 3;
    function.ReturnSlots = 1;
    function.Flags = ScriptFunctionFlags::Debuggable;
    function.Code = {static_cast<uint32>(ScriptOp::AddI32) | (2u << 8) | (0u << 16) | (1u << 24),
                     static_cast<uint32>(ScriptOp::Return) | (2u << 8) | (1u << 16)};
    function.Blocks = {0};
    ScriptSlot constant;
    constant.Bytes[0] = 42;
    function.Constants.Add(constant);
    function.Switches.AddDefaulted().Cases.Add({-5, 1});
    function.MapBytes = 1;
    function.Maps = {0, 0};
    function.Safepoints.Add({1, 0});
    AppendScriptLineEntry(function.LineTable, {}, {0, 0, 3, 9});
    AppendScriptLineEntry(function.LineTable, {0, 0, 3, 9}, {1, 0, 2, 5});
    module.StaticSize = 8;
    module.StaticAlignment = 4;
    module.Statics.Add({0, 4, 0});
    module.Exports.Add({ScriptExportKind::Function, 0, 0xABCD});
    module.Files.Add({2, 0x99});

    List<uint8> bytes;
    WriteScriptModule(module, bytes);
    ScriptModuleData read;
    String error;
    MARLIN_REQUIREF(ReadScriptModule(bytes.AsSpan(), read, error), "{0}", error.AsView());
    MARLIN_EXPECT(read.Strings.Count() == 3 && read.Strings[1].AsView() == "Add(int, int)");
    MARLIN_EXPECT(read.InterfaceHash == 0x1234 && read.Shapes.Count() == 2 && read.Shapes[1].Parameters.Count() == 2);
    MARLIN_REQUIRE(read.Functions.Count() == 1);
    const ScriptFunctionRecord &copy = read.Functions[0];
    MARLIN_EXPECT(copy.Code == function.Code && copy.Constants[0] == constant && copy.FrameSlots == 3);
    MARLIN_EXPECT(copy.Switches.Count() == 1 && copy.Switches[0].Cases[0].Value == -5);
    MARLIN_EXPECT(copy.Safepoints.Count() == 1 && copy.Safepoints[0].Pc == 1 && copy.Maps.Count() == 2);
    MARLIN_EXPECT(read.Statics.Count() == 1 && read.Exports[0].Hash == 0xABCD && read.Files[0].ContentHash == 0x99);
    List<ScriptLineEntry> lines;
    MARLIN_REQUIRE(DecodeScriptLineTable(copy.LineTable.AsSpan(), lines));
    MARLIN_EXPECT(lines.Count() == 2 && lines[1].Pc == 1 && lines[1].Line == 2 && lines[0].Column == 9 &&
                  lines[0].Line == 3);

    bytes[4] = 99;
    MARLIN_EXPECT(!ReadScriptModule(bytes.AsSpan(), read, error));
    WriteScriptModule(module, bytes);
    bytes.Resize(bytes.Count() - 3);
    MARLIN_EXPECT(!ReadScriptModule(bytes.AsSpan(), read, error));
    module.Functions[0].Name = 50;
    WriteScriptModule(module, bytes);
    MARLIN_EXPECT(!ReadScriptModule(bytes.AsSpan(), read, error));
}
} // namespace Marlin::Tests
