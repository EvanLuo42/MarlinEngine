#pragma once

#include "Sema/Sema.h"

namespace Marlin
{
/// What a name or member access denotes before it is used: a value, a type, a namespace or a group of functions.
struct BinderTarget
{
    enum class Kind : uint8
    {
        Error,
        Value,
        Type,
        Namespace,
        Functions,
    };

    Kind TargetKind = Kind::Error;
    SourceRange Range;
    BoundExpr *Value = nullptr;
    const SemaType *Type = nullptr;
    SemaSymbol *Namespace = nullptr;
    /// The overloads of a function group, and the object of an instance call or the struct of a static one.
    List<SemaSymbol *> Functions;
    BoundExpr *Receiver = nullptr;
    SemaStructInstance *Owner = nullptr;
    Span<AstNode *> GenericArguments;
    bool HasGenericArguments = false;
    /// The type parameter a `T.Implements(…)` is asked of.
    const SemaType *TypeParameterTarget = nullptr;
    /// The object an extension method is called on, passed as its `this` parameter (§5.3).
    BoundExpr *ExtensionReceiver = nullptr;
    /// The `extension` declarations (§3.5) whose members are among *Functions*; each is called on its own instance.
    List<SemaStructInstance *> Extensions;
    /// Reached through `base.`: calls go to the base class's method without dynamic dispatch.
    bool ViaBase = false;
};

/// A call argument after binding, with what overload resolution needs to know about it.
struct BinderArgument
{
    BoundExpr *Value = nullptr;
    StringView Name;
    SemaParameterMode Mode = SemaParameterMode::Value;
    SourceRange Range;
    /// Its constant value, which may narrow implicitly (§2.4) and is inferred from last.
    SemaConstant *Constant = nullptr;
};

/// Binds the statements and expressions of one function instance, or a lone constant expression when there is
/// no function.
class Binder
{
public:
    Binder(Sema &sema, SemaFunction *function, const SemaContext &context);

    BoundStmt *BindFunctionBody(const AstBody &body);
    /// The body of an auto-property's accessor: reads or assigns its hidden field.
    BoundStmt *BindAutoAccessor();
    BoundExpr *BindExpression(AstNode *node, const SemaType *expected = nullptr);
    /// Converts *expression* to *type* implicitly, reporting at its range when it cannot.
    BoundExpr *ConvertImplicit(BoundExpr *expression, const SemaType *type);

private:
    // Expressions (BinderExpressions.cpp)

    BinderTarget ResolveTarget(AstNode *node);
    BinderTarget ResolveName(const AstNameExpr *name);
    BinderTarget ResolveMember(const AstMemberExpr *member);
    BinderTarget ResolveStaticMember(const SemaType *type, const AstMemberExpr *member);
    BinderTarget ResolveInstanceMember(BoundExpr *object, const AstMemberExpr *member);
    /// The extension methods named like *member* in scope, to call on *object*; an Error target if none.
    BinderTarget FindExtensions(BoundExpr *object, const AstMemberExpr *member);
    /// The members named like *member* that `extension` declarations add to *type* (§3.5), used on *receiver* or
    /// statically when it is null; *found* is false if there are none.
    BinderTarget ResolveExtensionMember(const SemaType *type, BoundExpr *receiver, const AstMemberExpr *member,
                                        bool &found);
    BinderTarget FromSymbols(Span<SemaSymbol *const> symbols, SemaStructInstance *owner, BoundExpr *receiver,
                             SourceRange range, StringView name);
    BoundExpr *TargetToValue(BinderTarget &target);
    BoundExpr *BindLiteral(const AstLiteralExpr *literal, const SemaType *expected);
    BoundExpr *BindUnary(const AstUnaryExpr *unary);
    BoundExpr *BindBinary(const AstBinaryExpr *binary);
    BoundExpr *BindBinaryOperands(TokenKind op, BoundExpr *left, BoundExpr *right, SourceRange range);
    BoundExpr *BindAssign(const AstAssignExpr *assign);
    BoundExpr *BindConditional(const AstConditionalExpr *conditional, const SemaType *expected);
    BoundExpr *BindCast(const AstCastExpr *cast);
    BoundExpr *BindIndex(const AstIndexExpr *index);
    BoundExpr *BindNew(const AstNewExpr *node);
    BoundExpr *BindWith(const AstWithExpr *with);
    BoundExpr *BindDefault(const AstDefaultExpr *node, const SemaType *expected);
    BoundExpr *BindSizeof(const AstSizeofExpr *node);
    BoundExpr *BindNameof(const AstNameofExpr *node);
    BoundExpr *Convert(BoundExpr *expression, const SemaType *type, bool isExplicit);
    BoundExpr *ApplyInitializers(BoundExpr *initial, Span<AstMemberInitializer> initializers, SourceRange range);
    BoundExpr *Fold(BoundExpr *expression);
    BoundExpr *Error(SourceRange range);
    /// Reports a construct of *node* that exists only in shaders when binding a script.
    BoundExpr *NotInScript(const AstNode *node);
    bool RequireLValue(BoundExpr *expression, StringView what);
    /// The scalar type both operands convert to for arithmetic, or null.
    const SemaType *CommonScalar(BoundExpr *left, BoundExpr *right);

