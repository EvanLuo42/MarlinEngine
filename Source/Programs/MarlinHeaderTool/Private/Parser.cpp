#include "Parser.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Marlin::HeaderTool
{
namespace
{
enum class TokenKind : std::uint8_t
{
    Identifier,
    Number,
    String,
    Char,
    Punct,
};

struct Token
{
    TokenKind Kind;
    std::string_view Text;
    int Line;
    size_t Begin;
    size_t End;
    std::string Doc;
};

bool IsIdentifierStart(char c)
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool IsIdentifierChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

/// *doc* without its Markdown: `*name*` and `` `code` `` lose their marks, and `[text](target)` and `` [`Name`] ``
/// keep only their text.
std::string StripMarkdown(std::string_view doc)
{
    std::string result;
    result.reserve(doc.size());
    for (size_t i = 0; i < doc.size(); ++i)
    {
        const char c = doc[i];
        if (c == '`')
        {
            continue;
        }
        if (c == '*')
        {
            const bool opens = i + 1 < doc.size() && doc[i + 1] != ' ' && doc[i + 1] != '*' &&
                               (i == 0 || !IsIdentifierChar(doc[i - 1]));
            const bool closes = i > 0 && doc[i - 1] != ' ' && (i + 1 == doc.size() || !IsIdentifierChar(doc[i + 1]));
            if (opens || closes)
            {
                continue;
            }
        }
        if (c == '[')
        {
            const size_t close = doc.find(']', i + 1);
            if (close != std::string_view::npos)
            {
                const std::string_view text = doc.substr(i + 1, close - i - 1);
                const bool link = close + 1 < doc.size() && doc[close + 1] == '(';
                if (link || (text.size() >= 2 && text.front() == '`' && text.back() == '`'))
                {
                    result += StripMarkdown(text);
                    i = close;
                    if (link)
                    {
                        const size_t end = doc.find(')', close + 1);
                        i = end != std::string_view::npos ? end : doc.size();
                    }
                    continue;
                }
            }
        }
        result += c;
    }
    return result;
}

class Lexer
{
public:
    Lexer(std::string_view text, ParsedFile &file) : _text(text), _file(&file)
    {
    }

    bool Run(std::vector<Token> &tokens, std::string &error, int &errorLine)
    {
        bool lineStart = true;
        // The first paragraph of the `///` comment before the next token; the rest is for MarlinDoc
        // (Documentation/Architecture/16 §1.1).
        std::string doc;
        bool docEnded = false;
        while (_pos < _text.size())
        {
            const char c = _text[_pos];
            if (c == '\n')
            {
                ++_line;
                ++_pos;
                lineStart = true;
                continue;
            }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v')
            {
                ++_pos;
                continue;
            }
            if (c == '#' && lineStart)
            {
                Preprocessor();
                continue;
            }
            lineStart = false;
            if (c == '/' && Peek(1) == '/')
            {
                const size_t start = _pos;
                while (_pos < _text.size() && _text[_pos] != '\n')
                {
                    ++_pos;
                }
                const std::string_view comment = _text.substr(start, _pos - start);
                if (comment.starts_with("///") && !comment.starts_with("////"))
                {
                    std::string_view line = comment.substr(3);
                    if (line.starts_with(' '))
                    {
                        line.remove_prefix(1);
                    }
                    while (line.ends_with('\r') || line.ends_with(' '))
                    {
                        line.remove_suffix(1);
                    }
                    if (line.empty())
                    {
                        docEnded = !doc.empty();
                    }
                    else if (!docEnded)
                    {
                        if (!doc.empty())
                        {
                            doc += ' ';
                        }
                        doc += line;
                    }
                }
                continue;
            }
            if (c == '/' && Peek(1) == '*')
            {
                const size_t end = _text.find("*/", _pos + 2);
                if (end == std::string_view::npos)
                {
                    error = "unterminated comment";
                    errorLine = _line;
                    return false;
                }
                for (size_t i = _pos; i < end; ++i)
                {
                    _line += _text[i] == '\n' ? 1 : 0;
                }
                _pos = end + 2;
                continue;
            }

            Token token{TokenKind::Punct, {}, _line, _pos, _pos, StripMarkdown(doc)};
            doc.clear();
            docEnded = false;
            if (IsIdentifierStart(c))
            {
                size_t end = _pos;
                while (end < _text.size() && IsIdentifierChar(_text[end]))
                {
                    ++end;
                }
                const std::string_view word = _text.substr(_pos, end - _pos);
                if (end < _text.size() && _text[end] == '"' &&
                    (word == "R" || word == "u8R" || word == "LR" || word == "uR" || word == "UR"))
                {
                    _pos = end;
                    if (!RawString(error))
                    {
                        errorLine = token.Line;
                        return false;
                    }
                    token.Kind = TokenKind::String;
                }
                else
                {
                    _pos = end;
                    token.Kind = TokenKind::Identifier;
                }
            }
            else if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
                     (c == '.' && std::isdigit(static_cast<unsigned char>(Peek(1))) != 0))
            {
                while (_pos < _text.size())
                {
                    const char d = _text[_pos];
                    const char previous = _text[_pos - 1];
                    if (IsIdentifierChar(d) || d == '.' || d == '\'' ||
                        ((d == '+' || d == '-') &&
                         (previous == 'e' || previous == 'E' || previous == 'p' || previous == 'P')))
                    {
                        ++_pos;
                        continue;
                    }
                    break;
                }
                token.Kind = TokenKind::Number;
            }
            else if (c == '"' || c == '\'')
            {
                if (!Quoted(c))
                {
                    error = c == '"' ? "unterminated string" : "unterminated character literal";
                    errorLine = token.Line;
                    return false;
                }
                token.Kind = c == '"' ? TokenKind::String : TokenKind::Char;
            }
            else
            {
                static constexpr std::string_view Multi[] = {
                    "::", "->", "...", "==", "!=", "<=", ">=", "&&", "||", "+=", "-=", "|=", "&=",
                };
                size_t length = 1;
                for (const std::string_view op : Multi)
                {
                    if (_text.substr(_pos, op.size()) == op)
                    {
                        length = op.size();
                        break;
                    }
                }
                _pos += length;
            }
            token.End = _pos;
            token.Text = _text.substr(token.Begin, token.End - token.Begin);
            tokens.push_back(std::move(token));
        }
        return true;
    }

private:
    char Peek(size_t offset) const
    {
        return _pos + offset < _text.size() ? _text[_pos + offset] : '\0';
    }

    void Preprocessor()
    {
        const size_t start = _pos;
        const int line = _line;
        while (_pos < _text.size())
        {
            if (_text[_pos] == '\\' && Peek(1) == '\n')
            {
                _pos += 2;
                ++_line;
                continue;
            }
            if (_text[_pos] == '\\' && Peek(1) == '\r' && Peek(2) == '\n')
            {
                _pos += 3;
                ++_line;
                continue;
            }
            if (_text[_pos] == '\n')
            {
                break;
            }
            ++_pos;
        }
        std::string_view directive = _text.substr(start + 1, _pos - start - 1);
        while (!directive.empty() && (directive.front() == ' ' || directive.front() == '\t'))
        {
            directive.remove_prefix(1);
        }
        if (!directive.starts_with("include"))
        {
            return;
        }
        const size_t open = directive.find('"');
        const size_t close = open == std::string_view::npos ? open : directive.find('"', open + 1);
        if (close != std::string_view::npos)
        {
            _file->Includes.emplace_back(std::string(directive.substr(open + 1, close - open - 1)), line);
        }
        else
        {
            _file->Includes.emplace_back(std::string(), line);
        }
    }

    bool Quoted(char quote)
    {
        ++_pos;
        while (_pos < _text.size())
        {
            const char c = _text[_pos];
            if (c == '\\')
            {
                _pos += 2;
                continue;
            }
            if (c == '\n')
            {
                return false;
            }
            ++_pos;
            if (c == quote)
            {
                return true;
            }
        }
        return false;
    }

    bool RawString(std::string &error)
    {
        const size_t open = _text.find('(', _pos);
        if (open == std::string_view::npos)
        {
            error = "malformed raw string";
            return false;
        }
        const std::string terminator = ")" + std::string(_text.substr(_pos + 1, open - _pos - 1)) + "\"";
        const size_t close = _text.find(terminator, open);
        if (close == std::string_view::npos)
        {
            error = "unterminated raw string";
            return false;
        }
        for (size_t i = _pos; i < close; ++i)
        {
            _line += _text[i] == '\n' ? 1 : 0;
        }
        _pos = close + terminator.size();
        return true;
    }

    std::string_view _text;
    ParsedFile *_file;
    size_t _pos = 0;
    int _line = 1;
};

bool IsRecordMarker(std::string_view text)
{
    return text == "MSTRUCT" || text == "MCLASS" || text == "MINTERFACE";
}

bool IsMemberMarker(std::string_view text)
{
    return text == "MPROPERTY" || text == "MFUNCTION" || text == "MGENERATED_BODY";
}

bool IsBuiltinTypeWord(std::string_view text)
{
    static constexpr std::string_view Words[] = {
        "int", "float", "double", "bool", "char", "unsigned", "signed", "long", "short", "auto", "void",
    };
    for (const std::string_view word : Words)
    {
        if (text == word)
        {
            return true;
        }
    }
    return false;
}

std::string Unescape(std::string_view literal)
{
    std::string result;
    for (size_t i = 1; i + 1 < literal.size(); ++i)
    {
        char c = literal[i];
        if (c == '\\' && i + 2 < literal.size())
        {
            const char next = literal[++i];
            c = next == 'n' ? '\n' : next == 't' ? '\t' : next;
        }
        result += c;
    }
    return result;
}

class Parser
{
public:
    Parser(const std::string &path, std::string_view text, std::vector<Token> &tokens, ParsedFile &file,
           std::vector<Diagnostic> &errors)
        : _path(&path), _text(text), _tokens(&tokens), _file(&file), _errors(&errors)
    {
    }

    bool Run()
    {
        while (_index < _tokens->size())
        {
            const Token &token = (*_tokens)[_index];
            const std::string_view text = token.Text;
            if (token.Kind == TokenKind::Identifier && text == "namespace")
            {
                if (!Namespace())
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && text == "extern" && Is(_index + 1, TokenKind::String) &&
                     IsPunct(_index + 2, "{"))
            {
                _scopes.push_back({});
                _scopes.back().Transparent = true;
                _index += 3;
            }
            else if (IsPunct(_index, "{"))
            {
                if (!SkipBlock("reflected declarations must be at namespace scope"))
                {
                    return false;
                }
            }
            else if (IsPunct(_index, "}"))
            {
                if (!_scopes.empty())
                {
                    _scopes.pop_back();
                }
                ++_index;
            }
            else if (token.Kind == TokenKind::Identifier && IsRecordMarker(text))
            {
                if (!Record())
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && text == "MENUM")
            {
                if (!Enum())
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && text == "MARLIN_UPGRADE")
            {
                if (!Upgrade())
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && text == "MARLIN_TEST_SOURCE")
            {
                if (!TestSource())
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && (text == "MARLIN_TEST" || text == "MARLIN_TEST_F"))
            {
                if (!Test(text == "MARLIN_TEST_F"))
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && IsMemberMarker(text))
            {
                return Fail(token.Line, std::string(text) + " outside a reflected struct or class");
            }
            else
            {
                ++_index;
            }
        }
        return true;
    }

private:
    struct Scope
    {
        std::vector<std::string> Names;
        bool Transparent = false;
        bool Anonymous = false;
    };

    bool Fail(int line, std::string message)
    {
        _errors->push_back({*_path, line, std::move(message)});
        return false;
    }

    [[nodiscard]] bool Is(size_t index, TokenKind kind) const
    {
        return index < _tokens->size() && (*_tokens)[index].Kind == kind;
    }

    [[nodiscard]] bool IsPunct(size_t index, std::string_view text) const
    {
        return index < _tokens->size() && (*_tokens)[index].Kind == TokenKind::Punct && (*_tokens)[index].Text == text;
    }

    [[nodiscard]] bool IsWord(size_t index, std::string_view text) const
    {
        return index < _tokens->size() && (*_tokens)[index].Kind == TokenKind::Identifier &&
               (*_tokens)[index].Text == text;
    }

    [[nodiscard]] int LineAt(size_t index) const
    {
        return index < _tokens->size() ? (*_tokens)[index].Line : (_tokens->empty() ? 1 : _tokens->back().Line);
    }

    [[nodiscard]] std::string Source(size_t first, size_t last) const
    {
        if (first >= last)
        {
            return {};
        }
        return std::string(_text.substr((*_tokens)[first].Begin, (*_tokens)[last - 1].End - (*_tokens)[first].Begin));
    }

    /// The namespaces around the cursor, or false inside an anonymous one.
    bool CurrentNamespaces(std::vector<std::string> &names, int line)
    {
        for (const Scope &scope : _scopes)
        {
            if (scope.Anonymous)
            {
                return Fail(line, "reflected declarations cannot be in an anonymous namespace");
            }
            names.insert(names.end(), scope.Names.begin(), scope.Names.end());
        }
        return true;
    }

    bool Namespace()
    {
        ++_index;
        Scope scope;
        while (Is(_index, TokenKind::Identifier) || IsPunct(_index, "::"))
        {
            if (Is(_index, TokenKind::Identifier) && (*_tokens)[_index].Text != "inline")
            {
                scope.Names.emplace_back((*_tokens)[_index].Text);
            }
            ++_index;
        }
        if (IsPunct(_index, ";"))
        {
            ++_index;
            return true;
        }
        if (IsPunct(_index, "="))
        {
            while (_index < _tokens->size() && !IsPunct(_index, ";"))
            {
                ++_index;
            }
            ++_index;
            return true;
        }
        if (!IsPunct(_index, "{"))
        {
            return Fail(LineAt(_index), "expected '{' after namespace");
        }
        scope.Anonymous = scope.Names.empty();
        _scopes.push_back(std::move(scope));
        ++_index;
        return true;
    }

    /// Skips the block opening at the cursor; markup inside it is the error *message*.
    bool SkipBlock(const char *message)
    {
        int depth = 0;
        while (_index < _tokens->size())
        {
            const Token &token = (*_tokens)[_index];
            if (token.Kind == TokenKind::Punct && token.Text == "{")
            {
                ++depth;
            }
            else if (token.Kind == TokenKind::Punct && token.Text == "}")
            {
                if (--depth == 0)
                {
                    ++_index;
                    return true;
                }
            }
            else if (token.Kind == TokenKind::Identifier &&
                     (IsRecordMarker(token.Text) || IsMemberMarker(token.Text) || token.Text == "MENUM"))
            {
                return Fail(token.Line, std::string(token.Text) + ": " + message);
            }
            ++_index;
        }
        return Fail(LineAt(_index), "unbalanced braces");
    }

    /// Finds the bracket that closes the one at *open*.
    bool MatchingClose(size_t open, size_t &close)
    {
        int depth = 0;
        for (size_t i = open; i < _tokens->size(); ++i)
        {
            if ((*_tokens)[i].Kind != TokenKind::Punct)
            {
                continue;
            }
            const std::string_view text = (*_tokens)[i].Text;
            if (text == "(" || text == "[" || text == "{")
            {
                ++depth;
            }
            else if ((text == ")" || text == "]" || text == "}") && --depth == 0)
            {
                close = i;
                return true;
            }
        }
        return Fail((*_tokens)[open].Line, "unbalanced brackets");
    }

    /// Splits [first, last) at commas outside brackets; angle brackets count only when *angles* is set.
    std::vector<std::pair<size_t, size_t>> SplitTopLevel(size_t first, size_t last, bool angles) const
    {
        std::vector<std::pair<size_t, size_t>> parts;
        int depth = 0;
        int angle = 0;
        size_t start = first;
        for (size_t i = first; i < last; ++i)
        {
            const Token &token = (*_tokens)[i];
            if (token.Kind != TokenKind::Punct)
            {
                continue;
            }
            const std::string_view text = token.Text;
            if (text == "(" || text == "[" || text == "{")
            {
                ++depth;
            }
            else if (text == ")" || text == "]" || text == "}")
            {
                --depth;
            }
            else if (angles && depth == 0 && text == "<")
            {
                ++angle;
            }
            else if (angles && depth == 0 && text == ">" && angle > 0)
            {
                --angle;
            }
            else if (text == "," && depth == 0 && angle == 0)
            {
                parts.emplace_back(start, i);
                start = i + 1;
            }
        }
        if (start < last)
        {
            parts.emplace_back(start, last);
        }
        return parts;
    }

    /// Reads `(Key, Key=Value, ...)` after a marker at the cursor and moves past it.
    bool Metadata(std::vector<MetadataItem> &items)
    {
        const Token &marker = (*_tokens)[_index];
        ++_index;
        size_t close = 0;
        if (!IsPunct(_index, "(") || !MatchingClose(_index, close))
        {
            return Fail(marker.Line, "expected '(' after " + std::string(marker.Text));
        }
        for (const auto &[first, last] : SplitTopLevel(_index + 1, close, false))
        {
            if (!Is(first, TokenKind::Identifier))
            {
                return Fail((*_tokens)[first].Line, "metadata must be Key or Key=Value");
            }
            MetadataItem item{std::string((*_tokens)[first].Text), {}};
            if (last - first > 1)
            {
                if (!IsPunct(first + 1, "=") || last - first < 3)
                {
                    return Fail((*_tokens)[first].Line, "metadata must be Key or Key=Value");
                }
                item.Value =
                    last - first == 3 && Is(first + 2, TokenKind::String) && (*_tokens)[first + 2].Text[0] == '"'
                        ? Unescape((*_tokens)[first + 2].Text)
                        : Source(first + 2, last);
            }
            for (const MetadataItem &existing : items)
            {
                if (existing.Key == item.Key)
                {
                    return Fail((*_tokens)[first].Line, "metadata key " + item.Key + " is given twice");
                }
            }
            items.push_back(std::move(item));
        }
        if (!marker.Doc.empty())
        {
            bool hasTooltip = false;
            for (const MetadataItem &item : items)
            {
                hasTooltip = hasTooltip || item.Key == "Tooltip";
            }
            if (!hasTooltip)
            {
                items.push_back({"Tooltip", marker.Doc});
            }
        }
        _index = close + 1;
        return true;
    }

    void SkipAttributes()
    {
        while (true)
        {
            size_t close = 0;
            if ((IsPunct(_index, "[") && IsPunct(_index + 1, "[") && MatchingClose(_index, close)) ||
                (IsWord(_index, "alignas") && IsPunct(_index + 1, "(") && MatchingClose(_index + 1, close)))
            {
                _index = close + 1;
            }
            else
            {
                return;
            }
        }
    }

    bool Record()
    {
        RecordDecl record;
        const std::string_view marker = (*_tokens)[_index].Text;
        record.Kind = marker == "MCLASS"       ? RecordKind::Class
                      : marker == "MINTERFACE" ? RecordKind::Interface
                                               : RecordKind::Struct;
        record.Line = (*_tokens)[_index].Line;
        if (!Metadata(record.Metadata) || !CurrentNamespaces(record.Namespaces, record.Line))
        {
            return false;
        }
        if (IsWord(_index, "template"))
        {
            return Fail(LineAt(_index), "class templates cannot be reflected");
        }
        if (!IsWord(_index, "struct") && !IsWord(_index, "class"))
        {
            return Fail(LineAt(_index), std::string(marker) + " must be followed by a struct or class");
        }
        record.IsClassKey = (*_tokens)[_index].Text == "class";
        ++_index;
        SkipAttributes();
        if (Is(_index, TokenKind::Identifier) && Is(_index + 1, TokenKind::Identifier) &&
            (*_tokens)[_index].Text.ends_with("_API"))
        {
            record.Api = std::string((*_tokens)[_index].Text);
            ++_index;
        }
        if (!Is(_index, TokenKind::Identifier))
        {
            return Fail(LineAt(_index), "expected the type name");
        }
        record.Name = std::string((*_tokens)[_index].Text);
        ++_index;
        if (IsWord(_index, "final"))
        {
            ++_index;
        }
        if (IsPunct(_index, ":"))
        {
            const size_t first = ++_index;
            while (_index < _tokens->size() && !IsPunct(_index, "{") && !IsPunct(_index, ";"))
            {
                ++_index;
            }
            for (auto [begin, end] : SplitTopLevel(first, _index, true))
            {
                while (begin < end && (IsWord(begin, "public") || IsWord(begin, "protected") ||
                                       IsWord(begin, "private") || IsWord(begin, "virtual")))
                {
                    if (IsWord(begin, "virtual"))
                    {
                        return Fail((*_tokens)[begin].Line, record.Name + ": virtual bases cannot be reflected");
                    }
                    ++begin;
                }
                record.Bases.push_back(Source(begin, end));
            }
        }
        if (!IsPunct(_index, "{"))
        {
            return Fail(LineAt(_index), "expected the body of " + record.Name);
        }
        size_t close = 0;
        if (!MatchingClose(_index, close))
        {
            return false;
        }
        ++_index;
        if (!IsWord(_index, "MGENERATED_BODY") || !IsPunct(_index + 1, "(") || !IsPunct(_index + 2, ")"))
        {
            return Fail(LineAt(_index), "MGENERATED_BODY() must open the body of " + record.Name);
        }
        record.BodyLine = (*_tokens)[_index].Line;
        _index += 3;

        while (_index < close)
        {
            const Token &token = (*_tokens)[_index];
            if (token.Kind == TokenKind::Identifier && token.Text == "MPROPERTY")
            {
                if (record.Kind == RecordKind::Interface)
                {
                    return Fail(token.Line, record.Name + ": interfaces cannot have reflected fields");
                }
                if (!Field(record, close))
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier && token.Text == "MFUNCTION")
            {
                if (!Function(record, close))
                {
                    return false;
                }
            }
            else if (token.Kind == TokenKind::Identifier &&
                     (IsRecordMarker(token.Text) || token.Text == "MENUM" || token.Text == "MGENERATED_BODY"))
            {
                return Fail(token.Line,
                            record.Name + ": " + std::string(token.Text) +
                                (token.Text == "MGENERATED_BODY" ? " appears twice" : " types cannot be nested"));
            }
            else if (IsPunct(_index, "{"))
            {
                if (!SkipBlock("markup inside a member body or nested type"))
                {
                    return false;
                }
            }
            else
            {
                ++_index;
            }
        }
        _index = close + 1;
        if (IsPunct(_index, ";"))
        {
            ++_index;
        }
        for (const FunctionDecl &function : record.Functions)
        {
            for (const FunctionDecl &other : record.Functions)
            {
                if (&function != &other && function.Name == other.Name)
                {
                    return Fail(other.Line,
                                record.Name + "::" + other.Name + ": reflected functions cannot be overloaded");
                }
            }
        }
        _file->Records.push_back(std::move(record));
        return true;
    }

    /// The end of the declaration starting at the cursor: its ';', or the '{' of a function body.
    size_t DeclarationEnd(size_t limit, bool stopAtBody) const
    {
        int depth = 0;
        for (size_t i = _index; i < limit; ++i)
        {
            if ((*_tokens)[i].Kind != TokenKind::Punct)
            {
                continue;
            }
            const std::string_view text = (*_tokens)[i].Text;
            if (depth == 0 && (text == ";" || (stopAtBody && text == "{")))
            {
                return i;
            }
            if (text == "(" || text == "[" || text == "{")
            {
                ++depth;
            }
            else if (text == ")" || text == "]" || text == "}")
            {
                --depth;
            }
        }
        return limit;
    }

    bool Field(RecordDecl &record, size_t limit)
    {
        FieldDecl field;
        field.Line = (*_tokens)[_index].Line;
        if (!Metadata(field.Metadata))
        {
            return false;
        }
        const size_t end = DeclarationEnd(limit, false);
        size_t first = _index;
        while (first < end && Is(first, TokenKind::Identifier))
        {
            const std::string_view word = (*_tokens)[first].Text;
            if (word == "static")
            {
                field.IsStatic = true;
            }
            else if (word == "thread_local" || word == "template")
            {
                return Fail((*_tokens)[first].Line,
                            record.Name + ": " + std::string(word) + " fields cannot be reflected");
            }
            else if (word != "mutable" && word != "inline" && word != "constexpr" && word != "constinit")
            {
                break;
            }
            ++first;
        }

        size_t declaratorEnd = end;
        int depth = 0;
        int angle = 0;
        for (size_t i = first; i < end; ++i)
        {
            const Token &token = (*_tokens)[i];
            if (token.Kind != TokenKind::Punct)
            {
                continue;
            }
            const std::string_view text = token.Text;
            if (depth == 0 && angle == 0 && (text == "=" || text == "{"))
            {
                declaratorEnd = i;
                break;
            }
            if (text == "(" && depth == 0)
            {
                return Fail(token.Line, record.Name + ": MPROPERTY marks a field; mark functions with MFUNCTION");
            }
            if (text == "(" || text == "[" || text == "{")
            {
                ++depth;
            }
            else if (text == ")" || text == "]" || text == "}")
            {
                --depth;
            }
            else if (depth == 0 && text == "<")
            {
                ++angle;
            }
            else if (depth == 0 && text == ">" && angle > 0)
            {
                --angle;
            }
            else if (depth == 0 && angle == 0 && text == ",")
            {
                return Fail(token.Line, record.Name + ": declare one reflected field per MPROPERTY");
            }
            else if (depth == 0 && text == ":")
            {
                return Fail(token.Line, record.Name + ": bit-fields cannot be reflected");
            }
            else if (depth == 0 && angle == 0 && (text == "&" || text == "&&" || text == "*"))
            {
                return Fail(token.Line, record.Name + ": pointer and reference fields cannot be reflected");
            }
        }
        size_t nameIndex = declaratorEnd == 0 ? 0 : declaratorEnd - 1;
        while (nameIndex > first && IsPunct(nameIndex, "]"))
        {
            int brackets = 0;
            while (nameIndex > first)
            {
                if (IsPunct(nameIndex, "]"))
                {
                    ++brackets;
                }
                else if (IsPunct(nameIndex, "[") && --brackets == 0)
                {
                    --nameIndex;
                    break;
                }
                --nameIndex;
            }
        }
        if (nameIndex <= first || !Is(nameIndex, TokenKind::Identifier))
        {
            return Fail(field.Line, record.Name + ": cannot find the name of the field after MPROPERTY");
        }
        field.Name = std::string((*_tokens)[nameIndex].Text);
        record.Fields.push_back(std::move(field));
        _index = end + 1;
        return true;
    }

    bool Function(RecordDecl &record, size_t limit)
    {
        FunctionDecl function;
        function.Line = (*_tokens)[_index].Line;
        if (!Metadata(function.Metadata))
        {
            return false;
        }
        SkipAttributes();
        const size_t end = DeclarationEnd(limit, true);
        size_t open = end;
        int depth = 0;
        for (size_t i = _index; i < end; ++i)
        {
            if (IsWord(i, "operator") || IsWord(i, "template"))
            {
                return Fail((*_tokens)[i].Line, record.Name + ": operators and function templates cannot be reflected");
            }
            if (IsPunct(i, "(") && depth == 0 && i > _index && Is(i - 1, TokenKind::Identifier) &&
                (*_tokens)[i - 1].Text != "decltype" && (*_tokens)[i - 1].Text != "alignas")
            {
                open = i;
                break;
            }
            if (IsPunct(i, "(") || IsPunct(i, "[") || IsPunct(i, "<"))
            {
                ++depth;
            }
            else if (IsPunct(i, ")") || IsPunct(i, "]") || IsPunct(i, ">"))
            {
                --depth;
            }
        }
        size_t close = 0;
        if (open == end || !MatchingClose(open, close))
        {
            return Fail(function.Line, record.Name + ": cannot find the function after MFUNCTION");
        }
        function.Name = std::string((*_tokens)[open - 1].Text);
        if (function.Name == record.Name)
        {
            return Fail(function.Line, record.Name + ": constructors cannot be reflected functions");
        }

        const bool onlyVoid = close == open + 2 && IsWord(open + 1, "void");
        for (const auto &[first, last] :
             onlyVoid ? std::vector<std::pair<size_t, size_t>>{} : SplitTopLevel(open + 1, close, true))
        {
            ParameterDecl parameter;
            size_t declarator = last;
            for (size_t i = first; i < last; ++i)
            {
                if (IsPunct(i, "="))
                {
                    declarator = i;
                    parameter.DefaultValue = Source(i + 1, last);
                    break;
                }
                if (IsPunct(i, "..."))
                {
                    return Fail((*_tokens)[i].Line,
                                record.Name + "::" + function.Name + ": variadic functions cannot be reflected");
                }
            }
            const size_t name = declarator - 1;
            if (declarator - first < 2 || !Is(name, TokenKind::Identifier) || IsPunct(name - 1, "::") ||
                IsBuiltinTypeWord((*_tokens)[name].Text) || (*_tokens)[name].Text == "const")
            {
                return Fail((*_tokens)[first].Line,
                            record.Name + "::" + function.Name + ": reflected function parameters need names");
            }
            parameter.Name = std::string((*_tokens)[name].Text);
            function.Parameters.push_back(std::move(parameter));
        }

        _index = end;
        if (IsPunct(_index, "{"))
        {
            if (!SkipBlock("markup inside a function body"))
            {
                return false;
            }
            if (IsPunct(_index, ";"))
            {
                ++_index;
            }
        }
        else
        {
            ++_index;
        }
        record.Functions.push_back(std::move(function));
        return true;
    }

    bool Enum()
    {
        EnumDecl decl;
        decl.Line = (*_tokens)[_index].Line;
        if (!Metadata(decl.Metadata) || !CurrentNamespaces(decl.Namespaces, decl.Line))
        {
            return false;
        }
        if (!IsWord(_index, "enum"))
        {
            return Fail(LineAt(_index), "MENUM must be followed by an enum");
        }
        ++_index;
        if (IsWord(_index, "class") || IsWord(_index, "struct"))
        {
            decl.IsScoped = true;
            ++_index;
        }
        SkipAttributes();
        if (!Is(_index, TokenKind::Identifier))
        {
            return Fail(LineAt(_index), "expected the enum name");
        }
        decl.Name = std::string((*_tokens)[_index].Text);
        ++_index;
        if (IsPunct(_index, ":"))
        {
            const size_t first = ++_index;
            while (_index < _tokens->size() && !IsPunct(_index, "{"))
            {
                ++_index;
            }
            decl.Underlying = Source(first, _index);
        }
        if (!decl.IsScoped && decl.Underlying.empty())
        {
            return Fail(decl.Line, decl.Name + ": an unscoped reflected enum needs a fixed underlying type");
        }
        size_t close = 0;
        if (!IsPunct(_index, "{") || !MatchingClose(_index, close))
        {
            return Fail(LineAt(_index), "expected the enumerators of " + decl.Name);
        }
        for (const auto &[first, last] : SplitTopLevel(_index + 1, close, false))
        {
            if (!Is(first, TokenKind::Identifier))
            {
                return Fail((*_tokens)[first].Line, decl.Name + ": expected an enumerator");
            }
            EnumeratorDecl enumerator{std::string((*_tokens)[first].Text), {}};
            if (!(*_tokens)[first].Doc.empty())
            {
                enumerator.Metadata.push_back({"Tooltip", (*_tokens)[first].Doc});
            }
            decl.Enumerators.push_back(std::move(enumerator));
        }
        _index = close + 1;
        if (IsPunct(_index, ";"))
        {
            ++_index;
        }
        _file->Enums.push_back(std::move(decl));
        return true;
    }

    bool Upgrade()
    {
        UpgradeDecl upgrade;
        upgrade.Line = (*_tokens)[_index].Line;
        if (!CurrentNamespaces(upgrade.Namespaces, upgrade.Line))
        {
            return false;
        }
        size_t close = 0;
        if (!IsPunct(_index + 1, "(") || !MatchingClose(_index + 1, close))
        {
            return Fail(upgrade.Line, "expected MARLIN_UPGRADE(Type, From, To)");
        }
        const auto parts = SplitTopLevel(_index + 2, close, false);
        if (parts.size() != 3 || parts[0].second - parts[0].first != 1 || !Is(parts[0].first, TokenKind::Identifier) ||
            !Is(parts[1].first, TokenKind::Number) || !Is(parts[2].first, TokenKind::Number))
        {
            return Fail(upgrade.Line, "expected MARLIN_UPGRADE(Type, From, To) with a type name and two versions");
        }
        upgrade.Type = std::string((*_tokens)[parts[0].first].Text);
        upgrade.From = std::stoi(std::string((*_tokens)[parts[1].first].Text));
        upgrade.To = std::stoi(std::string((*_tokens)[parts[2].first].Text));
        if (upgrade.To <= upgrade.From)
        {
            return Fail(upgrade.Line, "MARLIN_UPGRADE must go to a later version");
        }
        _file->Upgrades.push_back(std::move(upgrade));
        _index = close + 1;
        return true;
    }

    bool TestSource()
    {
        TestDecl source;
        source.Source = true;
        source.Line = (*_tokens)[_index].Line;
        source.File = *_path;
        if (!CurrentNamespaces(source.Namespaces, source.Line))
        {
            return false;
        }
        size_t close = 0;
        if (!IsPunct(_index + 1, "(") || !MatchingClose(_index + 1, close) || close != _index + 3 ||
            !Is(_index + 2, TokenKind::Identifier))
        {
            return Fail(source.Line, "expected MARLIN_TEST_SOURCE(Name)");
        }
        source.Name = std::string((*_tokens)[_index + 2].Text);
        _file->Tests.push_back(std::move(source));
        _index = close + 1;
        return true;
    }

    bool Test(bool fixture)
    {
        TestDecl test;
        test.Line = (*_tokens)[_index].Line;
        test.File = *_path;
        if (!CurrentNamespaces(test.Namespaces, test.Line))
        {
            return false;
        }
        size_t close = 0;
        const char *usage = fixture ? "expected MARLIN_TEST_F(Fixture, Suite, Name, Tags...)"
                                    : "expected MARLIN_TEST(Suite, Name, Tags...)";
        if (!IsPunct(_index + 1, "(") || !MatchingClose(_index + 1, close))
        {
            return Fail(test.Line, usage);
        }
        const auto parts = SplitTopLevel(_index + 2, close, false);
        const size_t first = fixture ? 1 : 0;
        if (parts.size() < first + 2)
        {
            return Fail(test.Line, usage);
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const auto [begin, end] = parts[i];
            if (i < first + 2 && (end - begin != 1 || !Is(begin, TokenKind::Identifier)))
            {
                return Fail(test.Line, usage);
            }
            if (i == first)
            {
                test.Suite = std::string((*_tokens)[begin].Text);
            }
            else if (i == first + 1)
            {
                test.Name = std::string((*_tokens)[begin].Text);
            }
            else if (i > first + 1)
            {
                test.Tags.push_back(Source(begin, end));
            }
        }
        for (const TestDecl &other : _file->Tests)
        {
            if (other.Suite == test.Suite && other.Name == test.Name)
            {
                return Fail(test.Line, "test " + test.Suite + "." + test.Name + " is defined twice");
            }
        }
        _file->Tests.push_back(std::move(test));
        _index = close + 1;
        return true;
    }

    const std::string *_path;
    std::string_view _text;
    std::vector<Token> *_tokens;
    ParsedFile *_file;
    std::vector<Diagnostic> *_errors;
    std::vector<Scope> _scopes;
    size_t _index = 0;
};
} // namespace

bool ParseFile(const std::string &path, std::string_view text, ParsedFile &result, std::vector<Diagnostic> &errors)
{
    std::vector<Token> tokens;
    std::string error;
    int line = 0;
    Lexer lexer(text, result);
    if (!lexer.Run(tokens, error, line))
    {
        errors.push_back({path, line, error});
        return false;
    }
    return Parser(path, text, tokens, result, errors).Run();
}
} // namespace Marlin::HeaderTool
