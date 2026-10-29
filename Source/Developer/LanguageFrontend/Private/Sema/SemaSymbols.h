#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "LanguageFrontend/Ast.h"
#include "LanguageIR/MirModule.h"
#include "Sema/SemaType.h"

namespace Marlin
{
struct BoundStmt;
struct SemaFunction;

enum class SemaSymbolKind : uint8
{
    Namespace,
    Struct,
    /// A script class (MarlinScript §3.1).
    Class,
    /// A script delegate type: `delegate R Name(parameters);`.
    Delegate,
    Interface,
    Enum,
    EnumMember,
    Function,
    Constructor,
    Operator,
    Field,
    Property,
    /// A namespace-level `public using Name = Type;`.
    Alias,
    /// A built-in type name such as `float3` or `Texture2D`, or a built-in generic such as `Vector`.
    BuiltinType,
    /// A built-in static class such as `Wave`, `Barrier` or `Hit`, whose members the binder knows.
    BuiltinStatic,
    /// A group of built-in functions, such as `dot` or `lerp`.
    BuiltinFunction,
    /// `type Name : IBound;` in an interface, or a binding `type Name = Type;` in a struct or extension (§3.5).
    AssociatedType,
    /// An `extension` declaration (§3.5). It is not a member of its namespace: Sema keeps every extension in a list,
    /// and its members are found through the types it extends.
    Extension,
};

enum class SemaVisibility : uint8
{
    Public,
    Internal,
    Private,
    /// Visible in the declaring class and the classes derived from it (scripts only).
    Protected,
};

/// A declared name. Namespaces are merged across files; functions with the same name are separate symbols
/// that lookup returns together.
struct SemaSymbol
{
    SemaSymbolKind Kind = SemaSymbolKind::Namespace;
    StringView Name;
    SemaSymbol *Parent = nullptr;
    AstDecl *Decl = nullptr;
    const AstFile *File = nullptr;
    /// The mount root the declaration comes from, for `internal` (§1.4) and script packages (MarlinScript §1.2).
    uint32 Package = 0;
    SemaVisibility Visibility = SemaVisibility::Internal;
    HashMap<StringView, List<SemaSymbol *>> Members;
    /// Members in declaration order.
    List<SemaSymbol *> Ordered;
    /// The resolved type of an alias or enum.
    const SemaType *Type = nullptr;
    bool TypeResolving = false;
    /// The BuiltinTypeId of a core library struct, class or interface with `[Builtin]`.
    uint32 BuiltinId = 0;
    /// Declared in the core library (§9.10).
    bool IsCore = false;
    /// The value of an enum member once evaluated.
    struct SemaConstant *Value = nullptr;
    bool ValueResolving = false;

    [[nodiscard]] bool IsStatic() const
    {
        return Decl != nullptr && HasAnyFlags(Decl->Modifiers, AstModifiers::Static | AstModifiers::Const);
    }

    /// Structs, classes, interfaces and delegates: declarations instantiated into a SemaStructInstance.
    [[nodiscard]] bool IsAggregate() const
    {
        return Kind == SemaSymbolKind::Struct || Kind == SemaSymbolKind::Class || Kind == SemaSymbolKind::Interface ||
               Kind == SemaSymbolKind::Delegate;
    }

    /// Whether the symbol names a type.
    [[nodiscard]] bool IsType() const
    {
        return IsAggregate() || Kind == SemaSymbolKind::Enum || Kind == SemaSymbolKind::Alias ||
               Kind == SemaSymbolKind::AssociatedType;
    }

    /// Declarations whose members code inside them sees by their simple names: aggregates, enums and extensions.
    [[nodiscard]] bool IsTypeScope() const
    {
        return IsAggregate() || Kind == SemaSymbolKind::Enum || Kind == SemaSymbolKind::Extension;
    }
};

/// A compile-time value: a scalar, a composite of constants, or the zero value of any type.
struct SemaConstant
{
    const SemaType *Type = nullptr;
    /// Booleans are 0 or 1, integers are their bits sign- or zero-extended to 64, floats are stored as double.
    uint64 Bits = 0;
    double Float = 0;
    Span<SemaConstant *> Elements;
    /// The text of a string or feature constant.
    StringView Text;
    bool IsZero = false;