    // Calls (BinderCalls.cpp)

    BoundExpr *BindCall(const AstCallExpr *call);
    BoundExpr *BindConstruction(const SemaType *type, Span<AstArgument> arguments, SourceRange range);
    BoundExpr *BindVectorConstruction(const SemaType *type, Span<BinderArgument> arguments, SourceRange range);
    bool BindArguments(Span<AstArgument> arguments, List<BinderArgument> &bound);
    /// Picks the overload of *target* that fits *arguments* (§3.2) and binds the call.
    BoundExpr *ResolveCall(BinderTarget &target, Span<BinderArgument> arguments, SourceRange range);
    BoundExpr *MakeCall(SemaFunction *function, BoundExpr *receiver, Span<BoundExpr *> arguments, SourceRange range);
    BoundExpr *CallGetter(SemaSymbol *property, SemaStructInstance *owner, BoundExpr *receiver, SourceRange range);
    BoundExpr *EvaluateCompileTime(SemaFunction *function, Span<BoundExpr *> arguments, SourceRange range,
                                   const SemaType *typeParameter);
    BoundExpr *BindUserOperator(TokenKind op, Span<BoundExpr *> operands, SourceRange range, bool &found);
    void NoteCallee(SemaFunction *callee, SourceRange range);
    /// The struct instance whose members a value of a magic *type* has, such as `Texture2D<half4>`.
    SemaStructInstance *MagicOwner(const SemaType *type);

    // Script types (BinderScript.cpp)

    [[nodiscard]] bool InScript() const
    {
        return _context.Dialect == LanguageDialect::Script;
    }

