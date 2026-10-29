#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "LanguageIR/MirType.h"
#include "Memory/Arena.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
class MirBlock;
class MirFunction;
class MirInstruction;
class MirModule;

template <typename T> using MirList = List<T, ArenaAllocator>;

enum class MirValueKind : uint8
{
    Constant,
    Argument,
    Instruction,
    Global,
    Function,
};

/// Where a value comes from in the source: a file of MirModule::GetFiles, 1-based line and column, or 0.
struct MirLocation
{
    uint32 File = 0;
    uint32 Line = 0;
    uint32 Column = 0;
};

/// A source variable debuggers show, declared by DebugDeclare.
struct MirDebugVariable
{
    StringView Name;
    const MirType *Type = nullptr;
    MirLocation Location;
};

struct MirUse
{
    MirInstruction *User = nullptr;
    uint32 Index = 0;
};

/// Anything an instruction can take as an operand.
class LANGUAGEIR_API MirValue
{
public:
    /// Changes the type of the value, keeping its uses; for passes that rewrite types the target lacks.
    void SetType(const MirType *type)
    {
        _type = type;
    }

    MirValue(MirValueKind kind, const MirType *type, Arena &arena)
        : _uses(ArenaAllocator{&arena}), _type(type), _kind(kind)
    {
    }

    MARLIN_NONCOPYABLE(MirValue);

    [[nodiscard]] MirValueKind GetValueKind() const
    {
        return _kind;
    }

    [[nodiscard]] const MirType *GetType() const
    {
        return _type;
    }

    [[nodiscard]] Span<const MirUse> GetUses() const
    {
        return _uses.AsSpan();
    }

    [[nodiscard]] bool HasUses() const
    {
        return !_uses.IsEmpty();
    }

    /// A name for the printer and debug info; may be empty.
    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    void SetName(StringView name)
    {
        _name = name;
    }

    /// Makes every use of this value use *other* instead.
    void ReplaceAllUsesWith(MirValue *other);

private:
    friend class MirInstruction;

    /// Adds the use of operand *index* of *user*; returns where it is in the use list.
    uint32 AddUse(MirInstruction *user, uint32 index);
    /// Removes the use at *position* of the use list, operand *index* of *user*, without searching the list, which
    /// for a shared constant can be long.
    void RemoveUse(uint32 position, MirInstruction *user, uint32 index);

    MirList<MirUse> _uses;
    const MirType *_type;
    StringView _name;
    MirValueKind _kind;
};

enum class MirConstantKind : uint8
{
    Scalar,
    Composite,
    Null,
    Undef,
};

/// A constant: a scalar as bits, a composite of constants, the zero value or an undefined value.
class LANGUAGEIR_API MirConstant final : public MirValue
{
public:
    MirConstant(const MirType *type, MirConstantKind kind, Arena &arena)
        : MirValue(MirValueKind::Constant, type, arena), _elements(ArenaAllocator{&arena}), _constantKind(kind)
    {
    }

    [[nodiscard]] MirConstantKind GetConstantKind() const
    {
        return _constantKind;
    }

    /// The bits of a scalar: a bool is 0 or 1, an integer is sign- or zero-extended, a float is its encoding.
    [[nodiscard]] uint64 GetBits() const
    {
        return _bits;
    }

    [[nodiscard]] int64 GetInt() const
    {
        return static_cast<int64>(_bits);
    }

    /// A float scalar as double, whatever its width.
    [[nodiscard]] double GetFloat() const;

    [[nodiscard]] Span<MirConstant *const> GetElements() const
    {
        return _elements.AsSpan();
    }

private:
    friend class MirModule;

    MirList<MirConstant *> _elements;
    uint64 _bits = 0;
    MirConstantKind _constantKind;
};

class LANGUAGEIR_API MirArgument final : public MirValue
{
public:
    MirArgument(const MirType *type, MirFunction *parent, uint32 index, Arena &arena)
        : MirValue(MirValueKind::Argument, type, arena), _parent(parent), _index(index)
    {
    }

    [[nodiscard]] MirFunction *GetParent() const
    {
        return _parent;
    }