    [[nodiscard]] int64 GetInt() const
    {
        return static_cast<int64>(Bits);
    }
};

/// A generic argument: a type, or a constant for a value parameter.
struct SemaGenericArgument
{
    const SemaType *Type = nullptr;
    SemaConstant *Value = nullptr;

    bool operator==(const SemaGenericArgument &other) const;
};

struct SemaField
{
    StringView Name;
    const SemaType *Type = nullptr;
    /// The field, or the property whose hidden field this is.
    SemaSymbol *Symbol = nullptr;
    /// An explicit `[Offset(n)]`, or ~0u.
    uint32 Offset = ~0u;
};

/// How a class or struct implements one interface (MarlinScript §3.4).
struct SemaInterfaceMap
{
    SemaStructInstance *Interface = nullptr;
    /// The implementing method for each of the interface's VirtualSlots, in order.
    List<struct SemaFunction *> Methods;
};

/// A struct, class, interface or delegate declaration with its generic arguments bound.
struct SemaStructInstance
{
    SemaSymbol *Symbol = nullptr;
    Span<SemaGenericArgument> Arguments;
    /// Instance fields in layout order; a class's start with those of its base.
    List<SemaField> Fields;
    /// A `[StructLayout(Size = n)]`, or 0.
    uint32 Size = 0;
    bool FieldsResolved = false;
    bool FieldsResolving = false;
    const MirType *Lowered = nullptr;
    /// The base class of a class; null for `object`, structs, interfaces and delegates (MarlinScript §3.4).
    SemaStructInstance *Base = nullptr;
    /// Every interface the type implements, its base's included, each once.
    List<SemaStructInstance *> Interfaces;
    /// A class's virtual methods by slot, its base's first; an interface's methods and accessors in order.
    List<struct SemaFunction *> VirtualSlots;
    List<SemaInterfaceMap> InterfaceMaps;
    bool BasesResolved = false;
    bool BasesResolving = false;
    /// Slots and interface maps.
    bool HierarchyResolved = false;
    bool HierarchyResolving = false;
    bool HierarchyFailed = false;
    /// The type an extension instance extends: the `this` of its members (§3.5).
    const SemaType *Extended = nullptr;
};

enum class SemaParameterMode : uint8
{
    Value,
    In,
    Out,
    Ref,
};

/// The parameter types, modes and return type of a function or delegate.
struct SemaSignature
{
    List<const SemaType *> Parameters;
    List<SemaParameterMode> Modes;
    const SemaType *ReturnType = nullptr;
};

/// How a function's body runs (MarlinScript §4.2, §4.3).
enum class SemaFunctionKind : uint8
{
    Normal,
    /// `async`: returns `Task` or `Task<T>` and may `await`.
    Async,
    /// Has `yield`: returns `IEnumerable<T>` or `IEnumerator<T>`.
    Iterator,
};

/// Which accessor of a property or indexer a function instance is.
enum class SemaAccessor : uint8
{
    None,
    Getter,
    Setter,
};

/// One run of a loop body: locals declared in it get fresh storage each time round, so a lambda captures the
/// iteration it was made in.
struct SemaIteration
{
    SemaIteration *Parent = nullptr;
};

struct SemaLocal
{
    StringView Name;
    const SemaType *Type = nullptr;
    SourceRange Range;
    SemaParameterMode Mode = SemaParameterMode::Value;
    bool IsParameter = false;
    /// Cannot be assigned: an `in` parameter, the `this` of a struct's `readonly` member.
    bool IsReadOnly = false;
    /// Attributes of the parameter or local declaration.
    Span<AstAttribute> Attributes;
    /// The value of a `const` local.
    SemaConstant *Constant = nullptr;
    /// The function or lambda that declares the local.
    struct SemaFunction *Function = nullptr;
    /// A nested lambda uses the local, so it lives in a closure (MarlinScript §3.7).
    bool IsCaptured = false;
    /// The innermost loop iteration the local is declared in, or null.
    SemaIteration *Iteration = nullptr;
};

struct SemaCallSite
{
    SemaFunction *Callee = nullptr;
    SourceRange Range;
    /// The features `static if (Device.Supports(…))` proved where the call is (§9.7).
    Span<StringView> Guards;
};

enum class SemaFunctionState : uint8
{
    Declared,
    Binding,
    Bound,
    Failed,
};

/// A function, constructor, operator, property accessor or lambda with its generic arguments bound; non-generic
/// declarations have one instance.
struct SemaFunction
{
    SemaSymbol *Symbol = nullptr;
    SemaStructInstance *Owner = nullptr;
    Span<SemaGenericArgument> GenericArguments;
    /// The getter or setter of a property or indexer.
    SemaAccessor Accessor = SemaAccessor::None;
    /// The virtual slot of a class method, or the index of an interface member, once the hierarchy is resolved;
    /// ~0u for other functions.
    uint32 Slot = ~0u;
    List<SemaLocal *> Parameters;
    const SemaType *ReturnType = nullptr;
    /// The implicit `this` of an instance member, passed by reference as the first argument; a class's by value.
    SemaLocal *This = nullptr;
    SemaFunctionState State = SemaFunctionState::Declared;
    BoundStmt *Body = nullptr;
    bool IsConstFunction = false;
    bool SignatureResolved = false;
    /// Features the declaration names in `[Requires]`.
    List<StringView> Requires;
    /// Functions and built-ins this one calls, with where, for recursion, stage and feature checks.
    List<SemaCallSite> Callees;
    SemaFunctionKind Kind = SemaFunctionKind::Normal;
    /// What `return` gives in an `async` function (void for `Task`) and `yield return` gives in an iterator.
    const SemaType *ResultType = nullptr;
    /// The function a lambda is written in.
    SemaFunction *Enclosing = nullptr;
    /// The loop iteration of *Enclosing* the lambda is made in, or null.
    SemaIteration *Iteration = nullptr;
    /// The locals of enclosing functions a lambda uses, and whether it uses `this` (MarlinScript §3.7).
    List<SemaLocal *> Captures;
    bool CapturesThis = false;
    /// A `static` lambda, which captures nothing.
    bool IsStaticLambda = false;
    /// The lambdas written directly in this function's body.
    List<SemaFunction *> Lambdas;
    /// Stages whose intrinsics this function uses directly, as bits of MirStage; ~0u when any stage may call it.
    uint32 AllowedStages = ~0u;
    /// The first use that restricted AllowedStages, for the diagnostic.
    SourceRange StageUse;
    MirFunction *Lowered = nullptr;
    /// The functions `[ForwardDerivative]` and `[BackwardDerivative]` name, which differentiation calls instead of
    /// differentiating this one (§9.12), and the parameters they take a DifferentialPair for, as bits.
    SemaFunction *ForwardDerivative = nullptr;
    /// The `[ForwardDerivative2]`: the forward derivative along two directions at once (DifferentialPair2).
    SemaFunction *ForwardDerivative2 = nullptr;
    SemaFunction *BackwardDerivative = nullptr;
    uint32 DerivativeParameters = 0;
};

enum class SemaGlobalKind : uint8
{
    GroupShared,
    /// `[Specialization]`: a constant of the pipeline, fixed when it is created (§9.4).
    Specialization,
    /// A script `static` field: one per generic instance of its class (MarlinScript §3.4).
    Static,
};

/// A module-scope variable: a `[GroupShared]` field, a `[Specialization]` constant or a script static field.
struct SemaGlobal
{
    SemaSymbol *Symbol = nullptr;
    /// The instance a script static field belongs to.
    SemaStructInstance *Owner = nullptr;
    const SemaType *Type = nullptr;
    SemaGlobalKind Kind = SemaGlobalKind::GroupShared;
    SemaConstant *Default = nullptr;
    /// The initializer of a script static field, which the module's static initializer runs.
    struct BoundExpr *Initializer = nullptr;
    MirGlobal *Lowered = nullptr;
};
} // namespace Marlin
