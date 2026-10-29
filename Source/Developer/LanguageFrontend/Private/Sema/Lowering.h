#pragma once

#include "LanguageIR/MirBuilder.h"
#include "Memory/UniquePtr.h"
#include "Sema/Sema.h"

namespace Marlin
{
/// Lowers checked function instances to one MIR module: types, functions, globals and the entry point wrapper.
class Lowering
{
public:
    Lowering(Sema &sema, MirModule &module);

    const MirType *LowerType(const SemaType *type);
    MirFunction *LowerFunction(SemaFunction *function);
    /// Lowers *entry*, a bound entry point function of *stage*, and the functions it calls, and adds the entry
    /// point with its interface to the module.
    bool LowerEntryPoint(SemaFunction *entry, MirStage stage, StringView name, const MirEntryPoint &settings);

    // Script packages (LoweringScript.cpp, MarlinScript §5)
    /// The script type table entry of a class, interface, delegate, array or boxed value type, added on first use.
    uint32 ScriptTypeOf(const SemaType *type);
    /// Adds the function *name* that runs the initializers of *statics* in order as a static initializer.
    void LowerStaticInitializer(StringView name, Span<SemaGlobal *const> statics);
    /// The package being lowered: its functions are defined, those of other packages imported.
    void SetScriptPackage(uint32 package)
    {
        _package = package;
    }

private:
    struct LoopTargets
    {
        MirBlock *Break = nullptr;
        MirBlock *Continue = nullptr;
    };

    MirConstant *LowerConstant(const SemaConstant *constant, const SemaType *type);
    MirGlobal *LowerGlobal(SemaGlobal *global);
    MirGlobal *SystemValue(MirBuiltin builtin, const MirType *type, MirAddressSpace space, StringView name);

    void Statement(BoundStmt *statement);
    void Block(Span<BoundStmt *> statements);
    MirValue *Value(BoundExpr *expression);
    MirValue *LowerValue(BoundExpr *expression);
    /// The pointer to the storage *expression* names; for a value that is not stored, a temporary holding it.
    MirValue *Address(BoundExpr *expression);
    /// The index of *node*, clamped to the last element unless it is known to be in range or the function is
    /// `[Unchecked]` (§4.3).
    MirValue *IndexValue(BoundIndex *node);
    MirLocation LocationOf(SourceRange range);
    /// Tells debuggers that *variable* holds the source variable *name* when the options keep debug info (§11.3).
    void DeclareForDebugger(MirValue *variable, StringView name, const SemaType *type, SourceRange range);
    /// The output arrays of the mesh output *expression* indexes, one per field (§9.2); null for other expressions.
    const List<MirGlobal *> *MeshOutputOf(const BoundExpr *expression) const;
    /// The task payload variable of the entry point, created as a *type* on first use (§9.2).
    MirGlobal *TaskPayload(const MirType *type);
    /// Lowers `Debug.Print` and `Debug.Assert` (§9.11), or nothing when the options leave debug output off.
    void DebugOutput(BoundIntrinsic *intrinsic);
    /// With CheckSpans: reports an access to *element* whose *index* is not below *length* or whose *buffer* is null.
    void CheckSpanAccess(const BoundBufferElement *element, MirValue *buffer, MirValue *index, MirValue *length);
    /// Prints `file:line: ` of *range*, then *format* with *operands*, where *condition* is false (§9.11).
    void DebugCheck(MirValue *condition, SourceRange range, StringView format, Span<MirValue *const> operands);
    /// Whether stores to *target* are computed `[Precise]`: it is part of a local declared `[Precise]`.
    static bool IsPreciseTarget(const BoundExpr *target);
    MirValue *Spill(MirValue *value, StringView name = {});
    void Assign(BoundExpr *target, MirValue *value);
    MirValue *Call(BoundCall *call);
    /// Lowers *intrinsic*; one that makes a HitObject writes it to *destination*, or to a new variable, and returns
    /// the pointer (§9.8).
    /// `DifferentialAdd(first, second)`, or `DifferentialScale(first, second)` with *second* the float scale, of
    /// derivatives of *type* (§9.12).
    MirValue *DifferentialArithmetic(const SemaType *type, MirValue *first, MirValue *second, bool add);
    /// The scalars of *value* as 32-bit words, in field and component order, into *words*; false for a type with
    /// scalars that do not fit a word, or handles.
    bool AppendWords(MirValue *value, List<MirValue *> &words);
    /// A value of *type* built from *words* from *index* on, as AppendWords left them.
    MirValue *ValueFromWords(const MirType *type, Span<MirValue *const> words, usize &index);
    /// The words AppendWords gives a value of *type*; ~0u when it cannot.
    static uint32 CountWords(const MirType *type);
    MirValue *Intrinsic(BoundIntrinsic *intrinsic, MirValue *destination = nullptr);
    /// The variable of *space* that ray tracing passes values of *type* through: a payload, callable data, hit
    /// attributes or a shader record; one per space and type.
    /// Makes the HitObject *value* computes in *destination*; hit objects are never copied (§9.8).
    void HitObjectInto(BoundExpr *value, MirValue *destination);
    /// The element code of a cooperative-vector matrix or bias of *scalar* elements, the literals of MatMul and
    /// MatMulAdd (§9.9).
    static int64 ElementCode(const SemaType *scalar);
    MirGlobal *StageVariable(MirAddressSpace space, const MirType *type, StringView name);
    MirValue *ConvertValue(MirValue *value, const SemaType *from, const SemaType *to);
    MirValue *Arithmetic(MirOp op, MirValue *left, MirValue *right);
    MirValue *Conditional(BoundExpr *condition, BoundExpr *whenTrue, BoundExpr *whenFalse, const SemaType *type);
    MirValue *Splat(MirValue *scalar, const MirType *type);
    MirBlock *NewBlock(StringView name);
    void UseGlobal(MirGlobal *global);
    void SetLocation(SourceRange range);