    [[nodiscard]] uint32 GetIndex() const
    {
        return _index;
    }

private:
    MirFunction *_parent;
    uint32 _index;
};

/// A system value a stage reads or writes.
enum class MirBuiltin : uint8
{
    None,
    Position,
    VertexIndex,
    InstanceIndex,
    DispatchThreadId,
    GroupThreadId,
    GroupId,
    GroupIndex,
    Depth,
    Coverage,
    FrontFace,
    SampleIndex,
    Barycentrics,
    PrimitiveId,
    ViewIndex,
    ShadingRate,
    PrimitiveShadingRate,
    CullPrimitive,
    RenderTargetIndex,
    ViewportIndex,
    PrimitiveTriangleIndices,
    PrimitiveLineIndices,
    PrimitivePointIndices,
    LaunchIndex,
    LaunchSize,
    WorldRayOrigin,
    WorldRayDirection,
    ObjectRayOrigin,
    ObjectRayDirection,
    RayTMin,
    RayTCurrent,
    IncomingRayFlags,
    HitKind,
    InstanceIndexRt,
    InstanceId,
    GeometryIndex,
    PrimitiveIndexRt,
    ObjectToWorld,
    WorldToObject,
    WaveLaneIndex,
    WaveLaneCount,
    HelperLane,
};

enum class MirInterpolation : uint8
{
    Perspective,
    Linear,
    NoInterpolation,
};

enum class MirSampling : uint8
{
    Center,
    Centroid,
    Sample,
};

/// A module-scope variable; its value is a pointer into its address space.
class LANGUAGEIR_API MirGlobal final : public MirValue
{
public:
    MirGlobal(const MirType *pointerType, Arena &arena) : MirValue(MirValueKind::Global, pointerType, arena)
    {
    }

    [[nodiscard]] MirAddressSpace GetAddressSpace() const
    {
        return GetType()->GetAddressSpace();
    }

    [[nodiscard]] const MirType *GetValueType() const
    {
        return GetType()->GetElement();
    }

    MirConstant *Initializer = nullptr;
    MirBuiltin Builtin = MirBuiltin::None;
    /// The location of a user-defined Input or Output, the render target of a pixel output, or the id of a
    /// Specialization constant; ~0u when none.
    uint32 Location = ~0u;
    MirInterpolation Interpolation = MirInterpolation::Perspective;
    MirSampling Sampling = MirSampling::Center;
    /// A mesh shader output written per primitive rather than per vertex.
    bool PerPrimitive = false;
};

enum class MirOp : uint8
{
    Add,
    Sub,
    Mul,
    Div,
    Rem,
    Negate,
    BitAnd,
    BitOr,
    BitXor,
    BitNot,
    ShiftLeft,
    ShiftRight,
    LogicalAnd,
    LogicalOr,
    LogicalNot,
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    MatrixTimesVector,
    VectorTimesMatrix,
    MatrixTimesMatrix,
    MatrixTimesScalar,
    VectorTimesScalar,
    Convert,
    Bitcast,
    CompositeConstruct,
    CompositeExtract,
    CompositeInsert,
    VectorExtractDynamic,
    VectorInsertDynamic,
    VectorShuffle,
    Select,
    Variable,
    Load,
    Store,
    AccessChain,
    ArrayLength,
    BufferPointer,
    Call,
    Phi,
    Intrinsic,
    Branch,
    CondBranch,
    Switch,
    Return,
    Discard,
    Unreachable,
    IgnoreHit,
    AcceptHitAndEndSearch,
    EmitMeshTasks,
    /// Script operations (MarlinScript.md). Script types are named by their index in the module's script type
    /// table and objects are values of Reference types; operands and literals are laid out as MirBuilder creates
    /// them.
    New,
    /// A new array of the array type literal 0 with operand 0 elements.
    NewArray,
    /// A Managed pointer to field literal 0 of object operand 0, by the layout of the class literal 1.
    FieldAddress,
    /// A Managed pointer to element operand 1 of array operand 0.
    ElementAddress,
    /// The length of object operand 0.
    ObjectLength,
    /// A new object of the type literal 0 holding operand 0.
    Box,
    /// The value of the type literal 0 that object operand 0 holds.
    Unbox,
    /// Whether object operand 0 is an instance of the type literal 0.
    IsInstance,
    /// Object operand 0 as the type literal 0; with literal 1 (orNull) set, null when it is not one.
    Cast,
    /// The type information of the type literal 0.
    TypeOf,
    /// Operand 0, checked not to be null.
    NullCheck,
    /// Calls virtual slot literal 0 on the receiver, operand 1; operand 0 is the declared function and the operands
    /// after it are its arguments.
    CallVirtual,
    /// Calls method literal 1 of the interface literal 0 on the receiver, operand 1; operand 0 is the declared
    /// function and the operands after it are its arguments.
    CallInterface,
    /// Calls delegate operand 0 with the other operands; literal 0 (allowEmpty) makes a null delegate call nothing,
    /// as an event's does.
    CallDelegate,
    /// A delegate of the type literal 0 calling function operand 0, with receiver operand 1 when there is one;
    /// literal 1 is the slot.
    MakeDelegate,
    /// Delegates operand 0 and operand 1 combined, or with literal 0 (remove) set, operand 1 removed from operand 0;
    /// null is the empty delegate.
    CombineDelegates,
    /// String literal 0 of the module's string table.
    StringConstant,
    /// Awaits task operand 0 in an async function and gives its result.
    Await,
    /// Yields operand 0 in an iterator function.
    Yield,
    /// Raises fault literal 0, a MirFault, with message literal 1, or -1 for none; a terminator.
    Fault,
};

