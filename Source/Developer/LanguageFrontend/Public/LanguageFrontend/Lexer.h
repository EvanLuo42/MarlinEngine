#pragma once

#include "Containers/List.h"
#include "LanguageFrontend/Diagnostics.h"
#include "LanguageFrontend/Source.h"

namespace Marlin
{
enum class TokenKind : uint8
{
    EndOfFile,
    Identifier,
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    /// `'a'`, a Unicode scalar value (scripts).
    RuneLiteral,
    /// `$"text {hole:format}"`, one token for the whole string; the parser parses the holes.
    InterpolatedStringLiteral,

    KeywordNamespace,
    KeywordUsing,
    KeywordPublic,
    KeywordInternal,
    KeywordPrivate,
    KeywordStatic,
    KeywordConst,
    KeywordReadonly,
    KeywordStruct,
    KeywordEnum,
    KeywordInterface,
    KeywordWhere,
    KeywordIf,
    KeywordElse,
    KeywordFor,
    KeywordForeach,
    KeywordWhile,
    KeywordDo,
    KeywordSwitch,
    KeywordCase,
    KeywordDefault,
    KeywordBreak,
    KeywordContinue,
    KeywordReturn,
    KeywordTrue,
    KeywordFalse,
    KeywordNull,
    KeywordIn,
    KeywordOut,
    KeywordRef,
    KeywordNew,
    KeywordWith,
    KeywordThis,
    KeywordOperator,
    KeywordImplicit,
    KeywordExplicit,
    KeywordIs,
    KeywordAs,
    KeywordSizeof,
    KeywordTypeof,
    KeywordNameof,
    KeywordVoid,
    KeywordBool,
    KeywordInt,
    KeywordUint,
    KeywordShort,
    KeywordUshort,
    KeywordLong,
    KeywordUlong,
    KeywordFloat,
    KeywordDouble,
    KeywordHalf,

    OpenBrace,
    CloseBrace,
    OpenParen,
    CloseParen,
    OpenBracket,
    CloseBracket,
    Semicolon,
    Comma,
    Dot,
    Colon,
    Question,
    QuestionQuestion,
    QuestionQuestionAssign,
    QuestionDot,
    Arrow,
    Assign,
    Equal,
    NotEqual,
    Less,
    Greater,
    LessEqual,
    GreaterEqual,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Ampersand,
    Pipe,
    Caret,
    Tilde,
    Exclamation,
    AmpersandAmpersand,
    PipePipe,
    LessLess,
    PlusPlus,
    MinusMinus,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    AmpersandAssign,
    PipeAssign,
    CaretAssign,
    LessLessAssign,
    /// Made by the parser from adjacent `>` `>` and `>` `>=`; the lexer never produces them.
    GreaterGreater,
    GreaterGreaterAssign,
};

enum class LiteralSuffix : uint8
{
    None,
    U,
    L,
    UL,
    F,
    H,
    D,
};

/// A token. `>>` and `>>=` are never single tokens: the parser joins adjacent `>` tokens, so that `A<B<C>>` closes
/// two generic argument lists.
struct Token
{
    TokenKind Kind = TokenKind::EndOfFile;
    LiteralSuffix Suffix = LiteralSuffix::None;
    /// The identifier was written with `@`, so it is never a keyword, contextual or not; or the string was written
    /// with `@`, so `""` is a quote and `\` is not an escape.
    bool Verbatim = false;
    uint32 Begin = 0;
    uint32 End = 0;
    /// An integer literal's value, or a rune literal's scalar value.
    uint64 Integer = 0;
    /// A float literal's value.
    double Float = 0;

    [[nodiscard]] bool Is(TokenKind kind) const
    {
        return Kind == kind;
    }
};

/// The text of *token*, without the `@` of a verbatim identifier.
[[nodiscard]] LANGUAGEFRONTEND_API StringView GetTokenText(const SourceFile &file, const Token &token);
[[nodiscard]] LANGUAGEFRONTEND_API StringView GetTokenKindName(TokenKind kind);
/// How an operator token is written, such as `<<` or `+=`; empty for other tokens.
[[nodiscard]] LANGUAGEFRONTEND_API StringView GetOperatorText(TokenKind kind);
/// Whether *kind* names a built-in scalar type or `void`.
[[nodiscard]] LANGUAGEFRONTEND_API bool IsPredefinedTypeKeyword(TokenKind kind);

/// Splits *file* into tokens, skipping whitespace and comments; always ends with EndOfFile.
LANGUAGEFRONTEND_API void Tokenize(const SourceFile &file, LanguageDiagnostics &diagnostics, List<Token> &tokens);

/// The decoded value of a string literal token, escapes resolved.
LANGUAGEFRONTEND_API String DecodeStringLiteral(const SourceFile &file, const Token &token);
/// Resolves the escapes of *text*, the body of a string between its quotes, or `""` quotes if it is *verbatim*;
/// also used for the text between the holes of an interpolated string.
LANGUAGEFRONTEND_API String DecodeStringText(StringView text, bool verbatim);

/// Tokenizes the range [begin, end) of *file*, such as an interpolation hole; ends with EndOfFile at *end*, and
/// token offsets are offsets into the whole file.
LANGUAGEFRONTEND_API void TokenizeRange(const SourceFile &file, uint32 begin, uint32 end,
                                        LanguageDiagnostics &diagnostics, List<Token> &tokens);
} // namespace Marlin
