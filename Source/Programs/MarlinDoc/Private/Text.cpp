#include "Text.h"

#include "Diagnostics/Log.h"
#include "HAL/PlatformFile.h"
#include "Model.h"
#include "String/Format.h"

namespace Marlin::Doc
{
bool ReadFile(StringView path, String &text)
{
    const String native(path);
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    bool ok = PlatformFile::GetSize(file, size);
    if (ok)
    {
        char *data = text.ResizeUninitialized(static_cast<usize>(size));
        ok = PlatformFile::Read(file, data, static_cast<usize>(size), read) && read == size;
    }
    PlatformFile::Close(file);
    if (ok && text.AsView().StartsWith("\xEF\xBB\xBF"))
    {
        text.Remove(0, 3);
    }
    return ok;
}

bool WriteFile(StringView path, StringView text)
{
    String existing;
    if (ReadFile(path, existing) && existing.AsView() == text)
    {
        return true;
    }
    StringView folder;
    StringView name;
    if (path.SplitLast('/', folder, name) && !PlatformFile::CreateDirectory(String(folder).CStr()))
    {
        return false;
    }
    const String native(path);
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

String NormalizePath(StringView path)
{
    String result(path);
    result.Replace('\\', '/');
#if MARLIN_PLATFORM_WINDOWS
    result.ToLower();
#endif
    return result;
}

String RelativePath(StringView path, StringView root)
{
    String result(path);
    result.Replace('\\', '/');
    String base(root);
    base.Replace('\\', '/');
    if (!base.IsEmpty() && base.AsView().Last() != '/')
    {
        base.Append('/');
    }
    if (result.Length() > base.Length() && result.AsView().Left(base.Length()).EqualsIgnoreCase(base))
    {
        result.Remove(0, base.Length());
    }
    return result;
}

LineTable::LineTable(StringView text)
{
    _starts.Add(0);
    for (usize i = 0; i < text.Length(); ++i)
    {
        if (text[i] == '\n')
        {
            _starts.Add(static_cast<uint32>(i + 1));
        }
    }
}

void LineTable::Locate(uint32 offset, uint32 &line, uint32 &column) const
{
    usize low = 0;
    usize high = _starts.Count();
    while (high - low > 1)
    {
        const usize middle = (low + high) / 2;
        if (_starts[middle] <= offset)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    line = static_cast<uint32>(low + 1);
    column = offset - _starts[low] + 1;
}

namespace
{
bool IsReflectionMarker(StringView name)
{
    return name == "MCLASS" || name == "MSTRUCT" || name == "MENUM" || name == "MINTERFACE" || name == "MPROPERTY" ||
           name == "MFUNCTION";
}

/// The offset of the bracket that *close*, at *end* - 1, closes, or NotFound.
usize MatchBackwards(StringView text, usize end, char open, char close)
{
    int32 depth = 0;
    for (usize i = end; i > 0; --i)
    {
        const char c = text[i - 1];
        if (c == close)
        {
            ++depth;
        }
        else if (c == open && --depth == 0)
        {
            return i - 1;
        }
    }
    return NotFound;
}

/// Moves *position* back over whitespace; false if that crosses a blank line.
bool SkipWhiteSpaceBackwards(StringView text, usize &position)
{
    uint32 newlines = 0;
    while (position > 0 && Ascii::IsWhiteSpace(text[position - 1]))
    {
        newlines += text[position - 1] == '\n' ? 1 : 0;
        --position;
    }
    return newlines < 2;
}

usize LineStart(StringView text, usize position)
{
    while (position > 0 && text[position - 1] != '\n')
    {
        --position;
    }
    return position;
}

/// The text of a comment line after *marker* and one space, or false if the line is not such a comment.
bool CommentLine(StringView line, StringView marker, StringView &body)
{
    line = line.Trim();
    if (!line.StartsWith(marker) || (marker == "///" && line.StartsWith("////")))
    {
        return false;
    }
    body = line.RemoveStart(marker.Length());
    if (body.StartsWith(' '))
    {
        body = body.RemoveStart(1);
    }
    return true;
}
} // namespace

Preamble ScanPreamble(StringView text, uint32 offset, bool cpp)
{
    Preamble result;
    usize position = offset;
    while (true)
    {
        usize probe = position;
        if (!SkipWhiteSpaceBackwards(text, probe) || probe == 0)
        {
            break;
        }
        const usize probeLine = LineStart(text, probe);
        if (text.Substring(probeLine, probe - probeLine).Trim().StartsWith("//"))
        {
            break;
        }
        const char last = text[probe - 1];
        if (last == ']')
        {
            const usize open = MatchBackwards(text, probe, '[', ']');
            if (open == NotFound)
            {
                break;
            }
            position = open;
            continue;
        }
        if (cpp && (last == ')' || IsIdentifierChar(last)))
        {
            usize end = probe;
            usize open = NotFound;
            if (last == ')')
            {
                open = MatchBackwards(text, probe, '(', ')');
                if (open == NotFound)
                {
                    break;
                }
                end = open;
                while (end > 0 && Ascii::IsWhiteSpace(text[end - 1]))
                {
                    --end;
                }
            }
            usize begin = end;
            while (begin > 0 && IsIdentifierChar(text[begin - 1]))
            {
                --begin;
            }
            const StringView name = text.Substring(begin, end - begin);
            if (open != NotFound && IsReflectionMarker(name) && result.Macro.IsEmpty())
            {
                result.Macro = name;
                result.Arguments = CollapseWhitespace(text.Substring(open + 1, probe - open - 2));
                position = begin;
                continue;
            }
            if (open == NotFound && name.EndsWith("_API"))
            {
                position = begin;
                continue;
            }
        }
        break;
    }
    // The declaration must start its line; the comment is the run of `///` lines right above it.
    usize lineStart = LineStart(text, position);
    if (!text.Substring(lineStart, position - lineStart).Trim().IsEmpty())
    {
        return result;
    }
    List<StringView> lines;
    while (lineStart > 0)
    {
        const usize previousStart = LineStart(text, lineStart - 1);
        StringView body;
        if (!CommentLine(text.Substring(previousStart, lineStart - 1 - previousStart), "///", body))
        {
            break;
        }
        lines.Add(body.TrimEnd());
        lineStart = previousStart;
    }
    for (usize i = lines.Count(); i > 0; --i)
    {
        result.Docs.Append(lines[i - 1]);
        if (i > 1)
        {
            result.Docs.Append('\n');
        }
    }
    return result;
}

String ScanFileDocs(StringView text)
{
    String result;
    bool started = false;
    for (StringView line : text.Split('\n'))
    {
        StringView body;
        if (CommentLine(line, "//!", body))
        {
            if (started)
            {
                result.Append('\n');
            }
            result.Append(body.TrimEnd());
            started = true;
        }
        else if (started)
        {
            break;
        }
    }
    return result;
}

String CollapseWhitespace(StringView text)
{
    String result;
    bool space = false;
    for (const char c : text)
    {
        if (Ascii::IsWhiteSpace(c))
        {
            space = !result.IsEmpty();
            continue;
        }
        if (space)
        {
            result.Append(' ');
            space = false;
        }
        result.Append(c);
    }
    return result;
}

String FirstParagraph(StringView markdown)
{
    String result;
    for (StringView line : markdown.Split('\n'))
    {
        line = line.Trim();
        if (line.IsEmpty() || (line.StartsWith('#') && !result.IsEmpty()) || line.StartsWith("```"))
        {
            if (!result.IsEmpty() || line.StartsWith("```"))
            {
                break;
            }
            continue;
        }
        if (!result.IsEmpty())
        {
            result.Append(' ');
        }
        result.Append(line);
    }
    return result;
}

String PlainText(StringView markdown)
{
    String result;
    for (usize i = 0; i < markdown.Length(); ++i)
    {
        const char c = markdown[i];
        if (c == '`')
        {
            continue;
        }
        if (c == '*')
        {
            const bool opens =
                i + 1 < markdown.Length() && markdown[i + 1] != ' ' && (i == 0 || !IsIdentifierChar(markdown[i - 1]));
            const bool closes =
                i > 0 && markdown[i - 1] != ' ' && (i + 1 == markdown.Length() || !IsIdentifierChar(markdown[i + 1]));
            if (opens || closes)
            {
                continue;
            }
        }
        if (c == ']' && i + 1 < markdown.Length() && markdown[i + 1] == '(')
        {
            const usize close = markdown.IndexOf(')', i + 1);
            i = close != NotFound ? close : markdown.Length();
            continue;
        }
        if (c == '[' || c == ']')
        {
            continue;
        }
        result.Append(c);
    }
    return result;
}

void AppendHtml(StringWriter &output, StringView text)
{
    for (const char c : text)
    {
        switch (c)
        {
        case '&':
            output.Append("&amp;");
            break;
        case '<':
            output.Append("&lt;");
            break;
        case '>':
            output.Append("&gt;");
            break;
        case '"':
            output.Append("&quot;");
            break;
        default:
            output.Append(c);
            break;
        }
    }
}

String Slug(StringView text)
{
    String result;
    bool dash = false;
    const String plain = PlainText(text);
    for (const char c : plain.AsView())
    {
        const auto byte = static_cast<uint8>(c);
        if (byte >= 0x80 || Ascii::IsLetterOrDigit(c) || c == '_')
        {
            if (dash && !result.IsEmpty())
            {
                result.Append('-');
            }
            dash = false;
            result.Append(Ascii::ToLower(c));
        }
        else if (c == ' ' || c == '-')
        {
            dash = true;
        }
    }
    return result;
}

String FileNameOf(StringView text)
{
    String result;
    for (const char c : text)
    {
        result.Append(Ascii::IsLetterOrDigit(c) || c == '.' || c == '-' || c == '_' ? c : '_');
    }
    return result;
}

void Reporter::Warn(StringView file, uint32 line, StringView code, StringView message)
{
    ++_warnings;
    if (line != 0)
    {
        MARLIN_LOG(LogCore, Warning, "{0}({1}): warning {2}: {3}", file, line, code, message);
    }
    else
    {
        MARLIN_LOG(LogCore, Warning, "{0}: warning {1}: {2}", file, code, message);
    }
}

void Reporter::Error(StringView message)
{
    ++_errors;
    MARLIN_LOG(LogCore, Error, "{0}", message);
}
} // namespace Marlin::Doc