    // Scripts (LoweringScript.cpp)

    /// The closure object of a function with lambdas: the locals they capture, `this`, and the closure of the
    /// function the lambda is written in; or of one run of a loop body, for the locals declared in it
    /// (MarlinScript §3.7).
    struct Environment
    {
        SemaFunction *Function = nullptr;
        uint32 ScriptType = 0;
        const MirType *Reference = nullptr;
        const MirType *Layout = nullptr;
        HashMap<const SemaLocal *, uint32> Fields;
        uint32 Parent = ~0u;
        const Environment *Outer = nullptr;
    };

    [[nodiscard]] bool InScript() const
    {
        return _sema->Options->Language == LanguageDialect::Script;
    }

    const MirType *LowerScriptType(const SemaType *type);
    /// The fields of a class, its bases' first.
    const MirType *ClassLayout(SemaStructInstance *instance);
    uint32 BaseFieldCount(const SemaStructInstance *instance);
    /// A unique name for a script function: its qualified name, generic arguments, accessor and parameter types.
    String LinkName(SemaFunction *function);
    /// Sets up the script parts of a function being lowered: its kind, `this` passed by reference, its closure.
    void BeginScriptFunction(SemaFunction *function, MirFunction *result);
    Environment *NewEnvironment(StringView name, bool hasOuter, const Environment *outer,
                                Span<const SemaLocal *const> locals);
    /// The closure of *function*, and of the loop iterations whose locals its lambdas capture; null without lambdas.
    Environment *PrepareEnvironment(SemaFunction *function);
    /// The closure that holds the locals of *function* declared in *iteration* or the iterations around it.
    const Environment *ScopeEnvironment(SemaFunction *function, const SemaIteration *iteration);
    /// The closure *lambda* receives: the one of the scope it is made in.
    const Environment *EnclosingEnvironment(SemaFunction *lambda);
    MirValue *ScopeValue(const SemaIteration *iteration);
    /// Makes the closure of a new run of the loop body *iteration*, at the top of each run.
    void BeginIteration(const SemaIteration *iteration);
    /// The storage of *local* when a lambda captured it: a field of its function's closure.
    MirValue *CapturedAddress(const SemaLocal *local);
    MirValue *ScriptValue(BoundExpr *expression);
    MirValue *ScriptAddress(BoundExpr *expression);
    MirValue *TypeConversion(BoundTypeConversion *node);
    MirValue *ReadProperty(BoundProperty *property, MirValue *receiver, Span<MirValue *const> indices);
    void WriteProperty(BoundProperty *property, MirValue *receiver, Span<MirValue *const> indices, MirValue *value);
    /// Evaluates the receiver and indices of *property* once, for a read followed by a write.
    void PropertyOperands(BoundProperty *property, MirValue *&receiver, List<MirValue *> &indices);
    MirValue *CallFunction(SemaFunction *function, BoundDispatch dispatch, Span<MirValue *const> arguments);
    void PatternSwitch(BoundSwitch *node);

    Sema *_sema;
    MirModule *_module;
    MirBuilder _builder;
    HashMap<const SemaType *, const MirType *> _types;
    HashMap<const SemaFunction *, MirFunction *> _functions;
    HashMap<const SemaGlobal *, MirGlobal *> _globals;
    HashMap<uint32, MirGlobal *> _systemValues;
    HashMap<uint32, uint32> _files;
    List<SemaFunction *> _pending;
    List<MirGlobal *> _interface;

    MirFunction *_current = nullptr;
    SemaFunction *_currentSema = nullptr;
    bool _unchecked = false;
    /// Debug output was lowered, so the entry point carries the debug output buffer's address (§9.11).
    bool _debugOutput = false;
    HashMap<const SemaLocal *, MirValue *> _locals;
    List<LoopTargets> _loops;
    MirStage _stage = MirStage::Compute;
    HashMap<const SemaLocal *, List<MirGlobal *>> _meshOutputs;
    MirGlobal *_taskPayload = nullptr;
    List<MirGlobal *> _stageVariables;
    /// The id the next specialization constant gets, in order of first use.
    uint32 _specializations = 0;
    HashMap<const SemaType *, uint32> _scriptTypes;
    HashMap<const SemaStructInstance *, const MirType *> _classLayouts;
    List<UniquePtr<Environment>> _environmentStorage;
    HashMap<const SemaFunction *, Environment *> _environments;
    HashMap<const SemaIteration *, Environment *> _iterationEnvironments;
    HashMap<const SemaIteration *, MirValue *> _iterationValues;
    /// The closure of the function being lowered, and the closure a lambda receives as its first argument.
    MirValue *_environment = nullptr;
    MirValue *_outerEnvironment = nullptr;
    uint32 _lambdaCount = 0;
    uint32 _package = 0;
    // RECOVERY-DECISION lang-lower-D1: what a combined assignment (`x ??= y`, `s += t`, `e += h`) read from its
    // target, served when lowering its value meets the target node again, so the target is evaluated once.
    HashMap<const BoundExpr *, MirValue *> _combined;
};
} // namespace Marlin
