#pragma once

#include "Sema/SemaBuiltins.h"
#include "Sema/SemaSymbols.h"

namespace Marlin
{
/// The checked form of an expression: every name resolved, every operation typed, every conversion explicit.
enum class BoundKind : uint8
{
    Constant,
    Local,
    Global,
    Field,
    Swizzle,
    Index,
    BufferElement,
    Call,
    Intrinsic,
    Derivative,
    Construct,
    Convert,
    Bitcast,
    Unary,
    Binary,
    ShortCircuit,
    Assign,
    IncDec,
    Conditional,
    Zero,
    Sequence,
    ObjectField,
    NewObject,
    NewArray,
    ArrayElement,
    Property,
    TypeConversion,
    TypeTest,
    TypeOf,
    Let,
    Unbound,
    MakeDelegate,
    Lambda,
    InvokeDelegate,
    CombineDelegates,
    Await,
};

/// How a call reaches a method of a class or interface (MarlinScript §3.4).
enum class BoundDispatch : uint8
{
    /// The function itself: static and non-virtual methods, `base.` calls and every call on a struct.
    Direct,
    /// Through the receiver's virtual slots, by the function's Slot.
    Virtual,
    /// Through the receiver's map of the function's interface.
    Interface,
};

struct BoundExpr
{
    BoundKind Kind = BoundKind::Constant;
    const SemaType *Type = nullptr;
    SourceRange Range;
    /// Whether the expression names storage that can be assigned or passed by reference.
    bool IsLValue = false;

    template <typename T> [[nodiscard]] T *As()
    {
        MARLIN_ASSERT(Kind == T::StaticKind);
        return static_cast<T *>(this);
    }

