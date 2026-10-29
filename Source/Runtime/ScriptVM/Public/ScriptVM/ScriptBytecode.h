#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "String/StringView.h"

namespace Marlin
{
/// The first word of a script module (MarlinScript §6.1): "MSBC" read as little-endian.
inline constexpr uint32 ScriptModuleMagic = 0x4342534D;
inline constexpr uint32 ScriptFormatVersion = 4;
/// Bytes in a register slot: 128 bits, so vectors of four 32-bit lanes are one register (08 §5.1).
inline constexpr uint32 ScriptSlotBytes = 16;
/// Bytes of the object header (MarlinScript §9.1): the type pointer and the GC word.
inline constexpr uint32 ScriptObjectHeaderBytes = 16;
/// Where an array or string keeps its length, right after the header.
inline constexpr uint32 ScriptArrayLengthOffset = 16;

// RECOVERY-DECISION S1: the operand layouts of a 32-bit instruction word. The opcode is the low 8 bits; A is the
// next 8, then B and C 8 bits each, or Bx (16, unsigned) and sBx (16, signed) in their place; sAx is a signed
// 24-bit field over A, B and C. A Wide prefix word widens the next instruction: its bits 8..15 extend A and
// bits 16..31 extend B and C (8 bits each), Bx and sBx (16 bits) or sAx (8 bits, from bit 16).
/// How an instruction word lays out its operands.
enum class ScriptOperands : uint8
{
    None,
    A,
    AB,
    ABC,
    ABx,
    AsBx,
    sAx,
};

/// The instructions (MarlinScript §14), with their operands and the raw 32-bit words that follow them. Unless
/// noted, A is the destination register and B, C the source registers; multi-slot values sit in consecutive
/// registers. The K forms take their second operand as a 32-bit immediate in the trailing word (sign-extended for
/// the 64-bit ones). The 32/64/Ref forms of the loads and stores are those of the plain ones with the access implied:
/// 4 or 8 bytes, or a reference through the write barrier. The compare-and-jump instructions (JumpEqI32 to
/// JumpNeI64K) compare A with B, or with the immediate in their first trailing word, and jump when the comparison
/// holds by the signed offset in their last trailing word, counted from the next instruction. Loop is the safepoint
/// of a loop (the collection flag and the watchdog), then a jump like Jump's: at the loop header with offset 0, and in
/// place of the Jump of each unconditional back edge, to just after the header's Loop. MinF32 to LengthF32x3,
/// CrossF32x3 and NormalizeF32x3 are the Math operations of their names on F32 scalars or vectors of 3 or 4 lanes,
/// without a Math word. AndI32K to MulF32K take their second operand as a 32-bit immediate like the other K forms.
// clang-format off
#define MARLIN_SCRIPT_OPCODES(X) \
    X(Wide, None, 0) \
    X(Move, AB, 0) \
    X(MoveN, ABC, 0) \
    X(LoadI, AsBx, 0) \
    X(LoadK, ABx, 0) \
    X(LoadKN, ABx, 1) \
    X(Zero, AB, 0) \
    X(LoadString, ABx, 0) \
    X(AddI32, ABC, 0) X(SubI32, ABC, 0) X(MulI32, ABC, 0) \
    X(AddI32K, AB, 1) X(MulI32K, AB, 1) X(AddI64K, AB, 1) \
    X(AddI64, ABC, 0) X(SubI64, ABC, 0) X(MulI64, ABC, 0) \
    X(DivI32, ABC, 0) X(DivU32, ABC, 0) X(RemI32, ABC, 0) X(RemU32, ABC, 0) \
    X(DivI64, ABC, 0) X(DivU64, ABC, 0) X(RemI64, ABC, 0) X(RemU64, ABC, 0) \
    X(DivI32Common, ABC, 0) X(DivU32Common, ABC, 0) X(RemI32Common, ABC, 0) X(RemU32Common, ABC, 0) \
    X(DivI64Common, ABC, 0) X(DivU64Common, ABC, 0) X(RemI64Common, ABC, 0) X(RemU64Common, ABC, 0) \
    X(CheckedAddI32, ABC, 0) X(CheckedSubI32, ABC, 0) X(CheckedMulI32, ABC, 0) \
    X(CheckedAddU32, ABC, 0) X(CheckedSubU32, ABC, 0) X(CheckedMulU32, ABC, 0) \
    X(CheckedAddI64, ABC, 0) X(CheckedSubI64, ABC, 0) X(CheckedMulI64, ABC, 0) \
    X(CheckedAddU64, ABC, 0) X(CheckedSubU64, ABC, 0) X(CheckedMulU64, ABC, 0) \
    X(NegI32, AB, 0) X(NegI64, AB, 0) X(CheckedNegI32, AB, 0) X(CheckedNegI64, AB, 0) \
    X(And, ABC, 0) X(Or, ABC, 0) X(Xor, ABC, 0) X(Not, AB, 0) X(NotBool, AB, 0) \
    X(ShlI32, ABC, 0) X(ShlI64, ABC, 0) X(ShrI32, ABC, 0) X(ShrU32, ABC, 0) X(ShrI64, ABC, 0) X(ShrU64, ABC, 0) \
    X(AddF32, ABC, 0) X(SubF32, ABC, 0) X(MulF32, ABC, 0) X(DivF32, ABC, 0) X(RemF32, ABC, 0) X(NegF32, AB, 0) \
    X(AddF64, ABC, 0) X(SubF64, ABC, 0) X(MulF64, ABC, 0) X(DivF64, ABC, 0) X(RemF64, ABC, 0) X(NegF64, AB, 0) \
    X(EqI32, ABC, 0) X(NeI32, ABC, 0) X(EqI64, ABC, 0) X(NeI64, ABC, 0) \
    X(LtI32, ABC, 0) X(LeI32, ABC, 0) X(LtU32, ABC, 0) X(LeU32, ABC, 0) \
    X(LtI64, ABC, 0) X(LeI64, ABC, 0) X(LtU64, ABC, 0) X(LeU64, ABC, 0) \
    X(EqF32, ABC, 0) X(NeF32, ABC, 0) X(LtF32, ABC, 0) X(LeF32, ABC, 0) \
    X(EqF64, ABC, 0) X(NeF64, ABC, 0) X(LtF64, ABC, 0) X(LeF64, ABC, 0) \
    X(SExt8, AB, 0) X(ZExt8, AB, 0) X(SExt16, AB, 0) X(ZExt16, AB, 0) X(SExt32, AB, 0) X(ZExt32, AB, 0) \
    X(I32ToF32, AB, 0) X(U32ToF32, AB, 0) X(I64ToF32, AB, 0) X(U64ToF32, AB, 0) \
    X(I32ToF64, AB, 0) X(U32ToF64, AB, 0) X(I64ToF64, AB, 0) X(U64ToF64, AB, 0) \
    X(F32ToI32, AB, 0) X(F32ToU32, AB, 0) X(F32ToI64, AB, 0) X(F32ToU64, AB, 0) \
    X(F64ToI32, AB, 0) X(F64ToU32, AB, 0) X(F64ToI64, AB, 0) X(F64ToU64, AB, 0) \
    X(F32ToF64, AB, 0) X(F64ToF32, AB, 0) X(F16ToF32, AB, 0) X(F32ToF16, AB, 0) \
    X(ConvChecked, ABC, 0) \
    X(AddF32x4, ABC, 0) X(SubF32x4, ABC, 0) X(MulF32x4, ABC, 0) X(DivF32x4, ABC, 0) X(NegF32x4, AB, 0) \
    X(AddF64x2, ABC, 0) X(SubF64x2, ABC, 0) X(MulF64x2, ABC, 0) X(DivF64x2, ABC, 0) X(NegF64x2, AB, 0) \
    X(AddI32x4, ABC, 0) X(SubI32x4, ABC, 0) X(MulI32x4, ABC, 0) X(NegI32x4, AB, 0) \
    X(AddI64x2, ABC, 0) X(SubI64x2, ABC, 0) X(MulI64x2, ABC, 0) X(NegI64x2, AB, 0) \
    X(Splat32, AB, 0) X(Splat64, AB, 0) \
    X(Extract, AB, 2) X(Insert, AB, 2) X(ExtractDyn, ABC, 2) X(InsertDyn, ABC, 2) \
    X(RefLocal, AB, 0) X(RefStatic, ABx, 0) X(RefOffset, AB, 1) X(RefIndex, ABC, 2) X(RefIndexClamp, ABC, 2) \
    X(Load, AB, 1) X(Store, AB, 1) \
    X(Load32, AB, 0) X(Load64, AB, 0) X(Store32, AB, 0) X(Store64, AB, 0) X(StoreRef, AB, 0) \
    X(New, ABx, 0) X(NewArray, AB, 1) X(ArrayLength, AB, 0) \
    X(RefField, AB, 1) X(LoadField, AB, 2) X(StoreField, AB, 2) \
    X(LoadField32, AB, 1) X(LoadField64, AB, 1) X(StoreField32, AB, 1) X(StoreField64, AB, 1) \
    X(StoreFieldRef, AB, 1) \
    X(RefElem, ABC, 1) X(LoadElem, ABC, 2) X(StoreElem, ABC, 2) \
    X(LoadElem32, ABC, 1) X(LoadElem64, ABC, 1) X(StoreElem32, ABC, 1) X(StoreElem64, ABC, 1) \
    X(StoreElemRef, ABC, 1) \
    X(Box, AB, 1) X(Unbox, AB, 1) X(IsInstance, AB, 1) X(Cast, AB, 1) X(TryCast, AB, 1) X(TypeOf, ABx, 0) \
    X(NullCheck, AB, 0) \
    X(MakeDelegate, ABC, 2) X(CombineDelegates, ABC, 0) X(RemoveDelegates, ABC, 0) \
    X(Jump, sAx, 0) X(JumpIf, AsBx, 0) X(JumpIfNot, AsBx, 0) X(Switch, ABx, 0) X(Loop, sAx, 0) \
    X(JumpEqI32, AB, 1) X(JumpNeI32, AB, 1) X(JumpLtI32, AB, 1) X(JumpLeI32, AB, 1) \
    X(JumpLtU32, AB, 1) X(JumpLeU32, AB, 1) \
    X(JumpEqI64, AB, 1) X(JumpNeI64, AB, 1) X(JumpLtI64, AB, 1) X(JumpLeI64, AB, 1) \
    X(JumpLtU64, AB, 1) X(JumpLeU64, AB, 1) \
    X(JumpEqF32, AB, 1) X(JumpNeF32, AB, 1) X(JumpLtF32, AB, 1) X(JumpLeF32, AB, 1) \
    X(JumpNotLtF32, AB, 1) X(JumpNotLeF32, AB, 1) \
    X(JumpEqF64, AB, 1) X(JumpNeF64, AB, 1) X(JumpLtF64, AB, 1) X(JumpLeF64, AB, 1) \
    X(JumpNotLtF64, AB, 1) X(JumpNotLeF64, AB, 1) \
    X(JumpEqI32K, A, 2) X(JumpNeI32K, A, 2) X(JumpLtI32K, A, 2) X(JumpLeI32K, A, 2) \
    X(JumpGtI32K, A, 2) X(JumpGeI32K, A, 2) X(JumpEqI64K, A, 2) X(JumpNeI64K, A, 2) \
    X(Return, AB, 0) X(Fault, ABx, 0) \
    X(Call, ABx, 0) X(CallVirtual, ABx, 0) X(CallInterface, ABx, 0) X(CallDelegate, ABC, 0) \
    X(Await, ABC, 0) X(Yield, AB, 0) \
    X(Math, ABC, 1) \
    X(MinF32, ABC, 0) X(MaxF32, ABC, 0) X(AbsF32, AB, 0) X(SqrtF32, AB, 0) X(FloorF32, AB, 0) \
    X(DotF32x3, ABC, 0) X(DotF32x4, ABC, 0) X(LengthF32x3, AB, 0) X(CrossF32x3, ABC, 0) X(NormalizeF32x3, AB, 0) \
    X(AndI32K, AB, 1) X(ShlI32K, AB, 1) X(ShrI32K, AB, 1) X(ShrU32K, AB, 1) X(AddF32K, AB, 1) X(MulF32K, AB, 1)
// clang-format on

enum class ScriptOp : uint8
{
#define MARLIN_SCRIPT_OPCODE(name, operands, words) name,
    MARLIN_SCRIPT_OPCODES(MARLIN_SCRIPT_OPCODE)
#undef MARLIN_SCRIPT_OPCODE
        Count,
};

static_assert(static_cast<uint32>(ScriptOp::Count) <= 256);

struct ScriptOpInfo
{
    StringView Name;
    ScriptOperands Operands = ScriptOperands::None;
    /// Raw words after the instruction.
    uint8 TrailingWords = 0;
};

[[nodiscard]] SCRIPTVM_API const ScriptOpInfo &GetScriptOpInfo(ScriptOp op);

/// How Load, Store and the field, element and register accesses read and write a value: the low 8 bits of their
/// access word, with a 24-bit payload above. Registers hold integers narrower than 32 bits sign- or
/// zero-extended to 32 and every other value as its bytes in memory.
enum class ScriptAccess : uint8
{
    I8,
    U8,
    I16,
    U16,
    B32,
    B64,
    /// The payload is the size in bytes.
    Bytes,
    /// A reference stored with the write barrier.
    Reference,
    /// A value of the shape the payload names, with the write barrier for each reference in it.
    Typed,
};

[[nodiscard]] constexpr uint32 MakeScriptAccess(ScriptAccess access, uint32 payload = 0)
{
    return static_cast<uint32>(access) | (payload << 8);
}

[[nodiscard]] constexpr ScriptAccess GetScriptAccess(uint32 word)
{
    return static_cast<ScriptAccess>(word & 0xFF);
}

[[nodiscard]] constexpr uint32 GetScriptAccessPayload(uint32 word)
{
    return word >> 8;
}

/// Numeric kinds of ConvChecked (from in the high 4 bits of C, to in the low 4) and Math.
enum class ScriptNumber : uint8
{
    I8,
    U8,
    I16,
    U16,
    I32,
    U32,
    I64,
    U64,
    F16,
    F32,
    F64,
    Bool,
};

/// The operations of Math, which correspond to MIR intrinsics (MarlinLanguage §11.2) of the same names.
// clang-format off
#define MARLIN_SCRIPT_MATH_OPS(X) \
    X(Abs) X(Sign) X(Floor) X(Ceil) X(Round) X(Trunc) X(Fract) X(Sqrt) X(InverseSqrt) X(Exp) X(Exp2) X(Log) X(Log2) \
    X(Pow) X(Sin) X(Cos) X(Tan) X(Asin) X(Acos) X(Atan) X(Atan2) X(Sinh) X(Cosh) X(Tanh) X(Min) X(Max) X(Clamp) \
    X(Lerp) X(Step) X(SmoothStep) X(Fma) X(Dot) X(Cross) X(Length) X(Distance) X(Normalize) X(Reflect) X(Refract) \
    X(FaceForward) X(Determinant) X(Inverse) X(IsNan) X(IsInf) X(Any) X(All) X(CountBits) X(FirstBitHigh) \
    X(FirstBitLow) X(ReverseBits) X(PackHalf2x16) X(UnpackHalf2x16)
// clang-format on

enum class ScriptMathOp : uint8
{
#define MARLIN_SCRIPT_MATH_OP(name) name,
    MARLIN_SCRIPT_MATH_OPS(MARLIN_SCRIPT_MATH_OP)
#undef MARLIN_SCRIPT_MATH_OP
        Count,
};

[[nodiscard]] SCRIPTVM_API StringView GetScriptMathOpName(ScriptMathOp op);
[[nodiscard]] SCRIPTVM_API StringView GetScriptNumberName(ScriptNumber number);

// RECOVERY-DECISION S2: the Math word is the operation (bits 0..7), the lane number (8..11), the lanes or matrix
// rows (12..15) and the third operand register (16..31).
/// The trailing word of Math: *op* on *lanes* lanes of *number*, with the register of a third operand.
[[nodiscard]] constexpr uint32 MakeScriptMath(ScriptMathOp op, ScriptNumber number, uint32 lanes, uint32 third)
{
    return static_cast<uint32>(op) | (static_cast<uint32>(number) << 8) | ((lanes & 0xF) << 12) | (third << 16);
}

/// What a fault is (MarlinScript §8.2); the A operand of Fault.
enum class ScriptFault : uint8
{
    NullReference,
    IndexOutOfRange,
    InvalidCast,
    DivideByZero,
    Overflow,
    StackOverflow,
    Timeout,
    AssertFailed,
    Panic,
    Native,
};

[[nodiscard]] SCRIPTVM_API StringView GetScriptFaultName(ScriptFault fault);

/// An instruction as decoded from its words, the Wide prefix applied.
struct ScriptDecodedInstruction
{
    ScriptOp Op = ScriptOp::Wide;
    uint32 A = 0;
    uint32 B = 0;
    uint32 C = 0;
    uint32 Bx = 0;
    int64 SBx = 0;
    uint32 Trailing[2] = {};
    bool IsWide = false;
    /// Words the instruction takes, with its prefix and trailing words.
    uint32 Words = 0;
};

/// Decodes the instruction at *pc*; false when it is malformed or runs past the end.
[[nodiscard]] SCRIPTVM_API bool DecodeScriptInstruction(Span<const uint32> code, uint32 pc,
                                                        ScriptDecodedInstruction &instruction);

/// Whether *op* compares two values and jumps on the result.
[[nodiscard]] constexpr bool IsScriptCompareJump(ScriptOp op)
{
    return op >= ScriptOp::JumpEqI32 && op <= ScriptOp::JumpNeI64K;
}

/// The word the decoded *instruction* at *pc* may jump to, for Jump, Loop, JumpIf, JumpIfNot and the compare-and-jump
/// instructions; false for the others.
[[nodiscard]] SCRIPTVM_API bool GetScriptJumpTarget(const ScriptDecodedInstruction &instruction, uint32 pc,
                                                    int64 &target);
} // namespace Marlin
