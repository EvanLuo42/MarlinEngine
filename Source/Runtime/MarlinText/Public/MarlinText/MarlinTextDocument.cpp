#include "MarlinText/MarlinTextDocument.h"

#include "Memory/MemoryOps.h"
#include "String/Ascii.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "String/Utf.h"
#include "Templates/NumericLimits.h"

namespace Marlin
{
/// Reads MarlinText from *text*, whose first byte is at *base* in the document, into *arena*.
class MarlinTextParser
{
public:
    MarlinTextParser(StringView text, uint32 base, uint32 line, Arena &arena, MarlinTextDiagnostics &diagnostics,
                     const MarlinTextParseOptions &options)
        : _text(text), _base(base), _line(line), _arena(&arena), _diagnostics(&diagnostics), _options(&options)
    {
    }

    /// Finds the objects of a document and parses `!Version`.
    bool ParseDocument(MarlinTextDocument &document)
    {
        SkipTrivia();
        if (Peek() == '!')
        {
            ++_pos;
            if (ReadIdentifier(false) != "Version")
            {
                return Fail("only !Version may precede the objects");
            }
            SkipTrivia();
            MarlinTextValue version;
            if (!ParseValue(version) || version.Kind != MarlinTextValueKind::String)
            {
                return Fail("!Version takes a string");
            }
            document._version = version.Text;
        }
        return ParseObjects(document, nullptr, false, false);
    }

    /// Parses the fields of an object whose body starts at the cursor.
    bool ParseObjectBody(MarlinTextObject &object)
    {
        return ParseFields(object._fields, nullptr, false);
    }

    /// Parses a whole value that is all of the text.
    bool ParseStandaloneValue(MarlinTextValue &value)
    {
        SkipTrivia();
        if (!ParseValue(value))
        {
            return false;
        }
        SkipTrivia();
        return AtEnd() || Fail("unexpected text after the value");
    }

private:
    [[nodiscard]] bool AtEnd() const
    {
        return _pos >= _text.Length();
    }

    [[nodiscard]] char Peek(usize offset = 0) const
    {
        return _pos + offset < _text.Length() ? _text[_pos + offset] : '\0';
    }

    [[nodiscard]] uint32 Offset() const
    {
        return _base + static_cast<uint32>(_pos);
    }

    bool Fail(StringView message)
    {
        _diagnostics->Error(_line, message);
        return false;
    }

    void Advance()
    {
        _line += _text[_pos] == '\n' ? 1 : 0;
        ++_pos;
    }

