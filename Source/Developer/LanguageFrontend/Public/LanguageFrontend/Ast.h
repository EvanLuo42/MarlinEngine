#pragma once

#include "Containers/Span.h"
#include "EnumFlags.h"
#include "LanguageFrontend/Lexer.h"
#include "LanguageFrontend/Source.h"

namespace Marlin
{
/// Every node, including types and declarations. Nodes live in the arena of their file's syntax tree; names are
/// views into the source text.
enum class AstKind : uint8
{
    LiteralExpr,
    NameExpr,
    MemberExpr,
    CallExpr,
    IndexExpr,
    UnaryExpr,
    BinaryExpr,
    AssignExpr,
    ConditionalExpr,
    CastExpr,
    NewExpr,
    WithExpr,
    TupleExpr,
    SizeofExpr,
    TypeofExpr,
    NameofExpr,
    DefaultExpr,
    ThisExpr,
    IsExpr,
    AsExpr,
    BaseExpr,
    LambdaExpr,
    AwaitExpr,
    InterpolatedStringExpr,
    NullForgivingExpr,
    ConditionalAccessExpr,
    ConditionalReceiverExpr,
    SwitchExpr,
    CheckedExpr,
    RefExpr,
    ArrayCreationExpr,

    PredefinedType,
    NamedType,
    ArrayType,
    NullableType,
    SelfType,

    DiscardPattern,
    VarPattern,
    DeclarationPattern,
    ConstantPattern,
    RelationalPattern,
    PropertyPattern,
    BinaryPattern,
    NotPattern,

    BlockStmt,
    VarStmt,
    ExprStmt,
    IfStmt,
    ForStmt,
    ForeachStmt,
    WhileStmt,
    DoStmt,
    SwitchStmt,
    BreakStmt,
    ContinueStmt,
    ReturnStmt,
    DiscardStmt,
    YieldStmt,
    EmptyStmt,

    NamespaceDecl,
    UsingDecl,
    FunctionDecl,
    StructDecl,
    InterfaceDecl,
    EnumDecl,
    EnumMemberDecl,
    FieldDecl,
    DelegateDecl,
    PropertyDecl,
    ConstructorDecl,
    OperatorDecl,
    ParameterDecl,
    GenericParameterDecl,
    TypeDecl,
    ExtensionDecl,
};

enum class AstModifiers : uint32
{
    None = 0,
    Public = 1 << 0,
    Internal = 1 << 1,
    Private = 1 << 2,
    Static = 1 << 3,
    Const = 1 << 4,
    Readonly = 1 << 5,
    In = 1 << 6,
    Out = 1 << 7,
    Ref = 1 << 8,
    This = 1 << 9,
    Implicit = 1 << 11,
    Explicit = 1 << 12,
    /// Script-only modifiers, kept so the checker can report them in other dialects.
    Class = 1 << 13,
    Virtual = 1 << 14,
    Override = 1 << 15,
    Abstract = 1 << 16,
    Async = 1 << 17,
    Sealed = 1 << 18,
    Protected = 1 << 19,
    Event = 1 << 20,
};
MARLIN_ENUM_FLAGS(AstModifiers);

struct AstNode
{
    AstKind Kind;
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

    template <typename T> [[nodiscard]] T *TryAs()
    {
        return Kind == T::StaticKind ? static_cast<T *>(this) : nullptr;
    }

    template <typename T> [[nodiscard]] const T *TryAs() const
    {
        return Kind == T::StaticKind ? static_cast<const T *>(this) : nullptr;
    }

    [[nodiscard]] bool IsType() const
    {
        return Kind >= AstKind::PredefinedType && Kind <= AstKind::SelfType;
    }

    [[nodiscard]] bool IsExpression() const
    {
        return Kind <= AstKind::ArrayCreationExpr || IsType();
    }

    [[nodiscard]] bool IsPattern() const
    {
        return Kind >= AstKind::DiscardPattern && Kind <= AstKind::NotPattern;
    }