/// What a Fault raises.
enum class MirFault : uint8
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

/// Built-in operations that are not plain arithmetic; each backend maps them to its own instructions.
enum class MirIntrinsic : uint8
{
    None,
    Abs,
    Sign,
    Floor,
    Ceil,
    Round,
    Trunc,
    Fract,
    Sqrt,
    InverseSqrt,
    Exp,
    Exp2,
    Log,
    Log2,
    Pow,
    Sin,
    Cos,
    Tan,
    Asin,
    Acos,
    Atan,
    Atan2,
    Sinh,
    Cosh,
    Tanh,
    Min,
    Max,
    Clamp,
    Saturate,
    Lerp,
    Step,
    SmoothStep,
    Fma,
    Ldexp,
    Dot,
    Cross,
    Length,
    Distance,
    Normalize,
    Reflect,
    Refract,
    FaceForward,
    Determinant,
    Inverse,
    Transpose,
    IsNan,
    IsInf,
    Any,
    All,
    CountBits,
    FirstBitHigh,
    FirstBitLow,
    ReverseBits,
    PackHalf2x16,
    UnpackHalf2x16,
    Ddx,
    Ddy,
    DdxFine,
    DdyFine,
    DdxCoarse,
    DdyCoarse,
    Fwidth,
    Sample,
    SampleBias,
    SampleLevel,
    SampleGrad,
    SampleCmp,
    SampleCmpLevel,
    Gather,
    GatherCmp,
    TextureLoad,
    TextureStore,
    TextureDimensions,
    TextureLevels,
    TextureSamples,
    CalculateLod,
    AtomicLoad,
    AtomicStore,
    AtomicExchange,
    AtomicCompareExchange,
    AtomicAdd,
    AtomicMin,
    AtomicMax,
    AtomicAnd,
    AtomicOr,
    AtomicXor,
    GroupBarrier,
    GroupMemoryBarrier,
    DeviceMemoryBarrier,
    AllMemoryBarrier,
    WaveIsFirstLane,
    WaveActiveAllTrue,
    WaveActiveAnyTrue,
    WaveActiveAllEqual,
    WaveActiveBallot,
    WaveReadLaneAt,
    WaveReadFirstLane,
    WaveActiveSum,
    WaveActiveProduct,
    WaveActiveMin,
    WaveActiveMax,
    WaveActiveBitAnd,
    WaveActiveBitOr,
    WaveActiveBitXor,
    WaveActiveCountBits,
    WavePrefixSum,
    WavePrefixProduct,
    WavePrefixCountBits,
    SetMeshOutputCounts,
    TraceRay,
    ReportHit,
    CallShader,
    RayQueryInitialize,
    RayQueryProceed,
    RayQueryAbort,
    RayQueryConfirmIntersection,
    RayQueryGenerateIntersection,
    RayQueryGet,
    HitObjectTraceRay,
    HitObjectFromRayQuery,
    HitObjectMakeMiss,
    HitObjectMakeNop,
    HitObjectInvoke,
    HitObjectGet,
    ReorderThread,
    CoopVectorLoad,
    CoopVectorStore,
    CoopVectorMatMul,
    CoopVectorMatMulAdd,
    CoopVectorOuterProductAccumulate,
    CoopVectorReduceSumAccumulate,
    CoopMatrixLoad,
    CoopMatrixStore,
    CoopMatrixMulAdd,
    CoopMatrixLength,
    /// `Debug.Print` (§9.11): literal 0 is the index of the format in the module's string table, the operands are
    /// the arguments it references.
    DebugPrint,
    /// Declares source variable literal 0 (MirModule::GetDebugVariables), whose value is now operand 0.
    DebugDeclare,
    /// `Autodiff.Forward` (§9.12): operand 0 is the function, then its arguments; literal 0 marks the parameters
    /// given a DifferentialPair. The result is the pair of the result; MirAutodiffPass replaces it with a call.
    ForwardDerivative,
    /// `Autodiff.Backward`: like ForwardDerivative, then dResult; literal 1 marks the `ref` pairs, passed by pointer.
    BackwardDerivative,
    /// `Autodiff.Detach(x)`: operand 0 with a zero derivative. MirAutodiffPass replaces it with its operand.
    Detach,
    /// `Compiler.Require(condition, message)`: operand 0 must be the constant true once the code is optimized;
    /// literal 0 is the message's index in the string table. MirRequirePass checks and removes it.
    Require,
};