    BoundExpr *BindThis(SourceRange range);
    /// `base.Member`: the members of the base class, called without dynamic dispatch.
    BinderTarget ResolveBaseMember(const AstMemberExpr *member);
    /// The members named like *member* of a value of a class, interface, string, array or nullable type.
    BinderTarget ResolveReferenceMember(BoundExpr *object, const AstMemberExpr *member);
    /// A field symbol read through *receiver*, or statically when it is null.
    BinderTarget BindFieldSymbol(SemaSymbol *field, SemaStructInstance *owner, BoundExpr *receiver, SourceRange range);
    BoundExpr *BindNewObject(const SemaType *type, Span<AstArgument> arguments, SourceRange range);
    BoundExpr *BindArrayCreation(const AstArrayCreationExpr *node);
    BoundExpr *BindAs(const AstAsExpr *node);
    BoundExpr *BindTypeof(const AstTypeofExpr *node);
    BoundExpr *BindNullForgiving(const AstNullForgivingExpr *node);
    BoundExpr *BindIndexer(BoundExpr *object, BoundExpr *index, SourceRange range, bool viaBase);
    /// Runs `Add(element)` on a new collection for each element (MarlinScript §2.4).
    BoundExpr *BindCollectionInitializer(BoundExpr *initial, Span<AstNode *> elements, SourceRange range);
    BoundExpr *MakeTypeConversion(BoundExpr *operand, const SemaType *type, BoundTypeConversionKind kind,
                                  bool isExplicit);
    /// Converts between references, boxes and nullable types; *handled* is false when neither type is one, and
    /// the ordinary conversions apply.
    BoundExpr *ConvertReference(BoundExpr *expression, const SemaType *type, bool isExplicit, bool &handled);
    /// String concatenation, delegate combination and reference equality; *handled* is false for other operands.
    BoundExpr *BindReferenceOperator(TokenKind op, BoundExpr *left, BoundExpr *right, SourceRange range,
                                     bool &handled);
    /// The text of *value* by its `ToString()`, boxing value types that have none of their own.
    BoundExpr *CallToString(BoundExpr *value);
    /// Calls the method *name* of *receiver*; null after a diagnostic.
    BoundExpr *CallMethod(BoundExpr *receiver, StringView name, Span<BoundExpr *> arguments, SourceRange range);
    /// How a call of *function* on *receiver* is dispatched.
    BoundDispatch DispatchOf(SemaFunction *function, BoundExpr *receiver);
    /// A property or indexer access with its accessors this code may use.
    BoundExpr *MakeProperty(SemaSymbol *property, SemaStructInstance *owner, BoundExpr *receiver,
                            Span<BoundExpr *> arguments, SourceRange range);
    /// Whether *target* may be assigned: an lvalue, or a property with a usable setter.
    bool CanAssign(BoundExpr *target);
    /// A constructor's field initializers and its call of the base or another constructor of the same type.
    void BindConstructorPrologue(List<BoundStmt *> &statements);
    /// Whether the code being bound is inside *symbol*, a type, or one of the classes derived from it.
    [[nodiscard]] bool IsInsideType(const SemaSymbol *symbol) const;
    /// `a?.b`: the access on the value of *Target* when it is not null, else null.
    BoundExpr *BindConditionalAccess(const AstConditionalAccessExpr *node);
    /// `left ?? right`.
    BoundExpr *BindCoalesce(BoundExpr *left, AstNode *right, SourceRange range);
    BoundExpr *BindInterpolatedString(const AstInterpolatedStringExpr *node);
    /// `HasValue`, `Value` and `GetValueOrDefault` of a nullable value type; null for other names.
    BoundExpr *BindNullableValueMember(BoundExpr *object, const AstMemberExpr *member, bool &isMethod);
    /// Whether *value*, a reference or nullable, is not null.
    BoundExpr *MakeNotNull(BoundExpr *value, SourceRange range);

    // Delegates and lambdas (BinderDelegates.cpp)

    /// A lambda or method group as a value whose delegate type the context gives later (MarlinScript §3.7).
    BoundExpr *MakeUnbound(BinderTarget &target, const AstLambdaExpr *lambda, SourceRange range);
    /// Whether *value*, an unbound lambda or method group, can become a delegate of *type*.
    bool FitsDelegate(const BoundExpr *value, const SemaType *type);
    BoundExpr *ConvertToDelegate(BoundExpr *value, const SemaType *type);
    BoundExpr *BindLambda(const AstLambdaExpr *lambda, const SemaSignature &signature, const SemaType *type);
    BoundExpr *BindDelegateInvoke(BoundExpr *delegate, Span<AstArgument> arguments, SourceRange range);
    /// Records that the function being bound uses *local* of an enclosing function.
    void NoteCapture(SemaLocal *local, SourceRange range);
    /// The function whose `this` the code being bound uses: the function itself or the one a lambda is written in.
    [[nodiscard]] SemaFunction *ThisOwner() const;

    // Tasks and iterators (BinderAsync.cpp)

    /// Sets the kind and result type of *function* from `async` and the `yield` statements in *body*
    /// (MarlinScript §4.2, §4.3).
    void PrepareFunctionKind(SemaFunction *function, bool isAsync, AstNode *body, SourceRange range);
    BoundExpr *BindAwait(const AstAwaitExpr *node);
    BoundStmt *BindYield(const AstYieldStmt *node);
    /// The type `return` statements give: the result of an `async` function, else the return type.
    const SemaType *ReturnTypeForStatements();
    /// `foreach` over a script array or a type with `GetEnumerator()`.
    BoundStmt *BindScriptForeach(const AstForeachStmt *statement, BoundExpr *collection);

    // Patterns (BinderPatterns.cpp, MarlinScript §3.9)