    [[nodiscard]] bool IsStatement() const
    {
        return Kind >= AstKind::BlockStmt && Kind <= AstKind::EmptyStmt;
    }
};

template <AstKind TKind> struct AstNodeOf : AstNode
{
    static constexpr AstKind StaticKind = TKind;
};

struct AstAttributeArgument
{
    /// Empty for a positional argument.
    StringView Name;
    AstNode *Value = nullptr;
};

/// `[Name(args)]`, or `[return: Name]` with *Target* "return".
struct AstAttribute
{
    SourceRange Range;
    StringView Target;
    /// The name as written, possibly qualified: `Marlin.Parameter`.
    StringView Name;
    Span<AstAttributeArgument> Arguments;
    bool HasArguments = false;
};

struct AstArgument
{
    SourceRange Range;
    /// The parameter name of a named argument, or empty.
    StringView Name;
    /// In, Out or Ref, or None.
    AstModifiers Modifier = AstModifiers::None;
    AstNode *Value = nullptr;
};

struct AstMemberInitializer
{
    SourceRange Range;
    StringView Name;
    AstNode *Value = nullptr;
};

/// A parameter of a lambda; *Type* is null when it is inferred.
struct AstLambdaParameter
{
    SourceRange Range;
    StringView Name;
    AstNode *Type = nullptr;
    AstModifiers Modifier = AstModifiers::None;
};

/// A number, string, rune, `true`, `false` or `null`.
struct AstLiteralExpr : AstNodeOf<AstKind::LiteralExpr>
{
    Token Literal;
};

/// An identifier, optionally with generic arguments, which may be types or constant expressions.
struct AstNameExpr : AstNodeOf<AstKind::NameExpr>
{
    StringView Name;
    Span<AstNode *> GenericArguments;
    bool HasGenericArguments = false;
};

struct AstMemberExpr : AstNodeOf<AstKind::MemberExpr>
{
    AstNode *Target = nullptr;
    StringView Name;
    SourceRange NameRange;
    Span<AstNode *> GenericArguments;
    bool HasGenericArguments = false;
};

struct AstCallExpr : AstNodeOf<AstKind::CallExpr>
{
    AstNode *Callee = nullptr;
    Span<AstArgument> Arguments;
};

struct AstIndexExpr : AstNodeOf<AstKind::IndexExpr>
{
    AstNode *Target = nullptr;
    AstNode *Index = nullptr;
};

/// A prefix or postfix operator: `-x`, `!x`, `~x`, `++x`, `x--`.
struct AstUnaryExpr : AstNodeOf<AstKind::UnaryExpr>
{
    TokenKind Operator = TokenKind::Minus;
    bool Postfix = false;
    AstNode *Operand = nullptr;
};

struct AstBinaryExpr : AstNodeOf<AstKind::BinaryExpr>
{
    TokenKind Operator = TokenKind::Plus;
    AstNode *Left = nullptr;
    AstNode *Right = nullptr;
};

/// `=` or a compound assignment, including `??=`.
struct AstAssignExpr : AstNodeOf<AstKind::AssignExpr>
{
    TokenKind Operator = TokenKind::Assign;
    AstNode *Target = nullptr;
    AstNode *Value = nullptr;
};

struct AstConditionalExpr : AstNodeOf<AstKind::ConditionalExpr>
{
    AstNode *Condition = nullptr;
    AstNode *WhenTrue = nullptr;
    AstNode *WhenFalse = nullptr;
};

struct AstCastExpr : AstNodeOf<AstKind::CastExpr>
{
    AstNode *Type = nullptr;
    AstNode *Operand = nullptr;
};

/// `new T(args) { A = 1 }` or `new T(args) { 1, 2 }`; either part may be missing.
struct AstNewExpr : AstNodeOf<AstKind::NewExpr>
{
    AstNode *Type = nullptr;
    Span<AstArgument> Arguments;
    Span<AstMemberInitializer> Initializers;
    bool HasInitializer = false;
    /// The elements of a collection initializer, passed to `Add`.
    Span<AstNode *> Elements;
    bool HasCollectionInitializer = false;
};

/// `new T[n]`, `new T[] { a, b }` or `new[] { a, b }`; *Element* is null for `new[]` and *Length* is null when
/// the elements give it.
struct AstArrayCreationExpr : AstNodeOf<AstKind::ArrayCreationExpr>
{
    AstNode *Element = nullptr;
    AstNode *Length = nullptr;
    Span<AstNode *> Elements;
    bool HasElements = false;
};

struct AstWithExpr : AstNodeOf<AstKind::WithExpr>
{
    AstNode *Target = nullptr;
    Span<AstMemberInitializer> Initializers;
};

struct AstTupleExpr : AstNodeOf<AstKind::TupleExpr>
{
    Span<AstNode *> Elements;
};

struct AstSizeofExpr : AstNodeOf<AstKind::SizeofExpr>
{
    AstNode *Type = nullptr;
};

struct AstTypeofExpr : AstNodeOf<AstKind::TypeofExpr>
{
    AstNode *Type = nullptr;
};

struct AstNameofExpr : AstNodeOf<AstKind::NameofExpr>
{
    AstNode *Operand = nullptr;
};

/// `default(T)`, or `default` with the type taken from the context.
struct AstDefaultExpr : AstNodeOf<AstKind::DefaultExpr>
{
    AstNode *Type = nullptr;
};

struct AstThisExpr : AstNodeOf<AstKind::ThisExpr>
{
};

/// `x is pattern`.
struct AstIsExpr : AstNodeOf<AstKind::IsExpr>
{
    AstNode *Operand = nullptr;
    AstNode *Pattern = nullptr;
};

struct AstAsExpr : AstNodeOf<AstKind::AsExpr>
{
    AstNode *Operand = nullptr;
    AstNode *Type = nullptr;
};

/// `base`, as the target of a member access or index.
struct AstBaseExpr : AstNodeOf<AstKind::BaseExpr>
{
};

/// `x => x * 2` or `(int a, ref float b) => { ... }`, optionally `async` or `static`.
struct AstLambdaExpr : AstNodeOf<AstKind::LambdaExpr>
{
    Span<AstLambdaParameter> Parameters;
    /// A BlockStmt or an expression.
    AstNode *Body = nullptr;
    bool IsStatic = false;
    bool IsAsync = false;
};

struct AstAwaitExpr : AstNodeOf<AstKind::AwaitExpr>
{
    AstNode *Operand = nullptr;
};

/// A piece of an interpolated string: text, or a hole with its expression and optional format.
struct AstInterpolationPart
{
    SourceRange Range;
    /// The decoded text, when *Expression* is null.
    StringView Text;
    AstNode *Expression = nullptr;
    StringView Format;
};

/// `$"text {expression:format} text"`.
struct AstInterpolatedStringExpr : AstNodeOf<AstKind::InterpolatedStringExpr>
{
    Span<AstInterpolationPart> Parts;
};

/// `x!`: *x* is asserted not to be null.
struct AstNullForgivingExpr : AstNodeOf<AstKind::NullForgivingExpr>
{
    AstNode *Operand = nullptr;
};

/// `a?.b.c` or `a?[i]`: *Access* is the chain after the `?`, applied to a ConditionalReceiverExpr that stands for
/// the non-null value of *Target*.
struct AstConditionalAccessExpr : AstNodeOf<AstKind::ConditionalAccessExpr>
{
    AstNode *Target = nullptr;
    AstNode *Access = nullptr;
};

struct AstConditionalReceiverExpr : AstNodeOf<AstKind::ConditionalReceiverExpr>
{
};

struct AstSwitchArm
{
    SourceRange Range;
    AstNode *Pattern = nullptr;
    AstNode *When = nullptr;
    AstNode *Value = nullptr;
};

/// `selector switch { pattern when condition => value, ... }`.
struct AstSwitchExpr : AstNodeOf<AstKind::SwitchExpr>
{
    AstNode *Selector = nullptr;
    Span<AstSwitchArm> Arms;
};

/// `checked(expression)`: integer arithmetic in it faults on overflow.
struct AstCheckedExpr : AstNodeOf<AstKind::CheckedExpr>
{
    AstNode *Operand = nullptr;
};

/// `ref x`, the initializer of a `ref` local.
struct AstRefExpr : AstNodeOf<AstKind::RefExpr>
{
    AstNode *Operand = nullptr;
};

/// `int`, `float`, `void` and the other keyword types.
struct AstPredefinedType : AstNodeOf<AstKind::PredefinedType>
{
    TokenKind Keyword = TokenKind::KeywordInt;
};

/// `Name<Args>`, or `Qualifier.Name<Args>`.
struct AstNamedType : AstNodeOf<AstKind::NamedType>
{
    AstNode *Qualifier = nullptr;
    StringView Name;
    Span<AstNode *> GenericArguments;
    bool HasGenericArguments = false;
};

/// `Element[Size]`, or `Element[]` with a null *Size*.
struct AstArrayType : AstNodeOf<AstKind::ArrayType>
{
    AstNode *Element = nullptr;
    AstNode *Size = nullptr;
};

/// `Element?`.
struct AstNullableType : AstNodeOf<AstKind::NullableType>
{
    AstNode *Element = nullptr;
};

/// `this` where a type is expected, in an interface or extension: the type implementing the interface, or the type
/// the extension extends (§3.5).
struct AstSelfType : AstNodeOf<AstKind::SelfType>
{
};

/// `_`.
struct AstDiscardPattern : AstNodeOf<AstKind::DiscardPattern>
{
};

/// `var name`.
struct AstVarPattern : AstNodeOf<AstKind::VarPattern>
{
    StringView Name;
    SourceRange NameRange;
};

/// `Type name`.
struct AstDeclarationPattern : AstNodeOf<AstKind::DeclarationPattern>
{
    AstNode *Type = nullptr;
    StringView Name;
    SourceRange NameRange;
};

/// A constant, or a name that may be a type, compared with the value.
struct AstConstantPattern : AstNodeOf<AstKind::ConstantPattern>
{
    AstNode *Value = nullptr;
};

/// `< value`, `<= value`, `> value` or `>= value`.
struct AstRelationalPattern : AstNodeOf<AstKind::RelationalPattern>
{
    TokenKind Operator = TokenKind::Less;
    AstNode *Value = nullptr;
};

struct AstSubpattern
{
    SourceRange Range;
    StringView Name;
    AstNode *Pattern = nullptr;
};

/// `Type { Name: pattern, ... } name`; the type and the name are optional.
struct AstPropertyPattern : AstNodeOf<AstKind::PropertyPattern>
{
    AstNode *Type = nullptr;
    Span<AstSubpattern> Subpatterns;
    StringView Name;
    SourceRange NameRange;
};

/// `left and right` or `left or right`.
struct AstBinaryPattern : AstNodeOf<AstKind::BinaryPattern>
{
    bool IsAnd = false;
    AstNode *Left = nullptr;
    AstNode *Right = nullptr;
};

struct AstNotPattern : AstNodeOf<AstKind::NotPattern>
{
    AstNode *Operand = nullptr;
};

struct AstBlockStmt : AstNodeOf<AstKind::BlockStmt>
{
    Span<AstNode *> Statements;
};

struct AstVariableDeclarator
{
    SourceRange Range;
    StringView Name;
    AstNode *Initializer = nullptr;
};

/// `T a = 1, b;`, `var a = 1;` (null *Type*), `const T a = 1;` or `ref T a = ref b;`.
struct AstVarStmt : AstNodeOf<AstKind::VarStmt>
{
    Span<AstAttribute> Attributes;
    AstNode *Type = nullptr;
    Span<AstVariableDeclarator> Declarators;
    bool IsConst = false;
    bool IsRef = false;
};

struct AstExprStmt : AstNodeOf<AstKind::ExprStmt>
{
    AstNode *Expression = nullptr;
};

struct AstIfStmt : AstNodeOf<AstKind::IfStmt>
{
    AstNode *Condition = nullptr;
    AstNode *Then = nullptr;
    AstNode *Else = nullptr;
    bool IsStatic = false;
};

struct AstForStmt : AstNodeOf<AstKind::ForStmt>
{
    Span<AstAttribute> Attributes;
    Span<AstNode *> Initializers;
    AstNode *Condition = nullptr;
    Span<AstNode *> Increments;
    AstNode *Body = nullptr;
};

struct AstForeachStmt : AstNodeOf<AstKind::ForeachStmt>
{
    Span<AstAttribute> Attributes;
    AstNode *Type = nullptr;
    StringView Name;
    SourceRange NameRange;
    AstNode *Collection = nullptr;
    AstNode *Body = nullptr;
};

struct AstWhileStmt : AstNodeOf<AstKind::WhileStmt>
{
    Span<AstAttribute> Attributes;
    AstNode *Condition = nullptr;
    AstNode *Body = nullptr;
};

struct AstDoStmt : AstNodeOf<AstKind::DoStmt>
{
    Span<AstAttribute> Attributes;
    AstNode *Body = nullptr;
    AstNode *Condition = nullptr;
};

/// `case pattern:` or `case pattern when condition:`; a constant label is a ConstantPattern.
struct AstCaseLabel
{
    SourceRange Range;
    AstNode *Pattern = nullptr;
    AstNode *When = nullptr;
};

struct AstSwitchSection
{
    SourceRange Range;
    Span<AstCaseLabel> Labels;
    bool HasDefault = false;
    Span<AstNode *> Statements;
};

struct AstSwitchStmt : AstNodeOf<AstKind::SwitchStmt>
{
    AstNode *Selector = nullptr;
    Span<AstSwitchSection> Sections;
};

struct AstBreakStmt : AstNodeOf<AstKind::BreakStmt>
{
};

struct AstContinueStmt : AstNodeOf<AstKind::ContinueStmt>
{
};

struct AstReturnStmt : AstNodeOf<AstKind::ReturnStmt>
{
    AstNode *Value = nullptr;
};

struct AstDiscardStmt : AstNodeOf<AstKind::DiscardStmt>
{
};

/// `yield return value;`, or `yield break;` with *IsBreak*.
struct AstYieldStmt : AstNodeOf<AstKind::YieldStmt>
{
    AstNode *Value = nullptr;
    bool IsBreak = false;
};

struct AstEmptyStmt : AstNodeOf<AstKind::EmptyStmt>
{
};

/// What every declaration has.
struct AstDecl : AstNode
{
    Span<AstAttribute> Attributes;
    AstModifiers Modifiers = AstModifiers::None;
    StringView Name;
    SourceRange NameRange;
    /// The namespace or type the declaration is in; null at the top of a file.
    AstDecl *Parent = nullptr;
};

template <AstKind TKind> struct AstDeclOf : AstDecl
{
    static constexpr AstKind StaticKind = TKind;
};

/// `namespace A.B;` or `namespace A.B { ... }`; *Name* is the dotted name as written.
struct AstNamespaceDecl : AstDeclOf<AstKind::NamespaceDecl>
{
    bool FileScoped = false;
    Span<AstNode *> Members;
};

/// `using A.B;`, `using X = A.B.C;` or `using static A.B;`. An alias with a visibility modifier, `public using
/// float3 = Vector<float, 3>;`, is a member of its namespace rather than of its file.
struct AstUsingDecl : AstDeclOf<AstKind::UsingDecl>
{
    /// The alias, or empty.
    StringView Alias;
    AstNode *Target = nullptr;
    bool IsStatic = false;
};

/// A type parameter, or a constant one with *ValueType*; `= value` gives it a *Default*.
struct AstGenericParameterDecl : AstDeclOf<AstKind::GenericParameterDecl>
{
    AstNode *ValueType = nullptr;
    AstNode *Default = nullptr;
};

/// `where T : IFoo, struct`; also `unmanaged`, `class` and `new()`.
struct AstConstraint
{
    SourceRange Range;
    StringView Parameter;
    Span<AstNode *> Bounds;
    bool Struct = false;
    bool Unmanaged = false;
    bool Class = false;
    bool New = false;
};

struct AstParameterDecl : AstDeclOf<AstKind::ParameterDecl>
{
    AstNode *Type = nullptr;
    AstNode *Default = nullptr;
};

/// The body of a function, constructor, operator, property or accessor: a block or `=> expr`. A declaration scan
/// leaves it unparsed, as the token range [BodyBegin, BodyEnd) of its file, until ParseBody is called.
struct AstBody
{
    AstBlockStmt *Block = nullptr;
    AstNode *Expression = nullptr;
    uint32 BodyBegin = 0;
    uint32 BodyEnd = 0;
    bool Parsed = false;

