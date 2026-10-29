#include "LanguageFrontend/Lexer.h"

#include "String/Ascii.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "String/Utf.h"

namespace Marlin
{
namespace
{
struct Keyword
{
    StringView Text;
    TokenKind Kind;
};

constexpr Keyword Keywords[] = {
    {"namespace", TokenKind::KeywordNamespace},
    {"using", TokenKind::KeywordUsing},
    {"public", TokenKind::KeywordPublic},
    {"internal", TokenKind::KeywordInternal},
    {"private", TokenKind::KeywordPrivate},
    {"static", TokenKind::KeywordStatic},
    {"const", TokenKind::KeywordConst},
    {"readonly", TokenKind::KeywordReadonly},
    {"struct", TokenKind::KeywordStruct},
    {"enum", TokenKind::KeywordEnum},
    {"interface", TokenKind::KeywordInterface},
    {"where", TokenKind::KeywordWhere},
    {"if", TokenKind::KeywordIf},
    {"else", TokenKind::KeywordElse},
    {"for", TokenKind::KeywordFor},
    {"foreach", TokenKind::KeywordForeach},
    {"while", TokenKind::KeywordWhile},
    {"do", TokenKind::KeywordDo},
    {"switch", TokenKind::KeywordSwitch},
    {"case", TokenKind::KeywordCase},
    {"default", TokenKind::KeywordDefault},
    {"break", TokenKind::KeywordBreak},
    {"continue", TokenKind::KeywordContinue},
    {"return", TokenKind::KeywordReturn},
    {"true", TokenKind::KeywordTrue},
    {"false", TokenKind::KeywordFalse},
    {"null", TokenKind::KeywordNull},
    {"in", TokenKind::KeywordIn},
    {"out", TokenKind::KeywordOut},
    {"ref", TokenKind::KeywordRef},
    {"new", TokenKind::KeywordNew},
    {"with", TokenKind::KeywordWith},
    {"this", TokenKind::KeywordThis},
    {"operator", TokenKind::KeywordOperator},
    {"implicit", TokenKind::KeywordImplicit},
    {"explicit", TokenKind::KeywordExplicit},
    {"is", TokenKind::KeywordIs},
    {"as", TokenKind::KeywordAs},
    {"sizeof", TokenKind::KeywordSizeof},
    {"typeof", TokenKind::KeywordTypeof},
    {"nameof", TokenKind::KeywordNameof},
    {"void", TokenKind::KeywordVoid},
    {"bool", TokenKind::KeywordBool},
    {"int", TokenKind::KeywordInt},
    {"uint", TokenKind::KeywordUint},
    {"short", TokenKind::KeywordShort},
    {"ushort", TokenKind::KeywordUshort},
    {"long", TokenKind::KeywordLong},
    {"ulong", TokenKind::KeywordUlong},
    {"float", TokenKind::KeywordFloat},
    {"double", TokenKind::KeywordDouble},
    {"half", TokenKind::KeywordHalf},
};

constexpr usize KeywordCount = sizeof(Keywords) / sizeof(Keywords[0]);

/// The keywords by first letter, so an identifier is compared only with those that start like it: the keywords
/// starting with `'a' + i` are Order[Starts[i]] up to Order[Starts[i + 1]].
struct KeywordIndex
{
    uint8 Starts[27] = {};
    uint8 Order[KeywordCount] = {};
};

constexpr KeywordIndex MakeKeywordIndex()
{
    KeywordIndex index;
    for (const Keyword &keyword : Keywords)
    {
        ++index.Starts[keyword.Text[0] - 'a' + 1];
    }
    for (usize i = 1; i < 27; ++i)
    {
        index.Starts[i] = static_cast<uint8>(index.Starts[i] + index.Starts[i - 1]);
    }
    uint8 next[26] = {};
    for (usize i = 0; i < 26; ++i)
    {
        next[i] = index.Starts[i];
    }
    for (usize i = 0; i < KeywordCount; ++i)
    {
        index.Order[next[Keywords[i].Text[0] - 'a']++] = static_cast<uint8>(i);
    }
    return index;
}

constexpr KeywordIndex KeywordsByLetter = MakeKeywordIndex();

class Lexer
{
public:
    Lexer(const SourceFile &file, LanguageDiagnostics &diagnostics, List<Token> &tokens)
        : _file(&file), _text(file.GetText()), _diagnostics(&diagnostics), _tokens(&tokens)
    {
        if (_text.StartsWith("\xEF\xBB\xBF"))
        {
            _pos = 3;
        }
    }

