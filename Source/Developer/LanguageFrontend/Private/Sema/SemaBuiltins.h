#pragma once

#include "CoreTypes.h"
#include "LanguageIR/MirModule.h"
#include "String/StringView.h"

namespace Marlin
{
/// The magic type families a core library struct can be with `[Builtin("Name")]`.
enum class BuiltinTypeId : uint32
{
    None,
    IScalar,
    IScalarOrVector,
    INumeric,
    IInteger,
    IFloatingPoint,
    ILogical,
    IComparable,
    ScalarOf,
    Vector,
    Matrix,
    String,
    Range,
    Feature,
    Texture1D,
    Texture1DArray,
    Texture2D,
    Texture2DArray,
    Texture2DMultisample,
    Texture3D,
    TextureCube,
    TextureCubeArray,
    /// `Ptr<T, Access A>`: the device address of T in GPU memory (§4.5).
    Ptr,
    /// `Span<T, Access A>`: a Ptr and a length, an ordinary struct of the core library that indexes with clamping.
    Span,
    /// `byte`: the element of `Ptr<byte>`, which reads and writes at byte offsets; it has no arithmetic.
    Byte,
    Sampler,
    ComparisonSampler,
    AccelerationStructure,
    RayQuery,
    HitObject,
    CoopVector,
    CoopMatrix,
    MatrixRef,
    VectorRef,
    Vertices,
    Indices,
    Primitives,
    /// The script class `object`, which every class derives from (MarlinScript §3.2).
    Object,
    /// The script class whose members every `T[]` has (MarlinScript §3.5).
    Array,
    /// The types whose values have derivatives (§9.12); the compiler decides which conform.
    IDifferentiable,
};

/// The family named by a `[Builtin]` argument; None if unknown.
[[nodiscard]] BuiltinTypeId FindBuiltinType(StringView name);

/// What a bodyless `[Intrinsic("Name")]` call becomes: a MIR intrinsic, a MIR operation, or one of the memory
/// operations lowering builds from several MIR instructions.
enum class IntrinsicKind : uint8
{
    None,
    Mir,
    Op,
    /// `Read<U>(offset)` and `Write<U>(offset, value)` on a `Ptr<byte>`.
    PtrRead,
    PtrWrite,
    /// `p.Offset(bytes)`, `p + n`, `p.IsNull` and `p.Cast<U>()` on a Ptr, built from integer operations on its address.
    PtrOffset,
    PtrAdd,
    PtrIsNull,
    PtrCast,
    PtrEqual,
    PtrNotEqual,
    DebugPrint,
    DebugAssert,
    /// `T.DifferentialAdd(a, b)` and `T.DifferentialScale(d, s)` of a differentiable type (§9.12), built field by field.
    DifferentialAdd,
    DifferentialScale,
    /// `Compiler.Require(condition, message)`: a property that must hold once the code is folded (§9.4).
    Require,
    /// `Compiler.ToWords(value)` and `Compiler.FromWords<T>(words)`: the scalars of a value as 32-bit words and back,
    /// as a custom BSDF's closure payload is stored (07 §4.3).
    ToWords,
    FromWords,
};

struct IntrinsicTarget
{
    IntrinsicKind Kind = IntrinsicKind::None;
    MirIntrinsic Intrinsic = MirIntrinsic::None;
    MirOp Op = MirOp::Add;
};

[[nodiscard]] IntrinsicTarget FindIntrinsic(StringView name);

/// Whether the `[Builtin]` struct *id* is an ordinary struct the compiler only recognizes, such as Range and Span, rather
/// than a magic type with its own SemaType kind.
[[nodiscard]] constexpr bool IsPlainBuiltinStruct(BuiltinTypeId id)
{
    return id == BuiltinTypeId::Range || id == BuiltinTypeId::Span || id == BuiltinTypeId::Byte;
}
/// The system value named by `[SystemValue("Name")]`; None if unknown.
[[nodiscard]] MirBuiltin FindSystemValue(StringView name);

/// The functions `[CompileTime("Name")]` names, which the binder evaluates itself.
enum class CompileTimeFunction : uint8
{
    None,
    DeviceSupports,
    FeatureFromName,
    Implements,
    TypeId,
};

[[nodiscard]] CompileTimeFunction FindCompileTimeFunction(StringView name);
} // namespace Marlin
