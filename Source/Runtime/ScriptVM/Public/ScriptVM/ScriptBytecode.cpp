#include "ScriptVM/ScriptBytecode.h"

namespace Marlin
{
namespace
{
constexpr ScriptOpInfo OpInfos[] = {
#define MARLIN_SCRIPT_OPCODE(name, operands, words) {#name, ScriptOperands::operands, words},
    MARLIN_SCRIPT_OPCODES(MARLIN_SCRIPT_OPCODE)
#undef MARLIN_SCRIPT_OPCODE
};

constexpr StringView MathNames[] = {
#define MARLIN_SCRIPT_MATH_OP(name) #name,
    MARLIN_SCRIPT_MATH_OPS(MARLIN_SCRIPT_MATH_OP)
#undef MARLIN_SCRIPT_MATH_OP
};

constexpr StringView NumberNames[] = {"I8",  "U8",  "I16", "U16", "I32", "U32",
                                      "I64", "U64", "F16", "F32", "F64", "Bool"};

constexpr StringView FaultNames[] = {"NullReference", "IndexOutOfRange", "InvalidCast", "DivideByZero", "Overflow",
                                     "StackOverflow", "Timeout",         "AssertFailed", "Panic",        "Native"};

int64 SignExtend(uint64 value, uint32 bits)
{
    const uint64 sign = uint64{1} << (bits - 1);
    return static_cast<int64>((value ^ sign) - sign);
}
} // namespace

const ScriptOpInfo &GetScriptOpInfo(ScriptOp op)
{
    static constexpr ScriptOpInfo invalid = {"?", ScriptOperands::None, 0};
    return static_cast<uint32>(op) < static_cast<uint32>(ScriptOp::Count) ? OpInfos[static_cast<uint32>(op)] : invalid;
}

StringView GetScriptMathOpName(ScriptMathOp op)
{
    return static_cast<uint32>(op) < static_cast<uint32>(ScriptMathOp::Count) ? MathNames[static_cast<uint32>(op)]
                                                                              : StringView("?");
}

StringView GetScriptNumberName(ScriptNumber number)
{
    return static_cast<uint32>(number) < sizeof(NumberNames) / sizeof(NumberNames[0])
               ? NumberNames[static_cast<uint32>(number)]
               : StringView("?");
}

StringView GetScriptFaultName(ScriptFault fault)
{
    return static_cast<uint32>(fault) < sizeof(FaultNames) / sizeof(FaultNames[0])
               ? FaultNames[static_cast<uint32>(fault)]
               : StringView("?");
}

bool DecodeScriptInstruction(Span<const uint32> code, uint32 pc, ScriptDecodedInstruction &instruction)
{
    instruction = {};
    if (pc >= code.Count())
    {
        return false;
    }
    uint32 at = pc;
    uint32 prefix = 0;
    if (static_cast<ScriptOp>(code[at] & 0xFF) == ScriptOp::Wide)
    {
        prefix = code[at];
        instruction.IsWide = true;
        if (++at >= code.Count())
        {
            return false;
        }
    }
    const uint32 word = code[at];
    if ((word & 0xFF) >= static_cast<uint32>(ScriptOp::Count) || static_cast<ScriptOp>(word & 0xFF) == ScriptOp::Wide)
    {
        return false;
    }
    instruction.Op = static_cast<ScriptOp>(word & 0xFF);
    const ScriptOpInfo &info = GetScriptOpInfo(instruction.Op);
    const uint32 highA = (prefix >> 8) & 0xFF;
    const uint32 extension = prefix >> 16;
    switch (info.Operands)
    {
    case ScriptOperands::None:
        break;
    case ScriptOperands::A:
        instruction.A = ((word >> 8) & 0xFF) | (highA << 8);
        break;
    case ScriptOperands::AB:
        instruction.A = ((word >> 8) & 0xFF) | (highA << 8);
        instruction.B = ((word >> 16) & 0xFF) | ((extension & 0xFF) << 8);
        break;
    case ScriptOperands::ABC:
        instruction.A = ((word >> 8) & 0xFF) | (highA << 8);
        instruction.B = ((word >> 16) & 0xFF) | ((extension & 0xFF) << 8);
        instruction.C = (word >> 24) | ((extension >> 8) << 8);
        break;
    case ScriptOperands::ABx:
        instruction.A = ((word >> 8) & 0xFF) | (highA << 8);
        instruction.Bx = (word >> 16) | (extension << 16);
        break;
    case ScriptOperands::AsBx:
        instruction.A = ((word >> 8) & 0xFF) | (highA << 8);
        instruction.SBx = instruction.IsWide ? SignExtend((word >> 16) | (extension << 16), 32)
                                             : SignExtend(word >> 16, 16);
        break;
    case ScriptOperands::sAx:
        instruction.SBx = instruction.IsWide ? SignExtend((word >> 8) | ((extension & 0xFF) << 24), 32)
                                             : SignExtend(word >> 8, 24);
        break;
    }
    if (at + 1 + info.TrailingWords > code.Count())
    {
        return false;
    }
    for (uint32 i = 0; i < info.TrailingWords; ++i)
    {
        instruction.Trailing[i] = code[at + 1 + i];
    }
    instruction.Words = at + 1 + info.TrailingWords - pc;
    return true;
}

bool GetScriptJumpTarget(const ScriptDecodedInstruction &instruction, uint32 pc, int64 &target)
{
    const int64 next = static_cast<int64>(pc) + instruction.Words;
    if (instruction.Op == ScriptOp::Jump || instruction.Op == ScriptOp::Loop || instruction.Op == ScriptOp::JumpIf ||
        instruction.Op == ScriptOp::JumpIfNot)
    {
        target = next + instruction.SBx;
        return true;
    }
    if (IsScriptCompareJump(instruction.Op))
    {
        const uint32 offset = instruction.Trailing[GetScriptOpInfo(instruction.Op).TrailingWords - 1];
        target = next + static_cast<int32>(offset);
        return true;
    }
    return false;
}
} // namespace Marlin