    template <typename T> [[nodiscard]] const T *As() const
    {
        MARLIN_ASSERT(Kind == T::StaticKind);
        return static_cast<const T *>(this);
    }
};

template <BoundKind TKind> struct BoundExprOf : BoundExpr
{
    static constexpr BoundKind StaticKind = TKind;
};

struct BoundConstant : BoundExprOf<BoundKind::Constant>
{
    SemaConstant *Value = nullptr;
};

struct BoundLocal : BoundExprOf<BoundKind::Local>
{
    SemaLocal *Local = nullptr;
};

struct BoundGlobal : BoundExprOf<BoundKind::Global>
{
    SemaGlobal *Global = nullptr;
};

struct BoundField : BoundExprOf<BoundKind::Field>
{
    BoundExpr *Object = nullptr;
    uint32 FieldIndex = 0;
};

/// Components of a vector, `v.zyx`.
struct BoundSwizzle : BoundExprOf<BoundKind::Swizzle>
{
    BoundExpr *Object = nullptr;
    uint8 Components[4] = {};
    uint8 Count = 0;
};

/// An element of an array, a vector component or a matrix row by a runtime or constant index.
struct BoundIndex : BoundExprOf<BoundKind::Index>
{
    BoundExpr *Object = nullptr;
    BoundExpr *Index = nullptr;
    /// The index is known to be in range, so lowering does not clamp it (§4.3).
    bool InBounds = false;
};

/// `buffer[i]` of a structured or byte address buffer.
struct BoundBufferElement : BoundExprOf<BoundKind::BufferElement>
{
    BoundExpr *Buffer = nullptr;
    BoundExpr *Index = nullptr;
    /// For an element of a Span (§4.5): its length, which the index is clamped below.
    BoundExpr *Length = nullptr;
};

struct BoundCall : BoundExprOf<BoundKind::Call>
{
    SemaFunction *Function = nullptr;
    /// The arguments in parameter order, `this` first for an instance member.
    Span<BoundExpr *> Arguments;
    BoundDispatch Dispatch = BoundDispatch::Direct;
};

/// `Autodiff.Forward(f, …)` or `Autodiff.Backward(f, …)`: a derivative of *Function* in forward or reverse mode
/// (§9.12).
struct BoundDerivative : BoundExprOf<BoundKind::Derivative>
{
    SemaFunction *Function = nullptr;
    /// The object of an instance method, passed as its `this`; it is not differentiated.
    BoundExpr *Receiver = nullptr;
    bool Forward = true;
    /// The arguments in parameter order, then dResult for Backward; `ref` pairs are lvalues.
    Span<BoundExpr *> Arguments;
    /// The parameters given a DifferentialPair, and of those the ones passed by `ref` to receive a gradient.
    uint32 PairMask = 0;
    uint32 RefMask = 0;
    /// The forward-mode directions: 2 for DifferentialPair2, else 1.
    uint32 TangentCount = 1;
};

/// A call of a bodyless `[Intrinsic]` member, or a read of a `[SystemValue]` property (§9.10).
struct BoundIntrinsic : BoundExprOf<BoundKind::Intrinsic>
{
    /// The core library declaration, whose parameter modes say which arguments are passed by reference.
    SemaFunction *Function = nullptr;
    IntrinsicTarget Target;
    /// The arguments in parameter order, the object first for a member of a magic type. `ref` arguments are
    /// lvalues whose address the operation takes.
    Span<BoundExpr *> Arguments;
    /// Extra constant operands from the attribute, such as the property index of `RayQueryGet`.
    Span<int64> Literals;
    MirBuiltin SystemValue = MirBuiltin::None;
};

/// A vector, matrix, array or struct from its elements or fields in order.
struct BoundConstruct : BoundExprOf<BoundKind::Construct>
{
    Span<BoundExpr *> Elements;
};

struct BoundConvert : BoundExprOf<BoundKind::Convert>
{
    BoundExpr *Operand = nullptr;
};

struct BoundBitcast : BoundExprOf<BoundKind::Bitcast>
{
    BoundExpr *Operand = nullptr;
};

struct BoundUnary : BoundExprOf<BoundKind::Unary>
{
    MirOp Op = MirOp::Negate;
    BoundExpr *Operand = nullptr;
    bool Checked = false;
};

/// An arithmetic, bitwise or comparison operation on operands already converted to their common type; matrix
/// products have their MIR ops.
struct BoundBinary : BoundExprOf<BoundKind::Binary>
{
    MirOp Op = MirOp::Add;
    BoundExpr *Left = nullptr;
    BoundExpr *Right = nullptr;
    /// In a `checked` context: integer overflow faults instead of wrapping (MarlinScript §8.1).
    bool Checked = false;
};

/// `&&` or `||`, which evaluate the right side only when needed.
struct BoundShortCircuit : BoundExprOf<BoundKind::ShortCircuit>
{
    bool IsAnd = true;
    BoundExpr *Left = nullptr;
    BoundExpr *Right = nullptr;
};

/// `target = value`; a compound assignment has *Op* and evaluates the target once.
struct BoundAssign : BoundExprOf<BoundKind::Assign>
{
    BoundExpr *Target = nullptr;
    BoundExpr *Value = nullptr;
    bool IsCompound = false;
    MirOp Op = MirOp::Add;
    /// Converts the result of the compound operation back to the target type, or null.
    const SemaType *OperationType = nullptr;
    /// *Value* is the whole new value and reads the target through the very node *Target* points to (directly, or
    /// under conversions or a Let): `x ??= y`, and in scripts `s += t` of strings and `e += h` of delegates. Lowering
    /// evaluates the target's storage once and serves that read from it.
    bool Combined = false;
};

struct BoundIncDec : BoundExprOf<BoundKind::IncDec>
{
    BoundExpr *Target = nullptr;
    bool Increment = true;
    bool Postfix = false;
};

struct BoundConditional : BoundExprOf<BoundKind::Conditional>
{
    BoundExpr *Condition = nullptr;
    BoundExpr *WhenTrue = nullptr;
    BoundExpr *WhenFalse = nullptr;
};

/// The zero value of its type: `default`, missing fields of `new S { }`.
struct BoundZero : BoundExprOf<BoundKind::Zero>
{
};

/// Runs *Effects* then yields *Result*; used for `with` and object initializers that copy then set fields.
struct BoundSequence : BoundExprOf<BoundKind::Sequence>
{
    SemaLocal *Temporary = nullptr;
    BoundExpr *Initial = nullptr;
    Span<BoundExpr *> Effects;
};

// Script expressions (MarlinScript §3)

/// A field of an object on the GC heap: *Object* is a class reference, *Instance* the class declaring the field.
struct BoundObjectField : BoundExprOf<BoundKind::ObjectField>
{
    BoundExpr *Object = nullptr;
    SemaStructInstance *Instance = nullptr;
    uint32 FieldIndex = 0;
};

/// `new C(arguments)`: allocates an object of the class, then runs *Constructor* on it.
struct BoundNewObject : BoundExprOf<BoundKind::NewObject>
{
    SemaFunction *Constructor = nullptr;
    Span<BoundExpr *> Arguments;
};

/// `new T[n]` with *Length*, or `new T[] { … }` with *Elements* and a null *Length*.
struct BoundNewArray : BoundExprOf<BoundKind::NewArray>
{
    BoundExpr *Length = nullptr;
    Span<BoundExpr *> Elements;
};

/// An element of a script array; an index out of range faults (MarlinScript §8.1).
struct BoundArrayElement : BoundExprOf<BoundKind::ArrayElement>
{
    BoundExpr *Array = nullptr;
    BoundExpr *Index = nullptr;
};

/// A property or indexer, read with *Getter* and assigned with *Setter*.
struct BoundProperty : BoundExprOf<BoundKind::Property>
{
    SemaFunction *Getter = nullptr;
    SemaFunction *Setter = nullptr;
    /// The object of an instance property; null for a static one.
    BoundExpr *Receiver = nullptr;
    /// The indices of an indexer.
    Span<BoundExpr *> Arguments;
    BoundDispatch Dispatch = BoundDispatch::Direct;
};

/// The conversions of references, boxes and nullable types (MarlinScript §3.6).
enum class BoundTypeConversionKind : uint8
{
    /// A reference to a base class or an implemented interface; never checked.
    Upcast,
    /// An explicit cast of a reference to a derived type; faults if the object is not one.
    Downcast,
    /// `as`: the reference, or null if the object is not one.
    TryDowncast,
    /// A value copied into an object, to convert to `object` or an interface.
    Box,
    /// The value in a boxed object; faults if it holds another type.
    Unbox,
    /// A value type into `T?`.
    WrapNullable,
    /// `T?` to `T`; faults on null.
    RemoveNullable,
};

struct BoundTypeConversion : BoundExprOf<BoundKind::TypeConversion>
{
    BoundExpr *Operand = nullptr;
    BoundTypeConversionKind Conversion = BoundTypeConversionKind::Upcast;
    /// Written as a cast, `as` or `!`, rather than implied by the context.
    bool Explicit = false;
};

/// Whether *Operand*, a reference, is a non-null object of *Tested*, as in `x is T`.
struct BoundTypeTest : BoundExprOf<BoundKind::TypeTest>
{
    BoundExpr *Operand = nullptr;
    const SemaType *Tested = nullptr;
};

/// `typeof(T)`: the TypeInfo of *Described*.
struct BoundTypeOf : BoundExprOf<BoundKind::TypeOf>
{
    const SemaType *Described = nullptr;
};

/// A lambda or method group whose delegate type is not known yet; converting it to a delegate type binds it.
/// Never reaches lowering.
struct BoundUnbound : BoundExprOf<BoundKind::Unbound>
{
    const AstLambdaExpr *Lambda = nullptr;
    Span<SemaSymbol *> Functions;
    SemaStructInstance *Owner = nullptr;
    BoundExpr *Receiver = nullptr;
    bool ViaBase = false;
    Span<AstNode *> GenericArguments;
    bool HasGenericArguments = false;
};

/// A delegate that calls *Function*, on *Receiver* for an instance method (MarlinScript §3.7).
struct BoundMakeDelegate : BoundExprOf<BoundKind::MakeDelegate>
{
    SemaFunction *Function = nullptr;
    BoundExpr *Receiver = nullptr;
    BoundDispatch Dispatch = BoundDispatch::Direct;
};

/// A delegate that calls the lambda *Function* with the closure of the code that makes it.
struct BoundLambda : BoundExprOf<BoundKind::Lambda>
{
    SemaFunction *Function = nullptr;
};

/// Calls the functions of *Delegate* in order; the result is the last one's.
struct BoundInvokeDelegate : BoundExprOf<BoundKind::InvokeDelegate>
{
    BoundExpr *Delegate = nullptr;
    Span<BoundExpr *> Arguments;
    /// Raising an event, which does nothing when no handler is attached; calling a null delegate faults.
    bool IsEvent = false;
};

/// `a + b` or `a - b` of delegates: the calls of both, or *Left* without the last occurrence of *Right*.
struct BoundCombineDelegates : BoundExprOf<BoundKind::CombineDelegates>
{
    BoundExpr *Left = nullptr;
    BoundExpr *Right = nullptr;
    bool Remove = false;
};

/// `await task`: suspends the async function until *Operand* completes, then gives its result
/// (MarlinScript §4.2).
struct BoundAwait : BoundExprOf<BoundKind::Await>
{
    BoundExpr *Operand = nullptr;
};

/// Stores *Initial* in *Local*, then yields *Result*, which reads it; patterns test their input this way so it is
/// evaluated once.
struct BoundLet : BoundExprOf<BoundKind::Let>
{
    SemaLocal *Local = nullptr;
    BoundExpr *Initial = nullptr;
    BoundExpr *Result = nullptr;
};

enum class BoundStmtKind : uint8
{
    Block,
    Local,
    Expression,
    If,
    Loop,
    Switch,
    Break,
    Continue,
    Return,
    Discard,
    Yield,
};

struct BoundStmt
{
    BoundStmtKind Kind = BoundStmtKind::Block;
    SourceRange Range;