/// An operation. Operands are values, *targets* the blocks a terminator or phi refers to, and *literals* the
/// constant indices, shuffle components, switch cases and intrinsic-specific selectors.
class LANGUAGEIR_API MirInstruction final : public MirValue
{
public:
    MirInstruction(MirOp op, const MirType *type, Arena &arena)
        : MirValue(MirValueKind::Instruction, type, arena), _operands(ArenaAllocator{&arena}),
          _usePositions(ArenaAllocator{&arena}), _targets(ArenaAllocator{&arena}), _literals(ArenaAllocator{&arena}),
          _op(op)
    {
    }

    [[nodiscard]] MirOp GetOp() const
    {
        return _op;
    }

    [[nodiscard]] bool IsTerminator() const;

    [[nodiscard]] Span<MirValue *const> GetOperands() const
    {
        return _operands.AsSpan();
    }

    [[nodiscard]] MirValue *GetOperand(usize index) const
    {
        return _operands[index];
    }

    void AddOperand(MirValue *value);
    void SetOperand(usize index, MirValue *value);
    /// Removes operand *index*, and for a phi its incoming block too.
    void RemoveOperand(usize index);
    /// Removes every operand, dropping their uses; done before an instruction is erased.
    void DropOperands();

    [[nodiscard]] Span<MirBlock *const> GetTargets() const
    {
        return _targets.AsSpan();
    }

    void AddTarget(MirBlock *block)
    {
        _targets.Add(block);
    }

    void SetTarget(usize index, MirBlock *block)
    {
        _targets[index] = block;
    }

    void RemoveTarget(usize index)
    {
        _targets.RemoveAt(index);
    }

    [[nodiscard]] Span<const int64> GetLiterals() const
    {
        return _literals.AsSpan();
    }

    void AddLiteral(int64 literal)
    {
        _literals.Add(literal);
    }

    [[nodiscard]] MirIntrinsic GetIntrinsic() const
    {
        return _intrinsic;
    }

    void SetIntrinsic(MirIntrinsic intrinsic)
    {
        _intrinsic = intrinsic;
    }

    [[nodiscard]] MirBlock *GetParent() const
    {
        return _parent;
    }

    [[nodiscard]] MirInstruction *GetNext() const
    {
        return _next;
    }

    [[nodiscard]] MirInstruction *GetPrevious() const
    {
        return _previous;
    }

    /// Unlinks this instruction from its block and drops its operands; it must have no uses left.
    void Erase();

