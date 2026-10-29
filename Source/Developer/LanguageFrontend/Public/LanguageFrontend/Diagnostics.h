#pragma once

#include "Containers/List.h"
#include "LanguageFrontend/Source.h"
#include "String/Format.h"
#include "String/String.h"

namespace Marlin
{
/// Stable codes, written as `M` and four digits: 1xxx lexical, 2xxx syntax, 3xxx names and types, 4xxx dialect and
/// stage rules, 5xxx compile-time evaluation.
enum class DiagnosticCode : uint16
{
    UnexpectedCharacter = 1001,
    UnterminatedComment = 1002,
    UnterminatedString = 1003,
    InvalidNumber = 1004,
    InvalidEscape = 1005,
    NumberOutOfRange = 1006,

    Expected = 2001,
    UnexpectedToken = 2002,
    InvalidModifier = 2003,
    FileScopedNamespaceNotFirst = 2004,
    DuplicateModifier = 2005,

    UndefinedName = 3001,
    AmbiguousName = 3002,
    DuplicateDefinition = 3003,
    TypeMismatch = 3004,
    NoConversion = 3005,
    NoMatchingOverload = 3006,
    AmbiguousOverload = 3007,
    NotAType = 3008,
    NotAValue = 3009,
    NotAssignable = 3010,
    InvalidOperands = 3011,
    UnassignedVariable = 3012,
    MissingReturn = 3013,
    WrongArgumentCount = 3014,
    InaccessibleMember = 3015,
    InvalidGenericArguments = 3016,
    ConstraintNotSatisfied = 3017,
    RecursionNotAllowed = 3018,
    InvalidAttribute = 3019,
    LayoutViolation = 3020,
    MissingInterfaceMember = 3021,
    InvalidStatement = 3022,
    PossiblyNull = 3023,
    MissingAssociatedType = 3024,
    DuplicateConformance = 3025,
    InvalidExtensionMember = 3026,
    SelfTypeOutsideInterface = 3027,
    /// Differentiable code calls a function without a derivative, or a type has none (§9.12).
    NotDifferentiable = 3028,
    /// A `[ForwardDerivative]` or `[BackwardDerivative]` that names no function or one with the wrong signature.
    InvalidCustomDerivative = 3029,
    /// A DifferentialPair passed for a `[NoDiff]` parameter.
    PairForNoDiffParameter = 3030,

    NotInDialect = 4001,
    NotInStage = 4002,
    FeatureRequired = 4003,
    InvalidEntryPoint = 4004,
    InvalidMaterial = 4005,
    NonUniformCooperative = 4006,
    SurfaceShapeNotConstant = 4007,
    ShaderArgumentsTooLarge = 4008,
    VariantBudgetExceeded = 4009,

    NotConstant = 5001,
    EvaluationLimit = 5002,
    DivisionByZero = 5003,
    IndexOutOfRange = 5004,
};

enum class DiagnosticSeverity : uint8
{
    Error,
    Warning,
};

/// A range of source the diagnostic points at. The primary label is underlined with `^`, secondary ones with `-`.
struct DiagnosticLabel
{
    SourceRange Range;
    String Message;
    bool Primary = false;
};

enum class DiagnosticNoteKind : uint8
{
    Note,
    Help,
};

/// A note or help after the source excerpt: `= note: …` without a range, or a block of its own with one.
struct DiagnosticNote
{
    DiagnosticNoteKind Kind = DiagnosticNoteKind::Note;
    SourceRange Range;
    String Message;
};

/// One problem, rendered like rustc: a header, the source lines its labels point at, then its notes.
struct LANGUAGEFRONTEND_API LanguageDiagnostic
{
    DiagnosticSeverity Severity = DiagnosticSeverity::Error;
    DiagnosticCode Code = DiagnosticCode::UnexpectedToken;
    String Message;
    List<DiagnosticLabel> Labels;
    List<DiagnosticNote> Notes;

    /// The range of the primary label; invalid if it has none.
    [[nodiscard]] SourceRange GetRange() const;

    /// Sets the text under the primary label.
    LanguageDiagnostic &WithLabel(StringView message);
    LanguageDiagnostic &WithSecondaryLabel(SourceRange range, StringView message);
    LanguageDiagnostic &WithNote(StringView message);
    LanguageDiagnostic &WithNote(SourceRange range, StringView message);
    LanguageDiagnostic &WithHelp(StringView message);
};

/// The source files of a compilation, by id from 1, and the problems found in them.
class LANGUAGEFRONTEND_API LanguageDiagnostics
{
public:
    LanguageDiagnostics() = default;
    MARLIN_NONCOPYABLE(LanguageDiagnostics);

    /// Registers a file so ranges can name it; returns its id.
    uint32 AddFile(SourceFile &file);
    [[nodiscard]] const SourceFile *GetFile(uint32 id) const;

    /// Adds a diagnostic whose primary label is *range*; the result can be given labels and notes until the next
    /// diagnostic is reported.
    LanguageDiagnostic &Report(DiagnosticSeverity severity, DiagnosticCode code, SourceRange range,
                               StringView message);

    template <typename... TArgs>
    LanguageDiagnostic &Error(DiagnosticCode code, SourceRange range, FormatString<TypeIdentity<TArgs>...> format,
                              const TArgs &...args)
    {
        return Report(DiagnosticSeverity::Error, code, range, Format(format, args...).AsView());
    }

    template <typename... TArgs>
    LanguageDiagnostic &Warning(DiagnosticCode code, SourceRange range, FormatString<TypeIdentity<TArgs>...> format,
                                const TArgs &...args)
    {
        return Report(DiagnosticSeverity::Warning, code, range, Format(format, args...).AsView());
    }

    [[nodiscard]] bool HasErrors() const
    {
        return _errorCount != 0;
    }

    [[nodiscard]] uint32 GetErrorCount() const
    {
        return _errorCount;
    }

    [[nodiscard]] Span<const LanguageDiagnostic> GetAll() const
    {
        return _diagnostics.AsSpan();
    }

    /// Renders every diagnostic the way rustc does:
    ///
    ///     error[M3001]: cannot find `Albdo` in this scope
    ///      --> /Project/Language/Shaders/Rock.mshader:9:31
    ///       |
    ///     9 |     Slab rock = Slab.Default with { Albdo = albedo.rgb };
    ///       |                                     ^^^^^ not a field of `Slab`
    ///       |
    ///       = help: a field with a similar name exists: `Albedo`
    [[nodiscard]] String ToString() const;

private:
    List<SourceFile *> _files;
    List<LanguageDiagnostic> _diagnostics;
    uint32 _errorCount = 0;
};
} // namespace Marlin