    Lexer(const SourceFile &file, uint32 begin, uint32 end, LanguageDiagnostics &diagnostics, List<Token> &tokens)
        : _file(&file), _text(file.GetText().Left(end)), _diagnostics(&diagnostics), _tokens(&tokens), _pos(begin)
    {
    }

    void Run()
    {
        while (true)
        {
            SkipTrivia();
            Token token;
            token.Begin = static_cast<uint32>(_pos);
            if (_pos >= _text.Length())
            {
                token.End = token.Begin;
                _tokens->Add(token);
                return;
            }
            if (!LexToken(token))
            {
                continue;
            }
            token.End = static_cast<uint32>(_pos);
            _tokens->Add(token);
        }
    }

private:
    [[nodiscard]] char Peek(usize offset = 0) const
    {
        return _pos + offset < _text.Length() ? _text[_pos + offset] : '\0';
    }

    [[nodiscard]] SourceRange RangeFrom(usize begin) const
    {
        return {_file->GetId(), static_cast<uint32>(begin), static_cast<uint32>(_pos)};
    }

    void SkipTrivia()
    {
        while (_pos < _text.Length())
        {
            const char c = _text[_pos];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v')
            {
                ++_pos;
            }
            else if (c == '/' && Peek(1) == '/')
            {
                while (_pos < _text.Length() && _text[_pos] != '\n')
                {
                    ++_pos;
                }
            }
            else if (c == '/' && Peek(1) == '*')
            {
                const usize begin = _pos;
                _pos += 2;
                while (_pos < _text.Length() && !(_text[_pos] == '*' && Peek(1) == '/'))
                {
                    ++_pos;
                }
                if (_pos >= _text.Length())
                {
                    _diagnostics->Error(DiagnosticCode::UnterminatedComment, RangeFrom(begin), "the comment is not closed");
                    return;
                }
                _pos += 2;
            }
            else
            {
                return;
            }
        }
    }

    static bool IsIdentifierStart(char c)
    {
        return Ascii::IsLetter(c) || c == '_';
    }

    static bool IsIdentifierPart(char c)
    {
        return Ascii::IsLetterOrDigit(c) || c == '_';
    }

    bool LexToken(Token &token)
    {
        const char c = Peek();
        if (c == '@' && IsIdentifierStart(Peek(1)))
        {
            ++_pos;
            LexIdentifier(token);
            token.Kind = TokenKind::Identifier;
            token.Verbatim = true;
            return true;
        }
        if ((c == '@' && Peek(1) == '"') || (c == '$' && Peek(1) == '"') ||
            (c == '$' && Peek(1) == '@' && Peek(2) == '"') || (c == '@' && Peek(1) == '$' && Peek(2) == '"'))
        {
            const bool interpolated = c == '$' || Peek(1) == '$';
            token.Verbatim = c == '@' || Peek(1) == '@';
            _pos += interpolated && token.Verbatim ? 2 : 1;
            if (interpolated)
            {
                LexInterpolatedString(token);
            }
            else
            {
                LexString(token);
            }
            return true;
        }
        if (IsIdentifierStart(c))
        {
            LexIdentifier(token);
            return true;
        }
        if (Ascii::IsDigit(c) || (c == '.' && Ascii::IsDigit(Peek(1))))
        {
            LexNumber(token);
            return true;
        }
        if (c == '"')
        {
            LexString(token);
            return true;
        }
        if (c == '\'')
        {
            LexRune(token);
            return true;
        }
        return LexPunctuation(token);
    }

    void LexIdentifier(Token &token)
    {
        const usize begin = _pos;
        while (IsIdentifierPart(Peek()))
        {
            ++_pos;
        }
        const StringView text = _text.Substring(begin, _pos - begin);
        token.Kind = TokenKind::Identifier;
        if (!Ascii::IsLower(text[0]))
        {
            return;
        }
        const auto letter = static_cast<usize>(text[0] - 'a');
        for (usize i = KeywordsByLetter.Starts[letter]; i < KeywordsByLetter.Starts[letter + 1]; ++i)
        {
            const Keyword &keyword = Keywords[KeywordsByLetter.Order[i]];
            if (keyword.Text == text)
            {
                token.Kind = keyword.Kind;
                break;
            }
        }
    }