    template <typename T> [[nodiscard]] T *As()
    {
        MARLIN_ASSERT(Kind == T::StaticKind);
        return static_cast<T *>(this);
    }

    template <typename T> [[nodiscard]] const T *As() const
    {
        MARLIN_ASSERT(Kind == T::StaticKind);
        return static_cast<const T *>(this);
    }
};

template <BoundStmtKind TKind> struct BoundStmtOf : BoundStmt
{
    static constexpr BoundStmtKind StaticKind = TKind;
};

struct BoundBlock : BoundStmtOf<BoundStmtKind::Block>
{
    Span<BoundStmt *> Statements;
};

struct BoundLocalStmt : BoundStmtOf<BoundStmtKind::Local>
{
    SemaLocal *Local = nullptr;
    BoundExpr *Initializer = nullptr;
};

struct BoundExprStmt : BoundStmtOf<BoundStmtKind::Expression>
{
    BoundExpr *Expression = nullptr;
};

struct BoundIf : BoundStmtOf<BoundStmtKind::If>
{
    BoundExpr *Condition = nullptr;
    BoundStmt *Then = nullptr;
    BoundStmt *Else = nullptr;
};

/// `for`, `while`, `do` and `foreach`: *Initializers*, then while *Condition* (null for always) the body and
/// *Increments*. A `do` loop runs the body once before the first test.
struct BoundLoop : BoundStmtOf<BoundStmtKind::Loop>
{
    Span<BoundStmt *> Initializers;
    BoundExpr *Condition = nullptr;
    Span<BoundExpr *> Increments;
    BoundStmt *Body = nullptr;
    bool TestFirst = true;
    MirLoopControl Control = MirLoopControl::None;
    /// The iteration whose locals get fresh storage each time round the body; null in shaders.
    SemaIteration *Iteration = nullptr;
    /// For `[Unroll(n)]`; 0 unrolls fully.
    uint32 UnrollCount = 0;
    /// For `[MaxIterations(n)]`; 0 when not given.
    uint32 MaxIterations = 0;
};

struct BoundSwitchSection
{
    Span<int64> Values;
    /// For a switch over patterns: whether the section's labels match *Input*, tested in section order.
    BoundExpr *Match = nullptr;
    bool IsDefault = false;
    Span<BoundStmt *> Statements;
};

/// A switch over integer constants, or, when *Input* is set, over patterns: *Selector* is stored in *Input* and
/// the first section whose *Match* holds runs, `default` last (MarlinScript §3.9).
struct BoundSwitch : BoundStmtOf<BoundStmtKind::Switch>
{
    BoundExpr *Selector = nullptr;
    SemaLocal *Input = nullptr;
    Span<BoundSwitchSection> Sections;
};

struct BoundBreak : BoundStmtOf<BoundStmtKind::Break>
{
};

struct BoundContinue : BoundStmtOf<BoundStmtKind::Continue>
{
};

struct BoundReturn : BoundStmtOf<BoundStmtKind::Return>
{
    BoundExpr *Value = nullptr;
};

struct BoundDiscard : BoundStmtOf<BoundStmtKind::Discard>
{
};

/// `yield return value`, or `yield break` with a null *Value* (MarlinScript §4.3).
struct BoundYield : BoundStmtOf<BoundStmtKind::Yield>
{
    BoundExpr *Value = nullptr;
};

} // namespace Marlin