    MirLocation Location;
    /// Set on handles and resource accesses whose operands may differ between the lanes of a wave.
    bool NonUniform = false;
    /// A BufferPointer whose memory no other pointer reaches during the dispatch: `[NoAlias]` (MarlinLanguage §4.5).
    bool Restrict = false;
    /// Forbids reassociation and contraction of this floating-point operation.
    bool Precise = false;
    /// Integer arithmetic in a script `checked` context.
    bool Checked = false;

private:
    friend class MirBlock;
    friend class MirValue;

    /// Adds operand *index*, *value*, to its use list.
    void Use(usize index);
    /// Takes operand *index* off its use list.
    void Unuse(usize index);

    MirList<MirValue *> _operands;
    /// Where each operand's use is in the operand's use list.
    MirList<uint32> _usePositions;
    MirList<MirBlock *> _targets;
    MirList<int64> _literals;
    MirBlock *_parent = nullptr;
    MirInstruction *_previous = nullptr;
    MirInstruction *_next = nullptr;
    MirOp _op;
    MirIntrinsic _intrinsic = MirIntrinsic::None;
};

enum class MirMergeKind : uint8
{
    None,
    Selection,
    Loop,
};

/// What the author asked of a loop's unrolling: `[Unroll]` or `[Unroll(n)]` (§11.2).
enum class MirLoopControl : uint8
{
    None,
    Unroll,
};

/// The structured control flow a header block starts: a selection with its merge block, or a loop with its merge
/// and continue blocks.
struct MirMerge
{
    MirMergeKind Kind = MirMergeKind::None;
    MirBlock *MergeBlock = nullptr;
    MirBlock *ContinueBlock = nullptr;
    MirLoopControl Control = MirLoopControl::None;
    /// For `[Unroll(n)]`: unroll by *n*; 0 unrolls fully.
    uint32 UnrollCount = 0;
    /// For `[MaxIterations(n)]`: the body runs at most *n* times, which bounds what reverse-mode differentiation
    /// stores per iteration (§9.12); 0 when not given.
    uint32 MaxIterations = 0;
};

/// A basic block: a list of instructions that ends with exactly one terminator.
class LANGUAGEIR_API MirBlock
{
public:
    explicit MirBlock(MirFunction *parent) : _parent(parent)
    {
    }

    MARLIN_NONCOPYABLE(MirBlock);

    [[nodiscard]] MirFunction *GetParent() const
    {
        return _parent;
    }

    [[nodiscard]] MirInstruction *GetFirst() const
    {
        return _first;
    }

    [[nodiscard]] MirInstruction *GetLast() const
    {
        return _last;
    }

    /// The last instruction if it is a terminator.
    [[nodiscard]] MirInstruction *GetTerminator() const;

    [[nodiscard]] bool IsEmpty() const
    {
        return _first == nullptr;
    }

    void Append(MirInstruction *instruction);
    /// Inserts *instruction* before *position*, or at the end when *position* is null.
    void InsertBefore(MirInstruction *instruction, MirInstruction *position);
    /// Unlinks *instruction* without touching its operands.
    void Unlink(MirInstruction *instruction);

    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    void SetName(StringView name)
    {
        _name = name;
    }

    MirMerge Merge;

    class Iterator
    {
    public:
        explicit Iterator(MirInstruction *instruction) : _instruction(instruction)
        {
        }

        MirInstruction *operator*() const
        {
            return _instruction;
        }

        Iterator &operator++()
        {
            _instruction = _instruction->GetNext();
            return *this;
        }

        bool operator==(const Iterator &other) const = default;

    private:
        MirInstruction *_instruction;
    };

    [[nodiscard]] Iterator begin() const
    {
        return Iterator(_first);
    }

    [[nodiscard]] Iterator end() const
    {
        return Iterator(nullptr);
    }

private:
    MirFunction *_parent;
    MirInstruction *_first = nullptr;
    MirInstruction *_last = nullptr;
    StringView _name;
};

enum class MirStage : uint8
{
    Compute,
    Amplification,
    Mesh,
    Vertex,
    Pixel,
    RayGeneration,
    ClosestHit,
    AnyHit,
    Miss,
    Intersection,
    Callable,
};

