#include "Syntax.h"

#include "String/Format.h"
#include "Text.h"

namespace Marlin::Doc
{
String CutDeclaration(StringView text, bool function, bool type, bool cpp)
{
    StringBuilder<512> kept;
    int32 depth = 0;
    bool closedParameters = false;
    for (usize i = 0; i < text.Length(); ++i)
    {
        const char c = text[i];
        const char next = i + 1 < text.Length() ? text[i + 1] : '\0';
        if (c == '/' && next == '/')
        {
            while (i < text.Length() && text[i] != '\n')
            {
                ++i;
            }
            kept.Append(' ');
            continue;
        }
        if (c == '/' && next == '*')
        {
            const usize end = text.IndexOf("*/", i + 2);
            i = end != NotFound ? end + 1 : text.Length();
            kept.Append(' ');
            continue;
        }
        if (c == '"' || c == '\'')
        {
            const usize start = i;
            for (++i; i < text.Length() && text[i] != c; ++i)
            {
                i += text[i] == '\\' ? 1 : 0;
            }
            kept.Append(text.Substring(start, i - start + 1));
            continue;
        }
        if (c == '(' || c == '[')
        {
            ++depth;
        }
        else if (c == ')' || c == ']')
        {
            --depth;
            closedParameters = closedParameters || (c == ')' && depth == 0);
        }
        else if (depth == 0)
        {
            if (c == '{' && (function || type))
            {
                break;
            }
            if (!cpp && function && c == '=' && next == '>')
            {
                break;
            }
            if (cpp && function && closedParameters && c == ':' && next != ':' && (i == 0 || text[i - 1] != ':'))
            {
                break;
            }
        }
        kept.Append(c);
    }
    // `*_API` macros say how the declaration links, not what it is.
    String result;
    const String collapsed = CollapseWhitespace(kept.AsView());
    const StringView view = collapsed.AsView();
    usize i = 0;
    while (i < view.Length())
    {
        if (IsIdentifierChar(view[i]) && (i == 0 || !IsIdentifierChar(view[i - 1])))
        {
            usize end = i;
            while (end < view.Length() && IsIdentifierChar(view[end]))
            {
                ++end;
            }
            const StringView word = view.Substring(i, end - i);
            if (word.EndsWith("_API") && word.Length() > 4)
            {
                i = end < view.Length() && view[end] == ' ' ? end + 1 : end;
                continue;
            }
            result.Append(word);
            i = end;
            continue;
        }
        result.Append(view[i]);
        ++i;
    }
    result.Trim();
    while (result.AsView().EndsWith(';') || result.AsView().EndsWith('='))
    {
        result.Remove(result.Length() - 1);
        result.TrimEnd();
    }
    return result;
}

String NormalizeType(StringView type)
{
    String result = CollapseWhitespace(type);
    result.Replace("Marlin::", "");
    return result;
}

String WithoutWhiteSpace(StringView text)
{
    String result;
    for (const char c : text)
    {
        if (!Ascii::IsWhiteSpace(c))
        {
            result.Append(c);
        }
    }
    return result;
}

void FinishItems(Module &module)
{
    HashMap<String, uint32> functions;
    for (const Item &item : module.Items)
    {
        if (item.HasParameters)
        {
            ++functions.FindOrAdd(Format("{0}|{1}", item.Language, item.QualifiedName));
        }
    }
    for (Item &item : module.Items)
    {
        const uint32 *count =
            item.HasParameters ? functions.Find(Format("{0}|{1}", item.Language, item.QualifiedName)) : nullptr;
        if (count != nullptr && *count > 1)
        {
            item.Id = Format("{0}({1})", item.Id, item.Parameters);
        }
    }
    HashMap<String, usize> byId;
    for (usize i = 0; i < module.Items.Count(); ++i)
    {
        byId.TryAdd(module.Items[i].Id, i);
    }
    for (const Item &item : module.Items)
    {
        if (item.Parent.IsEmpty())
        {
            continue;
        }
        if (const usize *parent = byId.Find(item.Parent))
        {
            module.Items[*parent].Children.Add(item.Id);
        }
    }
}
} // namespace Marlin::Doc