    [[nodiscard]] bool IsMissing() const
    {
        return BodyBegin == BodyEnd;
    }
};

struct AstFunctionDecl : AstDeclOf<AstKind::FunctionDecl>
{
    AstNode *ReturnType = nullptr;
    Span<AstGenericParameterDecl *> GenericParameters;
    Span<AstParameterDecl *> Parameters;
    Span<AstConstraint> Constraints;
    AstBody Body;
};

struct AstConstructorDecl : AstDeclOf<AstKind::ConstructorDecl>
{
    Span<AstParameterDecl *> Parameters;
    AstBody Body;
    /// `: base(args)` or `: this(args)`.
    bool HasInitializer = false;
    bool InitializerIsBase = false;
    Span<AstArgument> InitializerArguments;
    SourceRange InitializerRange;
};

/// `operator +(…)`, or an `implicit`/`explicit operator T(…)` conversion with Implicit or Explicit set and
/// *Operator* EndOfFile.
struct AstOperatorDecl : AstDeclOf<AstKind::OperatorDecl>
{
    TokenKind Operator = TokenKind::Plus;
    AstNode *ReturnType = nullptr;
    Span<AstParameterDecl *> Parameters;
    AstBody Body;
};

/// A field, `const` or `static readonly` value, `event`, or a top-level `[GroupShared]` declaration; one node per
/// declarator.
struct AstFieldDecl : AstDeclOf<AstKind::FieldDecl>
{
    AstNode *Type = nullptr;
    AstNode *Initializer = nullptr;
};

/// The `set` or `init` accessor of a property, which may have its own visibility.
struct AstAccessor
{
    SourceRange Range;
    AstModifiers Modifiers = AstModifiers::None;
    AstBody Body;
    bool Present = false;
};

/// A property, `T Name => expr;` or `T Name { get ...; set ...; } = value;`, or an indexer `T this[int i] { ... }`.
/// Accessors without bodies make an auto-property.
struct AstPropertyDecl : AstDeclOf<AstKind::PropertyDecl>
{
    AstNode *Type = nullptr;
    AstBody Getter;
    bool HasGetter = false;
    AstAccessor Setter;
    /// The setter is written `init`.
    bool SetterIsInit = false;
    AstNode *Initializer = nullptr;
    Span<AstParameterDecl *> Parameters;
    bool IsIndexer = false;
};

/// `delegate TResult Func<T, TResult>(T argument);`.
struct AstDelegateDecl : AstDeclOf<AstKind::DelegateDecl>
{
    AstNode *ReturnType = nullptr;
    Span<AstGenericParameterDecl *> GenericParameters;
    Span<AstParameterDecl *> Parameters;
    Span<AstConstraint> Constraints;
};

/// A struct, or a class when Modifiers has Class.
struct AstStructDecl : AstDeclOf<AstKind::StructDecl>
{
    /// In source order: `<T>`, then `: Base, IFoo` (a class's base class comes first), then `where` clauses.
    Span<AstGenericParameterDecl *> GenericParameters;
    Span<AstNode *> Interfaces;
    Span<AstConstraint> Constraints;
    Span<AstDecl *> Members;
};

struct AstInterfaceDecl : AstDeclOf<AstKind::InterfaceDecl>
{
    Span<AstGenericParameterDecl *> GenericParameters;
    Span<AstNode *> Bases;
    Span<AstConstraint> Constraints;
    Span<AstDecl *> Members;
};

/// An associated type (§3.5): `type Name : IBound;` in an interface, or its binding `type Name = Type;` in a struct
/// or extension.
struct AstTypeDecl : AstDeclOf<AstKind::TypeDecl>
{
    Span<AstNode *> Bounds;
    /// The type a binding gives; null for the requirement of an interface.
    AstNode *Target = nullptr;
};

/// `extension<T> Type<T> : IFoo where T : IBar { ... }`: members and interface implementations added to a type outside
/// its declaration (§3.5). *Name* is the extended type as written.
struct AstExtensionDecl : AstDeclOf<AstKind::ExtensionDecl>
{
    Span<AstGenericParameterDecl *> GenericParameters;
    AstNode *ExtendedType = nullptr;
    Span<AstNode *> Interfaces;
    Span<AstConstraint> Constraints;
    Span<AstDecl *> Members;
};

struct AstEnumMemberDecl : AstDeclOf<AstKind::EnumMemberDecl>
{
    AstNode *Value = nullptr;
};

struct AstEnumDecl : AstDeclOf<AstKind::EnumDecl>
{
    AstNode *UnderlyingType = nullptr;
    Span<AstEnumMemberDecl *> Members;
};

/// The syntax tree of one file: its usings and declarations at the top level, and the tokens unparsed bodies
/// refer to.
struct AstFile
{
    const SourceFile *File = nullptr;
    Span<const Token> Tokens;
    Span<AstNode *> Members;
};
} // namespace Marlin