    void SkipTrivia()
    {
        while (!AtEnd())
        {
            const char c = Peek();
            if (Ascii::IsWhiteSpace(c))
            {
                Advance();
            }
            else if (c == '/' && Peek(1) == '/')
            {
                while (!AtEnd() && Peek() != '\n')
                {
                    ++_pos;
                }
            }
            else if (c == '/' && Peek(1) == '*')
            {
                _pos += 2;
                while (!AtEnd() && (Peek() != '*' || Peek(1) != '/'))
                {
                    Advance();
                }
                _pos += AtEnd() ? 0 : 2;
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

    StringView ReadIdentifier(bool allowDots)
    {
        const usize start = _pos;
        if (!IsIdentifierStart(Peek()))
        {
            return {};
        }
        while (!AtEnd() && (Ascii::IsLetterOrDigit(Peek()) || Peek() == '_' ||
                            (allowDots && Peek() == '.' && IsIdentifierStart(Peek(1)))))
        {
            ++_pos;
        }
        return _text.Substring(start, _pos - start);
    }

    StringView Copy(StringView text)
    {
        char *copy = _arena->AllocateArray<char>(text.Length() + 1);
        MemCopy(copy, text.Data(), text.Length());
        copy[text.Length()] = '\0';
        return {copy, text.Length()};
    }

    /// Skips a balanced `{ ... }` starting at the cursor, minding strings, links and comments.
    bool SkipBlock()
    {
        int32 depth = 0;
        while (!AtEnd())
        {
            const char c = Peek();
            if (c == '"' || c == '`')
            {
                Advance();
                while (!AtEnd() && Peek() != c)
                {
                    if (c == '"' && Peek() == '\\')
                    {
                        Advance();
                    }
                    Advance();
                }
                if (AtEnd())
                {
                    return Fail("unterminated string");
                }
                Advance();
                continue;
            }
            if (c == '/' && (Peek(1) == '/' || Peek(1) == '*'))
            {
                SkipTrivia();
                continue;
            }
            Advance();
            if (c == '{')
            {
                ++depth;
            }
            else if (c == '}' && --depth == 0)
            {
                return true;
            }
        }
        return Fail("unbalanced braces");
    }

    MarlinTextCondition *ParseCondition(const MarlinTextCondition *parent, bool parentElse)
    {
        if (!_options->AllowConditions)
        {
            Fail("@if is only allowed in config files");
            return nullptr;
        }
        ++_pos;
        if (ReadIdentifier(false) != "if")
        {
            Fail("expected @if");
            return nullptr;
        }
        auto *condition = _arena->New<MarlinTextCondition>();
        condition->Parent = parent;
        condition->ParentElse = parentElse;
        condition->Line = _line;
        SkipTrivia();
        condition->Variable = ReadIdentifier(true);
        SkipTrivia();
        if (Peek() == '=' && Peek(1) == '=')
        {
            condition->Equal = true;
        }
        else if (Peek() == '!' && Peek(1) == '=')
        {
            condition->Equal = false;
        }
        else
        {
            Fail("expected == or != after the @if variable");
            return nullptr;
        }
        _pos += 2;
        SkipTrivia();
        MarlinTextValue value;
        if (Peek() == '"' && ParseString(value))
        {
            condition->Value = value.Text;
        }
        else
        {
            condition->Value = ReadIdentifier(true);
        }
        if (condition->Variable.IsEmpty() || condition->Value.IsEmpty())
        {
            Fail("expected @if Variable == Value");
            return nullptr;
        }
        SkipTrivia();
        if (Peek() != '{')
        {
            Fail("expected '{' after the @if test");
            return nullptr;
        }
        ++_pos;
        return condition;
    }

    /// Reads the `@else {` after a condition's block, if there is one.
    bool ParseElse()
    {
        const usize save = _pos;
        const uint32 line = _line;
        SkipTrivia();
        if (Peek() == '@' && _text.Substring(_pos + 1).StartsWith("else"))
        {
            _pos += 5;
            SkipTrivia();
            if (Peek() != '{')
            {
                return Fail("expected '{' after @else");
            }
            ++_pos;
            return true;
        }
        _pos = save;
        _line = line;
        return false;
    }

    bool ParseObjects(MarlinTextDocument &document, const MarlinTextCondition *condition, bool inElse, bool nested)
    {
        while (true)
        {
            SkipTrivia();
            if (AtEnd())
            {
                return !nested || Fail("missing '}' of an @if block");
            }
            if (Peek() == '}' && nested)
            {
                ++_pos;
                return true;
            }
            if (Peek() == '@')
            {
                const MarlinTextCondition *inner = ParseCondition(condition, inElse);
                if (inner == nullptr || !ParseObjects(document, inner, false, true))
                {
                    return false;
                }
                if (ParseElse() && !ParseObjects(document, inner, true, true))
                {
                    return false;
                }
                if (_diagnostics->HasErrors())
                {
                    return false;
                }
                continue;
            }
            auto object = MakeUnique<MarlinTextObject>();
            object->_arena = _arena;
            object->_begin = Offset();
            object->_line = _line;
            object->_condition = condition;
            object->_inElse = inElse;
            object->_typeName = ReadIdentifier(true);
            if (object->_typeName.IsEmpty())
            {
                return Fail("expected an object: TypeName { ... }");
            }
            SkipTrivia();
            if (Peek() == ':')
            {
                ++_pos;
                SkipTrivia();
                if (Peek() != '<' || !ParseValue(object->_parent))
                {
                    return Fail("expected the <link> of the object this one overrides");
                }
                SkipTrivia();
            }
            if (Peek() != '{')
            {
                return Fail("expected '{' after the object type");
            }
            object->_bodyBegin = Offset() + 1;
            object->_bodyLine = _line;
            if (!SkipBlock())
            {
                return false;
            }
            object->_end = Offset();
            document._objects.Add(Move(object));
        }
    }

    bool ParseFields(List<MarlinTextField> &fields, const MarlinTextCondition *condition, bool inElse)
    {
        while (true)
        {
            SkipTrivia();
            if (AtEnd())
            {
                return Fail("missing '}'");
            }
            const char c = Peek();
            if (c == '}')
            {
                ++_pos;
                return true;
            }
            if (c == ',' || c == ';')
            {
                ++_pos;
                continue;
            }
            if (c == '@')
            {
                const MarlinTextCondition *inner = ParseCondition(condition, inElse);
                if (inner == nullptr || !ParseFields(fields, inner, false))
                {
                    return false;
                }
                if (ParseElse() && !ParseFields(fields, inner, true))
                {
                    return false;
                }
                if (_diagnostics->HasErrors())
                {
                    return false;
                }
                continue;
            }

            MarlinTextField field;
            field.Line = _line;
            field.Begin = Offset();
            field.Condition = condition;
            field.InElse = inElse;
            if (c == '!')
            {
                field.IsDirective = true;
                ++_pos;
            }
            if (IsIdentifierStart(Peek()))
            {
                field.Key = ReadIdentifier(false);
            }
            else if (!field.IsDirective && (Peek() == '"' || Peek() == '-' || Ascii::IsDigit(Peek())))
            {
                MarlinTextValue key;
                if (!ParseValue(key))
                {
                    return false;
                }
                field.Key = key.Kind == MarlinTextValueKind::String ? key.Text : key.Source;
            }
            else
            {
                return Fail("expected a field: Key = value");
            }
            SkipTrivia();
            if (Peek() == '=')
            {
                _pos += 1;
            }
            else if ((Peek() == '+' || Peek() == '-') && Peek(1) == '=')
            {
                field.Operation = Peek() == '+' ? MarlinTextOperation::Append : MarlinTextOperation::Remove;
                _pos += 2;
            }
            else
            {
                return Fail("expected = after the field key");
            }
            SkipTrivia();
            field.ValueBegin = Offset();
            if (!ParseValue(field.Value))
            {
                return false;
            }
            field.ValueEnd = Offset();
            field.End = field.ValueEnd;
            fields.Add(field);
        }
    }

    template <typename T> Span<const T> Store(const List<T> &items)
    {
        if (items.IsEmpty())
        {
            return {};
        }
        T *copy = _arena->AllocateArray<T>(items.Count());
        for (usize i = 0; i < items.Count(); ++i)
        {
            ConstructAt(copy + i, items[i]);
        }
        return {copy, items.Count()};
    }

    bool ParseString(MarlinTextValue &value)
    {
        ++_pos;
        const usize start = _pos;
        bool escaped = false;
        while (!AtEnd() && Peek() != '"')
        {
            if (Peek() == '\n')
            {
                return Fail("unterminated string");
            }
            if (Peek() == '\\')
            {
                escaped = true;
                ++_pos;
            }
            ++_pos;
        }
        if (AtEnd())
        {
            return Fail("unterminated string");
        }
        const StringView raw = _text.Substring(start, _pos - start);
        ++_pos;
        value.Kind = MarlinTextValueKind::String;
        if (!escaped)
        {
            value.Text = raw;
            return true;
        }
        StringBuilder<256> decoded;
        for (usize i = 0; i < raw.Length(); ++i)
        {
            if (raw[i] != '\\' || i + 1 == raw.Length())
            {
                decoded.Append(raw[i]);
                continue;
            }
            const char next = raw[++i];
            switch (next)
            {
            case 'n':
                decoded.Append('\n');
                break;
            case 't':
                decoded.Append('\t');
                break;
            case 'r':
                decoded.Append('\r');
                break;
            case '0':
                decoded.Append('\0');
                break;
            case 'u': {
                uint32 codePoint = 0;
                if (i + 4 >= raw.Length() || !TryParse(raw.Substring(i + 1, 4), codePoint, 16))
                {
                    return Fail("\\u takes four hex digits");
                }
                char bytes[4];
                decoded.Append(StringView(bytes, Utf8::Encode(static_cast<char32_t>(codePoint), bytes)));
                i += 4;
                break;
            }
            default:
                decoded.Append(next);
                break;
            }
        }
        value.Text = Copy(decoded.AsView());
        return true;
    }

    /// The text up to *close*, for a link.
    bool ParseLink(MarlinTextValue &value, char close, MarlinTextValueKind kind)
    {
        ++_pos;
        const usize start = _pos;
        while (!AtEnd() && Peek() != close && Peek() != '\n')
        {
            ++_pos;
        }
        if (Peek() != close)
        {
            return Fail(close == '>' ? "unterminated <link>" : "unterminated `link`");
        }
        const StringView content = _text.Substring(start, _pos - start).Trim();
        ++_pos;
        value.Kind = kind;
        StringView target = content;
        StringView uuid;
        if (content.SplitLast('#', target, uuid))
        {
            uuid = uuid.Trim();
            if (!uuid.IsEmpty() && !Guid::TryParse(uuid, value.LinkGuid))
            {
                return Fail("the #UUID of a link is not a valid GUID");
            }
        }
        target = target.Trim();
        StringView path;
        StringView name;
        if (kind == MarlinTextValueKind::ObjectLink && target.SplitLast(':', path, name))
        {
            value.Text = path;
            value.LinkName = name;
        }
        else if (kind == MarlinTextValueKind::ObjectLink)
        {
            value.LinkName = target;
        }
        else
        {
            value.Text = target;
        }
        return true;
    }

    bool ParseNumber(MarlinTextValue &value)
    {
        const usize start = _pos;
        if (Peek() == '-' || Peek() == '+')
        {
            ++_pos;
        }
        while (!AtEnd() && (Ascii::IsLetterOrDigit(Peek()) || Peek() == '.' || Peek() == '_' ||
                            ((Peek() == '-' || Peek() == '+') && (_text[_pos - 1] == 'e' || _text[_pos - 1] == 'E'))))
        {
            ++_pos;
        }
        const StringView text = _text.Substring(start, _pos - start);
        StringView digits = text;
        const bool negative = digits.StartsWith('-');
        if (negative || digits.StartsWith('+'))
        {
            digits = digits.RemoveStart(1);
        }
        uint64 magnitude = 0;
        if (digits.StartsWith("0x") || digits.StartsWith("0X"))
        {
            if (!TryParse(digits.RemoveStart(2), magnitude, 16))
            {
                return Fail("invalid hexadecimal number");
            }
        }
        else if (digits == "inf" || digits == "nan")
        {
            value.Kind = MarlinTextValueKind::Float;
            value.Float = digits == "nan" ? NumericLimits<float64>::QuietNaN() : NumericLimits<float64>::Infinity();
            value.Float = negative ? -value.Float : value.Float;
            return true;
        }
        else if (digits.IndexOfAny(".eE") != StringView::NotFound)
        {
            value.Kind = MarlinTextValueKind::Float;
            return TryParse(text, value.Float) || Fail("invalid number");
        }
        else if (!TryParse(digits, magnitude))
        {
            return Fail("invalid number");
        }
        value.Kind = MarlinTextValueKind::Integer;
        value.Integer = negative ? static_cast<int64>(0 - magnitude) : static_cast<int64>(magnitude);
        return true;
    }

    bool ParseElements(MarlinTextValue &value, char close)
    {
        ++_pos;
        List<MarlinTextValue> elements;
        while (true)
        {
            SkipTrivia();
            if (AtEnd())
            {
                return Fail(close == ']' ? "missing ']'" : "missing ')'");
            }
            if (Peek() == close)
            {
                ++_pos;
                break;
            }
            if (Peek() == ',')
            {
                ++_pos;
                continue;
            }
            MarlinTextValue element;
            if (!ParseValue(element))
            {
                return false;
            }
            elements.Add(element);
        }
        value.Elements = Store(elements);
        return true;
    }

    bool ParseValue(MarlinTextValue &value)
    {
        const usize start = _pos;
        value.Line = _line;
        bool ok = false;
        const char c = Peek();
        if (c == '"')
        {
            ok = ParseString(value);
        }
        else if (c == '<')
        {
            ok = ParseLink(value, '>', MarlinTextValueKind::ObjectLink);
        }
        else if (c == '`')
        {
            ok = ParseLink(value, '`', MarlinTextValueKind::BinaryLink);
        }
        else if (c == '(')
        {
            value.Kind = MarlinTextValueKind::Tuple;
            ok = ParseElements(value, ')');
        }
        else if (c == '[')
        {
            value.Kind = MarlinTextValueKind::Array;
            ok = ParseElements(value, ']');
        }
        else if (c == '{')
        {
            ++_pos;
            List<MarlinTextField> fields;
            value.Kind = MarlinTextValueKind::Block;
            ok = ParseFields(fields, nullptr, false);
            value.Fields = Store(fields);
        }
        else if (Ascii::IsDigit(c) ||
                 ((c == '-' || c == '+' || c == '.') && (Ascii::IsDigit(Peek(1)) || Peek(1) == '.')) ||
                 ((c == '-' || c == '+') &&
                  (_text.Substring(_pos + 1).StartsWith("inf") || _text.Substring(_pos + 1).StartsWith("nan"))))
        {
            ok = ParseNumber(value);
        }
        else if (IsIdentifierStart(c))
        {
            ok = ParseWord(value);
        }
        else
        {
            return Fail("expected a value");
        }
        value.Source = _text.Substring(start, _pos - start);
        return ok;
    }

    bool ParseWord(MarlinTextValue &value)
    {
        const usize start = _pos;
        const StringView word = ReadIdentifier(true);
        if (word == "true" || word == "false")
        {
            value.Kind = MarlinTextValueKind::Bool;
            value.Bool = word == "true";
            return true;
        }
        if (word == "null")
        {
            value.Kind = MarlinTextValueKind::Null;
            return true;
        }
        if (word == "inf" || word == "nan")
        {
            value.Kind = MarlinTextValueKind::Float;
            value.Float = word == "nan" ? NumericLimits<float64>::QuietNaN() : NumericLimits<float64>::Infinity();
            return true;
        }
        const usize save = _pos;
        const uint32 line = _line;
        SkipTrivia();
        if (Peek() == '{')
        {
            ++_pos;
            List<MarlinTextField> fields;
            value.Kind = MarlinTextValueKind::Object;
            value.Text = word;
            const bool ok = ParseFields(fields, nullptr, false);
            value.Fields = Store(fields);
            return ok;
        }
        usize end = save;
        while (Peek() == '|')
        {
            ++_pos;
            SkipTrivia();
            if (ReadIdentifier(false).IsEmpty())
            {
                return Fail("expected a flag after '|'");
            }
            end = _pos;
            SkipTrivia();
        }
        _pos = end;
        if (end == save)
        {
            _line = line;
        }
        value.Kind = MarlinTextValueKind::Identifier;
        value.Text = _text.Substring(start, end - start);
        return true;
    }

    StringView _text;
    uint32 _base;
    usize _pos = 0;
    uint32 _line;
    Arena *_arena;
    MarlinTextDiagnostics *_diagnostics;
    const MarlinTextParseOptions *_options;
};

void MarlinTextDiagnostics::Error(uint32 line, StringView message)
{
    _diagnostics.Add({line, String(message), true});
    ++_errorCount;
}

void MarlinTextDiagnostics::Warning(uint32 line, StringView message)
{
    _diagnostics.Add({line, String(message), false});
}

String MarlinTextDiagnostics::ToString(StringView path) const
{
    StringBuilder<256> text;
    for (const MarlinTextDiagnostic &diagnostic : _diagnostics)
    {
        char digits[16];
        text.Append(path.IsEmpty() ? StringView("<text>") : path)
            .Append('(')
            .Append(StringView(digits, FormatUnsigned(diagnostic.Line, digits)))
            .Append("): ")
            .Append(diagnostic.IsError ? "error: " : "warning: ")
            .Append(diagnostic.Message.AsView())
            .Append('\n');
    }
    return text.ToString();
}

const MarlinTextField *MarlinTextObject::Find(StringView key, bool directive) const
{
    for (const MarlinTextField &field : _fields)
    {
        if (field.Condition == nullptr && field.IsDirective == directive && field.Key == key)
        {
            return &field;
        }
    }
    return nullptr;
}

Guid MarlinTextObject::GetUuid() const
{
    Guid uuid;
    const MarlinTextField *field = Find("UUID", true);
    if (field != nullptr && field->Value.Kind == MarlinTextValueKind::String)
    {
        (void)Guid::TryParse(field->Value.Text, uuid);
    }
    return uuid;
}

StringView MarlinTextObject::GetName() const
{
    const MarlinTextField *field = Find("Name", true);
    return field != nullptr && field->Value.Kind == MarlinTextValueKind::String ? field->Value.Text : StringView();
}

uint32 MarlinTextObject::GetSchema() const
{
    const MarlinTextField *field = Find("Schema", true);
    return field != nullptr && field->Value.Kind == MarlinTextValueKind::Integer && field->Value.Integer > 0
               ? static_cast<uint32>(field->Value.Integer)
               : 1;
}

bool MarlinTextObject::Rename(StringView from, StringView to)
{
    for (MarlinTextField &field : _fields)
    {
        if (!field.IsDirective && field.Key == from)
        {
            char *copy = _arena->AllocateArray<char>(to.Length() + 1);
            MemCopy(copy, to.Data(), to.Length());
            field.Key = StringView(copy, to.Length());
            return true;
        }
    }
    return false;
}

bool MarlinTextObject::Remove(StringView key)
{
    return _fields.RemoveAll([&](const MarlinTextField &field) { return !field.IsDirective && field.Key == key; }) != 0;
}

bool MarlinTextObject::Set(StringView key, StringView valueText, bool directive)
{
    char *copy = _arena->AllocateArray<char>(key.Length() + valueText.Length() + 1);
    MemCopy(copy, key.Data(), key.Length());
    MemCopy(copy + key.Length(), valueText.Data(), valueText.Length());
    MarlinTextDiagnostics diagnostics;
    const MarlinTextParseOptions options;
    MarlinTextParser parser(StringView(copy + key.Length(), valueText.Length()), 0, _line, *_arena, diagnostics,
                            options);
    MarlinTextField field;
    field.Key = StringView(copy, key.Length());
    field.IsDirective = directive;
    field.Line = _line;
    if (!parser.ParseStandaloneValue(field.Value))
    {
        return false;
    }
    for (MarlinTextField &existing : _fields)
    {
        if (existing.Condition == nullptr && existing.IsDirective == directive && existing.Key == key)
        {
            existing.Value = field.Value;
            existing.Operation = MarlinTextOperation::Assign;
            return true;
        }
    }
    _fields.Add(field);
    return true;
}

MarlinTextDocument::MarlinTextDocument() : _arena(MakeUnique<Arena>())
{
}

MarlinTextDocument::~MarlinTextDocument() = default;

bool MarlinTextDocument::Parse(StringView text, MarlinTextDiagnostics &diagnostics,
                               const MarlinTextParseOptions &options)
{
    _objects.Clear();
    _arena->Reset();
    _version = {};
    _options = options;
    _text = text;
    MarlinTextParser parser(_text.AsView(), 0, 1, *_arena, diagnostics, _options);
    if (!parser.ParseDocument(*this))
    {
        _objects.Clear();
        return false;
    }
    return true;
}

const MarlinTextObject *MarlinTextDocument::GetObject(usize index, MarlinTextDiagnostics &diagnostics)
{
    MarlinTextObject &object = *_objects[index];
    if (!object._parsed)
    {
        const StringView body = _text.AsView().Substring(object._bodyBegin, object._end - object._bodyBegin);
        MarlinTextParser parser(body, object._bodyBegin, object._bodyLine, *_arena, diagnostics, _options);
        if (!parser.ParseObjectBody(object))
        {
            return nullptr;
        }
        object._parsed = true;
    }
    return &object;
}

void MarlinTextDocument::GetActiveObjects(const MarlinTextConditions &conditions, List<usize> &indices) const
{
    for (usize i = 0; i < _objects.Count(); ++i)
    {
        if (conditions.Holds(_objects[i]->_condition, _objects[i]->_inElse))
        {
            indices.Add(i);
        }
    }
}

usize MarlinTextDocument::FindObject(const Guid &uuid, MarlinTextDiagnostics &diagnostics)
{
    for (usize i = 0; i < _objects.Count(); ++i)
    {
        const MarlinTextObject *object = GetObject(i, diagnostics);
        if (object != nullptr && object->GetUuid() == uuid)
        {
            return i;
        }
    }
    return NotFound;
}

uint32 MarlinTextDocument::LineOf(uint32 offset) const
{
    uint32 line = 1;
    const char *text = _text.Data();
    for (uint32 i = 0; i < offset; ++i)
    {
        line += text[i] == '\n' ? 1 : 0;
    }
    return line;
}

bool MarlinTextDocument::Replace(uint32 begin, uint32 end, StringView text, MarlinTextDiagnostics &diagnostics)
{
    String edited;
    edited.Reserve(_text.Length() - (end - begin) + text.Length());
    edited.Append(_text.AsView().Left(begin)).Append(text).Append(_text.AsView().Substring(end));
    const MarlinTextParseOptions options = _options;
    return Parse(edited, diagnostics, options);
}

namespace
{
StringView LineIndent(StringView text, uint32 offset)
{
    uint32 start = offset;
    while (start > 0 && text[start - 1] != '\n')
    {
        --start;
    }
    uint32 end = start;
    while (end < text.Length() && (text[end] == ' ' || text[end] == '\t'))
    {
        ++end;
    }
    return text.Substring(start, end - start);
}

bool OnlySpaceBefore(StringView text, uint32 offset)
{
    for (uint32 i = offset; i > 0 && text[i - 1] != '\n'; --i)
    {
        if (text[i - 1] != ' ' && text[i - 1] != '\t')
        {
            return false;
        }
    }
    return true;
}

bool IsValueText(StringView text)
{
    Arena arena(1024);
    MarlinTextDiagnostics diagnostics;
    const MarlinTextParseOptions options;
    MarlinTextValue value;
    MarlinTextParser parser(text, 0, 1, arena, diagnostics, options);
    return parser.ParseStandaloneValue(value);
}
} // namespace

bool MarlinTextDocument::SetField(usize index, StringView key, StringView valueText, MarlinTextDiagnostics &diagnostics,
                                  bool directive)
{
    if (!IsValueText(valueText))
    {
        diagnostics.Error(0, "the new value of a field is not a MarlinText value");
        return false;
    }
    const MarlinTextObject *object = GetObject(index, diagnostics);
    if (object == nullptr)
    {
        return false;
    }
    const MarlinTextField *field = object->Find(key, directive);
    if (field != nullptr && field->Operation == MarlinTextOperation::Assign)
    {
        return Replace(field->ValueBegin, field->ValueEnd, valueText, diagnostics);
    }

    const StringView text = _text.AsView();
    const MarlinTextField *lastDirective = nullptr;
    const MarlinTextField *lastField = nullptr;
    for (const MarlinTextField &field : object->_fields)
    {
        if (field.Condition == nullptr)
        {
            (field.IsDirective ? lastDirective : lastField) = &field;
        }
    }
    const MarlinTextField *sibling = lastField != nullptr ? lastField : lastDirective;
    StringBuilder<256> line;
    if (sibling != nullptr)
    {
        line.Append(LineIndent(text, sibling->Begin));
    }
    else
    {
        line.Append(LineIndent(text, object->_begin)).Append("    ");
    }
    line.Append(directive ? "!" : "").Append(key).Append(" = ").Append(valueText).Append('\n');

    // Directives go after the other directives, or first; fields go last.
    if (directive)
    {
        uint32 at = lastDirective != nullptr ? lastDirective->End : object->_bodyBegin;
        while (at < text.Length() && text[at] != '\n')
        {
            ++at;
        }
        if (at < text.Length())
        {
            return Replace(at + 1, at + 1, line.AsView(), diagnostics);
        }
    }
    const uint32 close = object->_end - 1;
    if (OnlySpaceBefore(text, close))
    {
        uint32 lineStart = close;
        while (lineStart > 0 && text[lineStart - 1] != '\n')
        {
            --lineStart;
        }
        return Replace(lineStart, lineStart, line.AsView(), diagnostics);
    }
    StringBuilder<256> inserted("\n");
    inserted.Append(line.AsView()).Append(LineIndent(text, object->_begin));
    return Replace(close, close, inserted.AsView(), diagnostics);
}

bool MarlinTextDocument::RemoveField(usize index, StringView key, MarlinTextDiagnostics &diagnostics)
{
    const MarlinTextObject *object = GetObject(index, diagnostics);
    if (object == nullptr)
    {
        return false;
    }
    const MarlinTextField *field = object->Find(key);
    if (field == nullptr)
    {
        return false;
    }
    const StringView text = _text.AsView();
    uint32 begin = field->Begin;
    uint32 end = field->End;
    uint32 lineEnd = end;
    while (lineEnd < text.Length() && (text[lineEnd] == ' ' || text[lineEnd] == '\t' || text[lineEnd] == ',' ||
                                       text[lineEnd] == ';' || text[lineEnd] == '\r'))
    {
        ++lineEnd;
    }
    if (text.Substring(lineEnd).StartsWith("//"))
    {
        while (lineEnd < text.Length() && text[lineEnd] != '\n')
        {
            ++lineEnd;
        }
    }
    if (OnlySpaceBefore(text, begin) && (lineEnd == text.Length() || text[lineEnd] == '\n'))
    {
        while (begin > 0 && text[begin - 1] != '\n')
        {
            --begin;
        }
        end = lineEnd < text.Length() ? lineEnd + 1 : lineEnd;
    }
    return Replace(begin, end, {}, diagnostics);
}

bool MarlinTextDocument::AddObject(StringView objectText, MarlinTextDiagnostics &diagnostics)
{
    MarlinTextDocument probe;
    if (!probe.Parse(objectText, diagnostics, _options) || probe.GetObjectCount() != 1)
    {
        diagnostics.Error(0, "AddObject takes the text of exactly one object");
        return false;
    }
    StringBuilder<1024> added;
    const StringView text = _text.AsView();
    if (!text.IsEmpty())
    {
        added.Append(text.EndsWith('\n') ? "\n" : "\n\n");
    }
    added.Append(objectText.Trim()).Append('\n');
    return Replace(static_cast<uint32>(text.Length()), static_cast<uint32>(text.Length()), added.AsView(), diagnostics);
}

bool MarlinTextDocument::RemoveObject(usize index, MarlinTextDiagnostics &diagnostics)
{
    const MarlinTextObject &object = *_objects[index];
    const StringView text = _text.AsView();
    uint32 begin = object._begin;
    uint32 end = object._end;
    while (begin > 0 && text[begin - 1] != '\n')
    {
        --begin;
    }
    while (end < text.Length() && text[end] != '\n')
    {
        ++end;
    }
    end = end < text.Length() ? end + 1 : end;
    while (end < text.Length() && (text[end] == '\n' || text[end] == '\r'))
    {
        ++end;
    }
    return Replace(begin, end, {}, diagnostics);
}
bool MarlinTextDocument::ReplaceObject(usize index, StringView objectText, MarlinTextDiagnostics &diagnostics)
{
    MarlinTextDocument probe;
    if (!probe.Parse(objectText, diagnostics, _options) || probe.GetObjectCount() != 1)
    {
        diagnostics.Error(0, "ReplaceObject takes the text of exactly one object");
        return false;
    }
    const MarlinTextObject &object = *_objects[index];
    return Replace(object._begin, object._end, objectText.Trim(), diagnostics);
}
} // namespace Marlin