    void LexNumber(Token &token)
    {
        const usize begin = _pos;
        StringBuilder<64> digits;
        uint32 base = 10;
        bool isFloat = false;
        if (Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X' || Peek(1) == 'b' || Peek(1) == 'B'))
        {
            base = Peek(1) == 'x' || Peek(1) == 'X' ? 16 : 2;
            _pos += 2;
            while (Ascii::IsHexDigit(Peek()) || Peek() == '_')
            {
                if (Peek() != '_')
                {
                    digits.Append(Peek());
                }
                ++_pos;
            }
        }
        else
        {
            auto readDigits = [&] {
                while (Ascii::IsDigit(Peek()) || Peek() == '_')
                {
                    if (Peek() != '_')
                    {
                        digits.Append(Peek());
                    }
                    ++_pos;
                }
            };
            readDigits();
            if (Peek() == '.' && Ascii::IsDigit(Peek(1)))
            {
                isFloat = true;
                digits.Append('.');
                ++_pos;
                readDigits();
            }
            if ((Peek() == 'e' || Peek() == 'E') &&
                (Ascii::IsDigit(Peek(1)) || ((Peek(1) == '+' || Peek(1) == '-') && Ascii::IsDigit(Peek(2)))))
            {
                isFloat = true;
                digits.Append('e');
                ++_pos;
                if (Peek() == '+' || Peek() == '-')
                {
                    digits.Append(Peek());
                    ++_pos;
                }
                readDigits();
            }
        }

        const usize suffixBegin = _pos;
        while (Ascii::IsLetter(Peek()))
        {
            ++_pos;
        }
        const StringView suffix = _text.Substring(suffixBegin, _pos - suffixBegin);
        if (suffix.EqualsIgnoreCase("f"))
        {
            token.Suffix = LiteralSuffix::F;
        }
        else if (suffix.EqualsIgnoreCase("h"))
        {
            token.Suffix = LiteralSuffix::H;
        }
        else if (suffix.EqualsIgnoreCase("d"))
        {
            token.Suffix = LiteralSuffix::D;
        }
        else if (suffix.EqualsIgnoreCase("u"))
        {
            token.Suffix = LiteralSuffix::U;
        }
        else if (suffix.EqualsIgnoreCase("l"))
        {
            token.Suffix = LiteralSuffix::L;
        }
        else if (suffix.EqualsIgnoreCase("ul") || suffix.EqualsIgnoreCase("lu"))
        {
            token.Suffix = LiteralSuffix::UL;
        }
        else if (!suffix.IsEmpty())
        {
            _diagnostics->Error(DiagnosticCode::InvalidNumber, RangeFrom(begin), "unknown literal suffix '{0}'", suffix);
        }

        const bool floatSuffix =
            token.Suffix == LiteralSuffix::F || token.Suffix == LiteralSuffix::H || token.Suffix == LiteralSuffix::D;
        if (isFloat || (floatSuffix && base == 10))
        {
            token.Kind = TokenKind::FloatLiteral;
            if (!floatSuffix && token.Suffix != LiteralSuffix::None)
            {
                _diagnostics->Error(DiagnosticCode::InvalidNumber, RangeFrom(begin),
                                    "an integer suffix on a floating-point literal");
            }
            if (!TryParse(digits.AsView(), token.Float))
            {
                _diagnostics->Error(DiagnosticCode::InvalidNumber, RangeFrom(begin), "invalid floating-point literal");
            }
            return;
        }
        token.Kind = TokenKind::IntegerLiteral;
        if (floatSuffix)
        {
            _diagnostics->Error(DiagnosticCode::InvalidNumber, RangeFrom(begin),
                                "a floating-point suffix on a hexadecimal or binary literal");
        }
        if (digits.IsEmpty() || !TryParse(digits.AsView(), token.Integer, base))
        {
            _diagnostics->Error(DiagnosticCode::NumberOutOfRange, RangeFrom(begin),
                                "the integer literal is invalid or does not fit in 64 bits");
        }
    }

    /// Skips a string body after its opening quote, through the closing quote: `""` is a quote in a verbatim
    /// string, which may span lines; a regular one has `\` escapes and ends at the line.
    bool SkipStringBody(bool verbatim)
    {
        while (_pos < _text.Length())
        {
            const char c = _text[_pos];
            if (c == '"')
            {
                if (verbatim && Peek(1) == '"')
                {
                    _pos += 2;
                    continue;
                }
                ++_pos;
                return true;
            }
            if (c == '\n' && !verbatim)
            {
                return false;
            }
            _pos += c == '\\' && !verbatim && _pos + 1 < _text.Length() ? 2 : 1;
        }
        return false;
    }

    void LexString(Token &token)
    {
        const usize begin = token.Begin;
        ++_pos;
        if (!SkipStringBody(token.Verbatim))
        {
            _diagnostics->Error(DiagnosticCode::UnterminatedString, RangeFrom(begin), "the string is not closed");
        }
        token.Kind = TokenKind::StringLiteral;
    }

    /// `$"text {hole} text"`, `$@"…"` or `@$"…"`, lexed as one token after its prefix; the parser splits it. `{{` and
    /// `}}` are braces in the text, and a hole runs to its closing `}`.
    void LexInterpolatedString(Token &token)
    {
        const usize begin = token.Begin;
        ++_pos;
        token.Kind = TokenKind::InterpolatedStringLiteral;
        while (_pos < _text.Length())
        {
            const char c = _text[_pos];
            if (c == '"')
            {
                if (token.Verbatim && Peek(1) == '"')
                {
                    _pos += 2;
                    continue;
                }
                ++_pos;
                return;
            }
            if ((c == '{' || c == '}') && Peek(1) == c)
            {
                _pos += 2;
                continue;
            }
            if (c == '{')
            {
                ++_pos;
                SkipHole();
                continue;
            }
            if (c == '\n' && !token.Verbatim)
            {
                break;
            }
            _pos += c == '\\' && !token.Verbatim && _pos + 1 < _text.Length() ? 2 : 1;
        }
        _diagnostics->Error(DiagnosticCode::UnterminatedString, RangeFrom(begin), "the string is not closed");
    }

    /// Skips an interpolation hole after its `{`, through its closing `}`: brackets nest, strings and runes inside it
    /// are skipped whole, and a `:` outside brackets starts the format, which runs to the `}`.
    void SkipHole()
    {
        int32 depth = 0;
        while (_pos < _text.Length())
        {
            const char c = _text[_pos];
            const bool verbatim = c == '@' && (Peek(1) == '"' || (Peek(1) == '$' && Peek(2) == '"'));
            const bool interpolated = (c == '$' && (Peek(1) == '"' || (Peek(1) == '@' && Peek(2) == '"'))) ||
                                      (c == '@' && Peek(1) == '$' && Peek(2) == '"');
            if (c == '"' || verbatim || interpolated)
            {
                Token nested;
                nested.Begin = static_cast<uint32>(_pos);
                nested.Verbatim = verbatim || (interpolated && (Peek(1) == '@' || c == '@'));
                _pos += c == '"' ? 0 : (interpolated && nested.Verbatim ? 2 : 1);
                if (interpolated)
                {
                    LexInterpolatedString(nested);
                }
                else
                {
                    ++_pos;
                    SkipStringBody(nested.Verbatim);
                }
                continue;
            }
            if (c == '\'')
            {
                ++_pos;
                while (_pos < _text.Length() && _text[_pos] != '\'' && _text[_pos] != '\n')
                {
                    _pos += _text[_pos] == '\\' && _pos + 1 < _text.Length() ? 2 : 1;
                }
                _pos += Peek() == '\'' ? 1 : 0;
                continue;
            }
            if (c == '(' || c == '[' || c == '{')
            {
                ++depth;
            }
            else if ((c == ')' || c == ']') && depth > 0)
            {
                --depth;
            }
            else if (c == '}')
            {
                ++_pos;
                if (depth == 0)
                {
                    return;
                }
                --depth;
                continue;
            }
            else if (c == ':' && depth == 0)
            {
                while (_pos < _text.Length() && _text[_pos] != '}' && _text[_pos] != '"')
                {
                    ++_pos;
                }
                _pos += Peek() == '}' ? 1 : 0;
                return;
            }
            ++_pos;
        }
    }

    /// `'a'`, `'\n'` or `'\u00E9'`: exactly one Unicode scalar value, kept in Integer.
    void LexRune(Token &token)
    {
        const usize begin = _pos;
        ++_pos;
        while (_pos < _text.Length() && _text[_pos] != '\'' && _text[_pos] != '\n')
        {
            _pos += _text[_pos] == '\\' && _pos + 1 < _text.Length() ? 2 : 1;
        }
        token.Kind = TokenKind::RuneLiteral;
        if (Peek() != '\'')
        {
            _diagnostics->Error(DiagnosticCode::UnterminatedString, RangeFrom(begin), "the rune literal is not closed");
            return;
        }
        const String value = DecodeStringText(_text.Substring(begin + 1, _pos - begin - 1), false);
        ++_pos;
        const char *cursor = value.Data();
        const char *end = cursor + value.Length();
        const char32_t codePoint = cursor != end ? Utf8::Decode(cursor, end) : Utf8::ReplacementCharacter;
        if (value.Length() == 0 || cursor != end)
        {
            _diagnostics->Error(DiagnosticCode::InvalidEscape, RangeFrom(begin),
                                "a rune literal holds exactly one Unicode scalar value");
        }
        token.Integer = codePoint;
    }

    /// Makes *token* the *length*-character punctuation *kind*.
    bool Punctuation(Token &token, TokenKind kind, usize length)
    {
        token.Kind = kind;
        _pos += length;
        return true;
    }

    /// A punctuation token by its first characters, the longest that matches.
    bool LexPunctuation(Token &token)
    {
        const char second = Peek(1);
        switch (Peek())
        {
        case '{':
            return Punctuation(token, TokenKind::OpenBrace, 1);
        case '}':
            return Punctuation(token, TokenKind::CloseBrace, 1);
        case '(':
            return Punctuation(token, TokenKind::OpenParen, 1);
        case ')':
            return Punctuation(token, TokenKind::CloseParen, 1);
        case '[':
            return Punctuation(token, TokenKind::OpenBracket, 1);
        case ']':
            return Punctuation(token, TokenKind::CloseBracket, 1);
        case ';':
            return Punctuation(token, TokenKind::Semicolon, 1);
        case ',':
            return Punctuation(token, TokenKind::Comma, 1);
        case '.':
            return Punctuation(token, TokenKind::Dot, 1);
        case ':':
            return Punctuation(token, TokenKind::Colon, 1);
        case '~':
            return Punctuation(token, TokenKind::Tilde, 1);
        case '?':
            if (second == '?')
            {
                return Peek(2) == '=' ? Punctuation(token, TokenKind::QuestionQuestionAssign, 3)
                                      : Punctuation(token, TokenKind::QuestionQuestion, 2);
            }
            // `?.` is null-conditional access, but `c ? .5 : 1` is a conditional with a number.
            if (second == '.' && !Ascii::IsDigit(Peek(2)))
            {
                return Punctuation(token, TokenKind::QuestionDot, 2);
            }
            return Punctuation(token, TokenKind::Question, 1);
        case '=':
            return second == '>'   ? Punctuation(token, TokenKind::Arrow, 2)
                   : second == '=' ? Punctuation(token, TokenKind::Equal, 2)
                                   : Punctuation(token, TokenKind::Assign, 1);
        case '!':
            return second == '=' ? Punctuation(token, TokenKind::NotEqual, 2)
                                 : Punctuation(token, TokenKind::Exclamation, 1);
        case '<':
            if (second == '<')
            {
                return Peek(2) == '=' ? Punctuation(token, TokenKind::LessLessAssign, 3)
                                      : Punctuation(token, TokenKind::LessLess, 2);
            }
            return second == '=' ? Punctuation(token, TokenKind::LessEqual, 2) : Punctuation(token, TokenKind::Less, 1);
        case '>':
            return second == '=' ? Punctuation(token, TokenKind::GreaterEqual, 2)
                                 : Punctuation(token, TokenKind::Greater, 1);
        case '&':
            return second == '&'   ? Punctuation(token, TokenKind::AmpersandAmpersand, 2)
                   : second == '=' ? Punctuation(token, TokenKind::AmpersandAssign, 2)
                                   : Punctuation(token, TokenKind::Ampersand, 1);
        case '|':
            return second == '|'   ? Punctuation(token, TokenKind::PipePipe, 2)
                   : second == '=' ? Punctuation(token, TokenKind::PipeAssign, 2)
                                   : Punctuation(token, TokenKind::Pipe, 1);
        case '+':
            return second == '+'   ? Punctuation(token, TokenKind::PlusPlus, 2)
                   : second == '=' ? Punctuation(token, TokenKind::PlusAssign, 2)
                                   : Punctuation(token, TokenKind::Plus, 1);
        case '-':
            return second == '-'   ? Punctuation(token, TokenKind::MinusMinus, 2)
                   : second == '=' ? Punctuation(token, TokenKind::MinusAssign, 2)
                                   : Punctuation(token, TokenKind::Minus, 1);
        case '*':
            return second == '=' ? Punctuation(token, TokenKind::StarAssign, 2)
                                 : Punctuation(token, TokenKind::Star, 1);
        case '/':
            return second == '=' ? Punctuation(token, TokenKind::SlashAssign, 2)
                                 : Punctuation(token, TokenKind::Slash, 1);
        case '%':
            return second == '=' ? Punctuation(token, TokenKind::PercentAssign, 2)
                                 : Punctuation(token, TokenKind::Percent, 1);
        case '^':
            return second == '=' ? Punctuation(token, TokenKind::CaretAssign, 2)
                                 : Punctuation(token, TokenKind::Caret, 1);
        default:
            break;
        }
        const usize begin = _pos;
        ++_pos;
        while (_pos < _text.Length() && (static_cast<uint8>(_text[_pos]) & 0xC0) == 0x80)
        {
            ++_pos;
        }
        _diagnostics->Error(DiagnosticCode::UnexpectedCharacter, RangeFrom(begin), "unexpected character '{0}'",
                            _text.Substring(begin, _pos - begin));
        return false;
    }

    const SourceFile *_file;
    StringView _text;
    LanguageDiagnostics *_diagnostics;
    List<Token> *_tokens;
    usize _pos = 0;
};
} // namespace

StringView GetTokenText(const SourceFile &file, const Token &token)
{
    const StringView text = file.GetText().Substring(token.Begin, token.End - token.Begin);
    return token.Verbatim && token.Kind == TokenKind::Identifier ? text.RemoveStart(1) : text;
}

StringView GetTokenKindName(TokenKind kind)
{
    switch (kind)
    {
    case TokenKind::EndOfFile:
        return "end of file";
    case TokenKind::Identifier:
        return "identifier";
    case TokenKind::IntegerLiteral:
        return "integer literal";
    case TokenKind::FloatLiteral:
        return "floating-point literal";
    case TokenKind::StringLiteral:
        return "string literal";
    case TokenKind::RuneLiteral:
        return "rune literal";
    case TokenKind::InterpolatedStringLiteral:
        return "interpolated string";
    default:
        break;
    }
    for (const Keyword &keyword : Keywords)
    {
        if (keyword.Kind == kind)
        {
            return keyword.Text;
        }
    }
    switch (kind)
    {
    case TokenKind::OpenBrace:
        return "'{'";
    case TokenKind::CloseBrace:
        return "'}'";
    case TokenKind::OpenParen:
        return "'('";
    case TokenKind::CloseParen:
        return "')'";
    case TokenKind::OpenBracket:
        return "'['";
    case TokenKind::CloseBracket:
        return "']'";
    case TokenKind::Semicolon:
        return "';'";
    case TokenKind::Comma:
        return "','";
    case TokenKind::Dot:
        return "'.'";
    case TokenKind::Colon:
        return "':'";
    case TokenKind::Assign:
        return "'='";
    case TokenKind::Less:
        return "'<'";
    case TokenKind::Greater:
        return "'>'";
    case TokenKind::Arrow:
        return "'=>'";
    case TokenKind::Question:
        return "'?'";
    case TokenKind::QuestionQuestion:
        return "'?\?'";
    case TokenKind::QuestionDot:
        return "'?.'";
    default:
        return "operator";
    }
}

StringView GetOperatorText(TokenKind kind)
{
    switch (kind)
    {
    case TokenKind::Equal:
        return "==";
    case TokenKind::NotEqual:
        return "!=";
    case TokenKind::Less:
        return "<";
    case TokenKind::Greater:
        return ">";
    case TokenKind::LessEqual:
        return "<=";
    case TokenKind::GreaterEqual:
        return ">=";
    case TokenKind::Plus:
        return "+";
    case TokenKind::Minus:
        return "-";
    case TokenKind::Star:
        return "*";
    case TokenKind::Slash:
        return "/";
    case TokenKind::Percent:
        return "%";
    case TokenKind::Ampersand:
        return "&";
    case TokenKind::Pipe:
        return "|";
    case TokenKind::Caret:
        return "^";
    case TokenKind::Tilde:
        return "~";
    case TokenKind::Exclamation:
        return "!";
    case TokenKind::AmpersandAmpersand:
        return "&&";
    case TokenKind::PipePipe:
        return "||";
    case TokenKind::LessLess:
        return "<<";
    case TokenKind::GreaterGreater:
        return ">>";
    case TokenKind::PlusPlus:
        return "++";
    case TokenKind::MinusMinus:
        return "--";
    case TokenKind::PlusAssign:
        return "+=";
    case TokenKind::MinusAssign:
        return "-=";
    case TokenKind::StarAssign:
        return "*=";
    case TokenKind::SlashAssign:
        return "/=";
    case TokenKind::PercentAssign:
        return "%=";
    case TokenKind::AmpersandAssign:
        return "&=";
    case TokenKind::PipeAssign:
        return "|=";
    case TokenKind::CaretAssign:
        return "^=";
    case TokenKind::LessLessAssign:
        return "<<=";
    case TokenKind::GreaterGreaterAssign:
        return ">>=";
    default:
        return {};
    }
}

bool IsPredefinedTypeKeyword(TokenKind kind)
{
    return kind >= TokenKind::KeywordVoid && kind <= TokenKind::KeywordHalf;
}

void Tokenize(const SourceFile &file, LanguageDiagnostics &diagnostics, List<Token> &tokens)
{
    Lexer(file, diagnostics, tokens).Run();
}

void TokenizeRange(const SourceFile &file, uint32 begin, uint32 end, LanguageDiagnostics &diagnostics,
                   List<Token> &tokens)
{
    Lexer(file, begin, end, diagnostics, tokens).Run();
}

String DecodeStringLiteral(const SourceFile &file, const Token &token)
{
    StringView text = file.GetText().Substring(token.Begin, token.End - token.Begin);
    if (token.Verbatim)
    {
        text = text.RemoveStart(1);
    }
    const StringView body = text.Length() >= 2 && text.EndsWith('"') ? text.Substring(1, text.Length() - 2)
                                                                       : text.RemoveStart(1);
    return DecodeStringText(body, token.Verbatim);
}

String DecodeStringText(StringView body, bool verbatim)
{
    StringBuilder<128> output;
    for (usize i = 0; i < body.Length(); ++i)
    {
        if (verbatim)
        {
            output.Append(body[i]);
            if (body[i] == '"' && i + 1 < body.Length() && body[i + 1] == '"')
            {
                ++i;
            }
            continue;
        }
        if (body[i] != '\\' || i + 1 >= body.Length())
        {
            output.Append(body[i]);
            continue;
        }
        const char escape = body[++i];
        switch (escape)
        {
        case 'n':
            output.Append('\n');
            break;
        case 't':
            output.Append('\t');
            break;
        case 'r':
            output.Append('\r');
            break;
        case '0':
            output.Append('\0');
            break;
        case 'u':
        {
            uint32 codePoint = 0;
            if (i + 4 < body.Length() && TryParse(body.Substring(i + 1, 4), codePoint, 16))
            {
                char encoded[4];
                output.Append(StringView(encoded, Utf8::Encode(static_cast<char32_t>(codePoint), encoded)));
                i += 4;
            }
            break;
        }
        default:
            output.Append(escape);
            break;
        }
    }
    return output.ToString();
}
} // namespace Marlin