    BoundExpr *BindIs(const AstIsExpr *node);
    /// Whether *input*, which can be read more than once, matches *pattern*; declares the pattern's variables in
    /// the current scope.
    BoundExpr *BindPattern(AstNode *pattern, BoundExpr *input);
    /// *value* if it can be read again without effects, else a read of a new *local* to store it in.
    BoundExpr *StoreInput(BoundExpr *value, SemaLocal *&local);
    /// *expression* converted between Ptr and Span types (§4.5), or null if neither converts to *type* that way.
    BoundExpr *ConvertPointer(BoundExpr *expression, const SemaType *type);
    BoundExpr *MakeLet(SemaLocal *local, BoundExpr *initial, BoundExpr *result);
    BoundExpr *BindSwitchExpression(const AstSwitchExpr *node, const SemaType *expected);
    BoundStmt *BindPatternSwitch(const AstSwitchStmt *statement);
    /// Whether unguarded *patterns* together match every value of *type*.
    bool IsExhaustive(Span<AstNode *const> patterns, const SemaType *type);
    /// Whether *pattern* alone matches every value of *type*.
    bool MatchesEverything(const AstNode *pattern, const SemaType *type);

    // Statements (BinderStatements.cpp)

    BoundStmt *BindStatement(AstNode *node);
    BoundBlock *BindBlock(const AstBlockStmt *block);
    BoundStmt *BindLocal(const AstVarStmt *declaration);
    BoundStmt *BindIf(const AstIfStmt *statement);
    BoundStmt *BindFor(const AstForStmt *statement);
    /// Checks the constant format or message and the arguments of `Debug.Print` / `Debug.Assert` (§9.11).
    bool CheckDebugOutput(const BoundIntrinsic *intrinsic);
    /// `Autodiff.Forward(f, …)` / `Autodiff.Backward(f, …)` (§9.12).
    BoundExpr *BindDerivative(const AstCallExpr *call, bool forward);
    /// Whether values of *type* have derivatives (Sema::IsDifferentiable).
    bool IsDifferentiable(const SemaType *type);
    /// `Autodiff.Detach(x)`: *x* with a zero derivative (§9.12).
    BoundExpr *BindDetach(const AstCallExpr *call);
    /// Whether the code being bound is the body of a `[Differentiable]` function.
    [[nodiscard]] bool InDifferentiableCode() const;
    /// Reports a call from differentiable code to a function with neither `[Differentiable]` nor custom derivatives
    /// that a derivative could flow through, outside `Autodiff.Detach`; false if so.
    bool CheckDifferentiableCall(SemaFunction *function, Span<BoundExpr *const> arguments, SourceRange range);
    /// Reads `[Unroll]`, `[Unroll(n)]` and `[MaxIterations(n)]` from the attributes of a loop statement into *loop*.
    void BindLoopControl(Span<AstAttribute> attributes, BoundLoop *loop);
    /// Starts a loop body whose locals get fresh storage each iteration; the caller restores *_iteration* to the
    /// result's parent when the body is bound.
    SemaIteration *BeginIteration(BoundLoop *loop);
    BoundStmt *BindForeach(const AstForeachStmt *statement);
    BoundStmt *BindSwitch(const AstSwitchStmt *statement);
    BoundExpr *BindCondition(AstNode *node);
    SemaLocal *DeclareLocal(StringView name, const SemaType *type, SourceRange range);
    SemaLocal *FindLocal(StringView name) const;
    void PushScope();
    void PopScope();
    /// The features a `static if` condition proves: every `Device.Supports(F)` in a chain of `&&`.
    void CollectGuards(AstNode *condition, List<StringView> &features);

    Sema *_sema;
    SemaFunction *_function;
    SemaContext _context;
    List<HashMap<StringView, SemaLocal *>> _scopes;
    List<StringView> _guards;
    uint32 _loopDepth = 0;
    uint32 _breakDepth = 0;
    /// How many `Autodiff.Detach` arguments the code being bound is in.
    uint32 _detached = 0;
    /// The loop iteration the code being bound is in, for the locals it declares.
    SemaIteration *_iteration = nullptr;
    /// Binding the target of `base.`: calls are direct.
    bool _baseAccess = false;
    /// Binding an object initializer, which may set `init` properties.
    bool _initializing = false;
    /// Binding the target of `+=` or `-=`, which may name an event outside its class.
    bool _eventCompound = false;
    /// Inside `checked(…)`.
    bool _checked = false;
    /// The non-null values that `ConditionalReceiverExpr` nodes stand for, innermost last.
    List<BoundExpr *> _conditionalReceivers;
};
} // namespace Marlin