enum class MirMeshTopology : uint8
{
    Point,
    Line,
    Triangle,
};

/// How a script function runs.
enum class MirFunctionKind : uint8
{
    Normal,
    /// Called, it starts a task on its own stack and returns the Task; `Return` gives the task's result.
    Async,
    /// Called, it returns an enumerator over the values its `Yield`s give.
    Iterator,
};

/// A function; the first block is its entry.
class LANGUAGEIR_API MirFunction final : public MirValue
{
public:
    MirFunction(const MirType *functionType, MirModule &module, Arena &arena);

    [[nodiscard]] MirModule &GetModule() const
    {
        return *_module;
    }

    [[nodiscard]] const MirType *GetReturnType() const
    {
        return GetType()->GetElement();
    }

    [[nodiscard]] Span<MirArgument *const> GetArguments() const
    {
        return _arguments.AsSpan();
    }

    [[nodiscard]] Span<MirBlock *const> GetBlocks() const
    {
        return _blocks.AsSpan();
    }

    [[nodiscard]] MirBlock *GetEntry() const
    {
        return _blocks.IsEmpty() ? nullptr : _blocks[0];
    }

    MirBlock *AddBlock(StringView name = {});
    /// Removes *block*, which nothing may branch to; its instructions are dropped.
    void RemoveBlock(MirBlock *block);
    /// Moves *block* to *index* in the block order.
    void MoveBlock(MirBlock *block, usize index);

    MirLocation Location;
    /// Inlined into every caller by the inline pass.
    bool AlwaysInline = true;
    MirFunctionKind Kind = MirFunctionKind::Normal;
    /// What `Return` gives in an async function; null otherwise.
    const MirType *ResultType = nullptr;
    /// A function the VM implements, called by this name.
    StringView NativeName;
    /// Compiled from a `.mcommon` file.
    bool Common = false;
    /// Defined by another script module.
    bool Imported = false;
    /// A test function.
    bool Test = false;
    /// Bit i is set when argument i is a script `in` parameter: a pointer the function only reads through.
    uint64 ReadOnlyArguments = 0;
    /// The `[ForwardDerivative]` and `[BackwardDerivative]` of the function (§9.12), which differentiation calls
    /// instead of differentiating it. The inline pass keeps the function, and them, until MirAutodiffPass has run and
    /// cleared these.
    MirFunction *ForwardDerivative = nullptr;
    /// The `[ForwardDerivative2]`: the forward derivative along two directions at once, taking a DifferentialPair2
    /// {Primal, Dx, Dy} for each differentiable argument; what two-direction forward mode calls (§9.12).
    MirFunction *ForwardDerivative2 = nullptr;
    MirFunction *BackwardDerivative = nullptr;
    /// Bit i is set when argument i is differentiable, so the custom derivatives take a DifferentialPair for it.
    uint32 DerivativeArguments = 0;

private:
    MirModule *_module;
    MirList<MirArgument *> _arguments;
    MirList<MirBlock *> _blocks;
};

/// A function a stage starts at, with the module-scope variables it reads and writes.
struct MirEntryPoint
{
    explicit MirEntryPoint(Arena &arena) : Features(ArenaAllocator{&arena}), Interface(ArenaAllocator{&arena})
    {
    }

    StringView Name;
    MirStage Stage = MirStage::Compute;
    MirFunction *Function = nullptr;
    /// The optional hardware features (§9.7) the entry point uses.
    MirList<StringView> Features;
    /// Threads of a compute, amplification or mesh group.
    uint32 GroupSize[3] = {1, 1, 1};
    uint32 MaxVertices = 0;
    uint32 MaxPrimitives = 0;
    MirMeshTopology Topology = MirMeshTopology::Triangle;
    /// The [Uniform] parameter block is larger than the arguments passed directly, so they hold a Ptr to it in upload
    /// memory instead (MarlinLanguage §9.3): the Uniform global is that Ptr.
    bool ArgumentsIndirect = false;
    /// The entry point prints debug output (§9.11): its Uniform global is a struct of the arguments (or their Ptr),
    /// if any, and then the address of the debug output buffer, its last field.
    bool DebugBuffer = false;
    /// Every global the entry point uses, as SPIR-V's interface list needs.
    MirList<MirGlobal *> Interface;
};

enum class MirScriptTypeKind : uint8
{
    Class,
    Interface,
    Delegate,
    Array,
    String,
    /// A boxed value type.
    Value,
};

/// The methods a class implements an interface with.
struct MirInterfaceImpl
{
    explicit MirInterfaceImpl(Arena &arena) : Methods(ArenaAllocator{&arena})
    {
    }

    uint32 Interface = ~0u;
    MirList<MirFunction *> Methods;
};

/// An entry of a script module's type table, which objects, casts and virtual calls name by index (MarlinScript §5.1).
struct MirScriptType
{
    explicit MirScriptType(Arena &arena)
        : VirtualSlots(ArenaAllocator{&arena}), Interfaces(ArenaAllocator{&arena}), Implements(ArenaAllocator{&arena})
    {
    }

    MirScriptTypeKind Kind = MirScriptTypeKind::Class;
    /// The fully qualified name, with generic arguments.
    StringView Name;
    /// A class: its fields, its base class's first; an array: its element; a value: the type; a delegate: its
    /// function type; null for strings and interfaces.
    const MirType *Layout = nullptr;
    /// The Reference type values of this type have.
    const MirType *Reference = nullptr;
    /// A class's base class.
    uint32 Base = ~0u;
    bool Abstract = false;
    bool Sealed = false;
    /// Declared by another script module.
    bool Imported = false;
    /// A class's virtual slots, inherited ones first; an interface's methods.
    MirList<MirFunction *> VirtualSlots;
    /// A class's implementations of each interface it has, directly or through a base.
    MirList<MirInterfaceImpl> Interfaces;
    /// The interfaces an interface inherits.
    MirList<uint32> Implements;
};

/// A field of a MirAttribute and its value as text: a string as is, a bool as `true` or `false`, a number in decimal.
struct MirAttributeArgument
{
    StringView Name;
    StringView Value;
};

/// A user-declared attribute put on a script struct or function, kept for the engine to read from the module, such as
/// `[Component]` and `[System(Phase = "GameplayPhase")]` (MarlinScript §11.2).
struct MirAttribute
{
    explicit MirAttribute(Arena &arena) : Arguments(ArenaAllocator{&arena})
    {
    }

    /// The qualified name of the attribute struct.
    StringView Name;
    /// What it is put on: a struct type or a function.
    const MirType *Type = nullptr;
    MirFunction *Function = nullptr;
    MirList<MirAttributeArgument> Arguments;
};

/// A compilation result: types, constants, globals, functions and entry points, all owned by the module's arena.
class LANGUAGEIR_API MirModule
{
public:
    MirModule();
    ~MirModule();
    MARLIN_NONCOPYABLE(MirModule);

    [[nodiscard]] Arena &GetArena()
    {
        return _arena;
    }

    /// The name of the module (§1.2).
    [[nodiscard]] StringView GetName() const
    {
        return _name;
    }

    void SetName(StringView name)
    {
        _name = Intern(name);
    }

    [[nodiscard]] MirTypeContext &GetTypes()
    {
        return _types;
    }

    [[nodiscard]] MirConstant *GetBool(bool value);
    /// An integer or bool constant of a scalar *type*.
    [[nodiscard]] MirConstant *GetInt(const MirType *type, int64 value);
    /// A float constant of a scalar *type*, rounded to its width.
    [[nodiscard]] MirConstant *GetFloat(const MirType *type, double value);
    [[nodiscard]] MirConstant *GetScalarBits(const MirType *type, uint64 bits);
    /// The composite of *type* with *elements*; equal composites are one constant, like scalars.
    [[nodiscard]] MirConstant *GetComposite(const MirType *type, Span<MirConstant *const> elements);
    /// A composite whose elements are all *element*, such as a splatted vector.
    [[nodiscard]] MirConstant *GetSplat(const MirType *type, MirConstant *element);
    [[nodiscard]] MirConstant *GetNull(const MirType *type);
    [[nodiscard]] MirConstant *GetUndef(const MirType *type);

    MirGlobal *AddGlobal(const MirType *valueType, MirAddressSpace space, StringView name);
    MirFunction *AddFunction(const MirType *functionType, StringView name);
    MirEntryPoint &AddEntryPoint(StringView name, MirStage stage, MirFunction *function);

    [[nodiscard]] Span<MirGlobal *const> GetGlobals() const
    {
        return _globals.AsSpan();
    }

    [[nodiscard]] Span<MirFunction *const> GetFunctions() const
    {
        return _functions.AsSpan();
    }

    [[nodiscard]] Span<MirEntryPoint *const> GetEntryPoints() const
    {
        return _entryPoints.AsSpan();
    }

    void RemoveFunction(MirFunction *function);
    void RemoveGlobal(MirGlobal *global);

    /// Adds a source file for MirLocation::File and returns its index, starting at 1; *text* is kept for debug info.
    uint32 AddFile(StringView path, StringView text = {});

    [[nodiscard]] Span<const StringView> GetFiles() const
    {
        return _files.AsSpan();
    }

    /// The text of each file, empty when it was not kept.
    [[nodiscard]] Span<const StringView> GetFileTexts() const
    {
        return _fileTexts.AsSpan();
    }

    uint32 AddDebugVariable(StringView name, const MirType *type, MirLocation location);

    /// Records a problem a pass found in the source program; the pass manager reports it after the pass.
    void ReportError(String message)
    {
        _errors.Add(Move(message));
    }

    [[nodiscard]] List<String> TakeErrors()
    {
        return Move(_errors);
    }

    [[nodiscard]] Span<const MirDebugVariable> GetDebugVariables() const
    {
        return _debugVariables.AsSpan();
    }

    /// Copies *text* into the module's arena.
    [[nodiscard]] StringView Intern(StringView text);

    /// Adds *text* to the string table, the formats of `DebugPrint`, and returns its index; equal texts share one.
    uint32 AddString(StringView text);

    [[nodiscard]] Span<const StringView> GetStrings() const
    {
        return _strings.AsSpan();
    }

    /// Adds an entry to the script type table and returns its index.
    uint32 AddScriptType(MirScriptType *type);
    [[nodiscard]] MirScriptType *NewScriptType()
    {
        return _arena.New<MirScriptType>(_arena);
    }

    [[nodiscard]] Span<MirScriptType *const> GetScriptTypes() const
    {
        return _scriptTypes.AsSpan();
    }

    /// The functions that run a script module's static field initializers when it loads, in order (see
    /// MarlinScript.md).
    void AddStaticInitializer(MirFunction *function)
    {
        _staticInitializers.Add(function);
    }

    [[nodiscard]] Span<MirFunction *const> GetStaticInitializers() const
    {
        return _staticInitializers.AsSpan();
    }

    /// Records attribute *name* on a script struct or function; removing the function drops it.
    MirAttribute &AddAttribute(StringView name);

    [[nodiscard]] Span<MirAttribute *const> GetAttributes() const
    {
        return _attributes.AsSpan();
    }

private:
    struct ScalarKey
    {
        const MirType *Type;
        uint64 Bits;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const ScalarKey &) const = default;
    };

    struct CompositeKey
    {
        const MirType *Type;
        Span<MirConstant *const> Elements;

        [[nodiscard]] uint64 GetHash() const;
        bool operator==(const CompositeKey &other) const;
    };

    Arena _arena;
    MirTypeContext _types;
    StringView _name;
    HashMap<ScalarKey, MirConstant *> _scalars;
    /// Keyed by the elements of the constant itself.
    HashMap<CompositeKey, MirConstant *> _composites;
    HashMap<const MirType *, MirConstant *> _nulls;
    HashMap<const MirType *, MirConstant *> _undefs;
    List<MirGlobal *> _globals;
    List<MirFunction *> _functions;
    List<MirEntryPoint *> _entryPoints;
    List<StringView> _files;
    List<StringView> _fileTexts;
    List<StringView> _strings;
    List<MirScriptType *> _scriptTypes;
    List<MirFunction *> _staticInitializers;
    List<MirAttribute *> _attributes;
    List<MirDebugVariable> _debugVariables;
    List<String> _errors;
};
} // namespace Marlin
