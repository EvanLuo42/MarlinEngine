#include "Links.h"

#include "Containers/HashSet.h"
#include "String/Format.h"
#include "Syntax.h"
#include "Text.h"

namespace Marlin::Doc
{
namespace
{
/// The end of the code span whose opening backticks start at *begin*, or *begin* + their count if it never closes.
usize SkipCodeSpan(StringView line, usize begin)
{
    usize run = 0;
    while (begin + run < line.Length() && line[begin + run] == '`')
    {
        ++run;
    }
    usize i = begin + run;
    while (i < line.Length())
    {
        if (line[i] != '`')
        {
            ++i;
            continue;
        }
        usize other = 0;
        while (i + other < line.Length() && line[i + other] == '`')
        {
            ++other;
        }
        if (other == run)
        {
            return i + other;
        }
        i += other;
    }
    return begin + run;
}

/// *text* is one code span, `` `like this` ``: its content, or false.
bool SingleCodeSpan(StringView text, StringView &content)
{
    if (text.Length() < 3 || text.First() != '`' || text.Last() != '`')
    {
        return false;
    }
    content = text.Substring(1, text.Length() - 2);
    return !content.Contains('`');
}

bool IsRelativePage(StringView target)
{
    if (target.Contains("://") || target.StartsWith('#') || target.StartsWith("mailto:") ||
        target.StartsWith("manual:"))
    {
        return false;
    }
    const usize hash = target.IndexOf('#');
    return (hash != NotFound ? target.Left(hash) : target).EndsWith(".md");
}
} // namespace

void ForEachLink(StringView markdown, bool pageLinks, FunctionRef<void(const LinkOccurrence &)> visitor)
{
    bool fenced = false;
    usize lineStart = 0;
    while (lineStart <= markdown.Length())
    {
        usize lineEnd = markdown.IndexOf('\n', lineStart);
        lineEnd = lineEnd == NotFound ? markdown.Length() : lineEnd;
        const StringView line = markdown.Substring(lineStart, lineEnd - lineStart);
        if (IsFence(line))
        {
            fenced = !fenced;
        }
        else if (!fenced)
        {
            usize i = 0;
            while (i < line.Length())
            {
                const char c = line[i];
                if (c == '\\')
                {
                    i += 2;
                    continue;
                }
                if (c == '`')
                {
                    i = SkipCodeSpan(line, i);
                    continue;
                }
                if (c != '[')
                {
                    ++i;
                    continue;
                }
                const usize close = line.IndexOf(']', i + 1);
                if (close == NotFound)
                {
                    ++i;
                    continue;
                }
                const StringView label = line.Substring(i + 1, close - i - 1);
                const char after = close + 1 < line.Length() ? line[close + 1] : '\0';
                LinkOccurrence link;
                link.Begin = lineStart + i;
                link.Text = label;
                if (after == '(')
                {
                    // The target may hold parentheses itself: `` (`Add(int, int)`) ``.
                    usize end = NotFound;
                    int32 depth = 1;
                    for (usize j = close + 2; j < line.Length() && end == NotFound; ++j)
                    {
                        depth += line[j] == '(' ? 1 : (line[j] == ')' ? -1 : 0);
                        end = depth == 0 ? j : NotFound;
                    }
                    if (end == NotFound)
                    {
                        ++i;
                        continue;
                    }
                    StringView target = line.Substring(close + 2, end - close - 2).Trim();
                    if (target.StartsWith('<') && target.EndsWith('>'))
                    {
                        target = target.Substring(1, target.Length() - 2);
                    }
                    link.End = lineStart + end + 1;
                    StringView symbol;
                    if (SingleCodeSpan(target, symbol))
                    {
                        link.Kind = LinkKind::Symbol;
                        link.Target = symbol;
                        visitor(link);
                    }
                    else if (target.StartsWith("manual:"))
                    {
                        link.Kind = LinkKind::Manual;
                        link.Target = target;
                        visitor(link);
                    }
                    else if (pageLinks && IsRelativePage(target))
                    {
                        link.Kind = LinkKind::Page;
                        link.Target = target;
                        visitor(link);
                    }
                    i = end + 1;
                    continue;
                }
                StringView symbol;
                if (after != '[' && after != ':' && SingleCodeSpan(label, symbol))
                {
                    link.Kind = LinkKind::Symbol;
                    link.Target = symbol;
                    link.End = lineStart + close + 1;
                    visitor(link);
                    i = close + 1;
                    continue;
                }
                ++i;
            }
        }
        lineStart = lineEnd + 1;
    }
}

String CanonicalName(StringView name)
{
    String result;
    int32 depth = 0;
    for (usize i = 0; i < name.Length(); ++i)
    {
        const char c = name[i];
        if (c == '<')
        {
            ++depth;
            continue;
        }
        if (c == '>')
        {
            --depth;
            continue;
        }
        if (depth > 0 || c == ' ')
        {
            continue;
        }
        if (c == ':' && i + 1 < name.Length() && name[i + 1] == ':')
        {
            result.Append('.');
            ++i;
            continue;
        }
        result.Append(c);
    }
    return result;
}

namespace
{
StringView LastSegment(StringView canonical)
{
    StringView left;
    StringView right;
    return canonical.SplitLast('.', left, right) ? right : canonical;
}

bool InFamily(StringView context, StringView language)
{
    if (context == language)
    {
        return true;
    }
    return (context == "mscript" || context == "mshader") && language == "mcommon";
}

bool IsLanguageName(StringView name)
{
    return name == "cpp" || name == "mscript" || name == "mshader" || name == "mcommon";
}

bool IsType(const Item &item)
{
    const StringView kind = item.Kind;
    return kind == "class" || kind == "struct" || kind == "union" || kind == "enum" || kind == "interface" ||
           kind == "namespace" || kind == "extension";
}

enum class Outcome : uint8
{
    Resolved,
    Broken,
    Ambiguous,
};

struct Resolution
{
    Outcome Result = Outcome::Broken;
    String Id;
    String Candidates;
};

class Resolver
{
public:
    Resolver(Project &project, Reporter &reporter) : _project(&project), _reporter(&reporter)
    {
        for (uint32 m = 0; m < project.Modules.Count(); ++m)
        {
            const Module &module = project.Modules[m];
            for (uint32 i = 0; i < module.Items.Count(); ++i)
            {
                const Item &item = module.Items[i];
                if (!project.Items.TryAdd(item.Id, {m, i}))
                {
                    continue;
                }
                String canonical = CanonicalName(item.QualifiedName);
                _byLast.FindOrAdd(String(LastSegment(canonical))).Add({m, i});
                _canonical.Add(item.Id, canonical);
                _byName.FindOrAdd(Move(canonical)).Add({m, i});
            }
        }
        for (const Book &book : project.Books)
        {
            for (const Page &page : book.Pages)
            {
                _pages.Add(page.Id);
                for (const Heading &heading : page.Headings)
                {
                    _pages.Add(Format("{0}#{1}", page.Id, heading.Anchor));
                }
                _pagePaths.Add(NormalizePath(Format("{0}/{1}", book.Directory, page.Path)), page.Id);
            }
        }
    }

    void Run()
    {
        for (Module &module : _project->Modules)
        {
            for (Item &item : module.Items)
            {
                for (const String &base : item.Bases)
                {
                    const Resolution found = Resolve(base, ScopesOf(item, false), item.Language, true);
                    item.BaseIds.Add(found.Result == Outcome::Resolved ? found.Id : String());
                }
                if (!item.Extends.IsEmpty())
                {
                    const Resolution found = Resolve(item.Extends, ScopesOf(item, false), item.Language, true);
                    item.ExtendsId = found.Result == Outcome::Resolved ? found.Id : String();
                }
            }
        }
        for (Module &module : _project->Modules)
        {
            for (Item &item : module.Items)
            {
                const List<String> scopes = ScopesOf(item, true);
                // The comment ends right above the declaration.
                const uint32 docLines = item.Docs.IsEmpty() ? 0 : LineOf(item.Docs, item.Docs.Length()) + 1;
                const uint32 docLine = item.Line > docLines ? item.Line - docLines : item.Line;
                ResolveText(item.Docs, scopes, item.Language, item.File, docLine, StringView(), item.Links);
                Tokenize(item);
                AddRelations(item);
            }
            const List<String> none{String()};
            ResolveText(module.Docs, none, StringView(), Format("{0}/Docs.md", module.Directory), 0, StringView(),
                        module.Links);
            for (FileDocs &file : module.Files)
            {
                ResolveText(file.Docs, none, StringView(), file.File, 0, StringView(), file.Links);
            }
        }
        for (Book &book : _project->Books)
        {
            for (Page &page : book.Pages)
            {
                const List<String> none{String()};
                StringView folder;
                StringView name;
                String base = Format("{0}/{1}", book.Directory, page.Path);
                base = base.AsView().SplitLast('/', folder, name) ? String(folder) : String();
                ResolveText(page.Markdown, none, StringView(), Format("{0}/{1}", book.Directory, page.Path), 1, base,
                            page.Links);
            }
        }
    }

private:
    /// Where names in *item*'s docs are looked up, innermost first: the item itself if it has members, its
    /// enclosing types and namespaces, each type's bases after it, and the global scope.
    List<String> ScopesOf(const Item &item, bool includeSelf) const
    {
        List<String> scopes;
        String canonical = CanonicalName(item.QualifiedName);
        if (!includeSelf || !IsType(item))
        {
            StringView left;
            StringView right;
            canonical = canonical.AsView().SplitLast('.', left, right) ? String(left) : String();
        }
        while (true)
        {
            if (!canonical.IsEmpty())
            {
                scopes.Add(canonical);
                AddBaseScopes(canonical, item.Language, scopes, 0);
            }
            if (canonical.IsEmpty())
            {
                break;
            }
            StringView left;
            StringView right;
            canonical = canonical.AsView().SplitLast('.', left, right) ? String(left) : String();
        }
        scopes.Add(String());
        return scopes;
    }

    void AddBaseScopes(const String &canonical, StringView language, List<String> &scopes, uint32 depth) const
    {
        const List<ItemRef> *types = _byName.Find(canonical);
        if (types == nullptr || depth > 8)
        {
            return;
        }
        for (const ItemRef &ref : *types)
        {
            const Item &type = _project->Modules[ref.Module].Items[ref.Item];
            if (type.Language != language)
            {
                continue;
            }
            for (const String &baseId : type.BaseIds)
            {
                if (const String *base = baseId.IsEmpty() ? nullptr : _canonical.Find(baseId))
                {
                    if (!scopes.Contains(*base))
                    {
                        scopes.Add(*base);
                        AddBaseScopes(*base, language, scopes, depth + 1);
                    }
                }
            }
        }
    }

    Resolution Resolve(StringView target, Span<const String> scopes, StringView context, bool familyOnly) const
    {
        StringView name = target.Trim();
        StringView language;
        const usize at = name.IndexOf('@');
        if (at != NotFound && IsLanguageName(name.Left(at)))
        {
            language = name.Left(at);
            name = name.RemoveStart(at + 1);
        }
        StringView parameters;
        bool hasParameters = false;
        const usize open = name.IndexOf('(');
        if (open != NotFound && name.EndsWith(')') && !name.Left(open).EndsWith("operator"))
        {
            parameters = name.Substring(open + 1, name.Length() - open - 2);
            hasParameters = true;
            name = name.Left(open);
        }
        const String canonical = CanonicalName(name);
        const String wantedParameters = WithoutWhiteSpace(NormalizeType(parameters));
        auto accepts = [&](const Item &item, uint32 pass) {
            if (!language.IsEmpty() && item.Language != language)
            {
                return false;
            }
            if (language.IsEmpty() && pass == 0 && !context.IsEmpty() && !InFamily(context, item.Language))
            {
                return false;
            }
            return !hasParameters || WithoutWhiteSpace(item.Parameters) == wantedParameters;
        };
        const uint32 passes = familyOnly || !language.IsEmpty() || context.IsEmpty() ? 1 : 2;
        for (uint32 pass = 0; pass < passes; ++pass)
        {
            List<ItemRef> found;
            for (const String &scope : scopes)
            {
                const String key = scope.IsEmpty() ? canonical : Format("{0}.{1}", scope, canonical);
                if (const List<ItemRef> *candidates = _byName.Find(key))
                {
                    for (const ItemRef &ref : *candidates)
                    {
                        if (accepts(_project->Modules[ref.Module].Items[ref.Item], pass))
                        {
                            found.Add(ref);
                        }
                    }
                }
                if (!found.IsEmpty())
                {
                    return Decide(found);
                }
            }
            if (const List<ItemRef> *candidates = _byLast.Find(LastSegment(canonical)))
            {
                const String suffix = Format(".{0}", canonical);
                for (const ItemRef &ref : *candidates)
                {
                    const Item &item = _project->Modules[ref.Module].Items[ref.Item];
                    const String *full = _canonical.Find(item.Id);
                    if (full != nullptr && (*full == canonical || full->AsView().EndsWith(suffix)) &&
                        accepts(item, pass))
                    {
                        found.Add(ref);
                    }
                }
            }
            if (!found.IsEmpty())
            {
                return Decide(found);
            }
        }
        return {};
    }

    /// One declaration, or the overloads of one, resolves; anything else is ambiguous.
    Resolution Decide(const List<ItemRef> &found) const
    {
        Resolution result;
        const Item &first = _project->Modules[found[0].Module].Items[found[0].Item];
        const String &name = *_canonical.Find(first.Id);
        result.Result = Outcome::Resolved;
        result.Id = first.Id;
        for (const ItemRef &ref : found)
        {
            const Item &item = _project->Modules[ref.Module].Items[ref.Item];
            if (*_canonical.Find(item.Id) != name || item.Language != first.Language)
            {
                result.Result = Outcome::Ambiguous;
            }
            if (result.Candidates.Length() < 400)
            {
                result.Candidates.Append(result.Candidates.IsEmpty() ? "" : ", ");
                result.Candidates.Append(item.Id);
            }
        }
        return result;
    }

    /// The `manual:` address *target* names, relative to the page folder *base* for a Page link, or empty.
    String ResolvePage(LinkKind kind, StringView target, StringView base) const
    {
        StringView path = target;
        StringView anchor;
        const usize hash = target.IndexOf('#');
        if (hash != NotFound)
        {
            path = target.Left(hash);
            anchor = target.RemoveStart(hash + 1);
        }
        String id;
        if (kind == LinkKind::Manual)
        {
            id = path.RemoveStart(7);
            if (!_pages.Contains(id))
            {
                return {};
            }
        }
        else
        {
            List<String> parts;
            for (StringView part : base.Split('/'))
            {
                parts.Add(part);
            }
            for (StringView part : path.Split('/'))
            {
                if (part == "..")
                {
                    if (!parts.IsEmpty())
                    {
                        parts.Pop();
                    }
                }
                else if (part != "." && !part.IsEmpty())
                {
                    parts.Add(part);
                }
            }
            StringBuilder<256> joined;
            for (usize i = 0; i < parts.Count(); ++i)
            {
                joined.Append(i > 0 ? "/" : "");
                joined.Append(parts[i]);
            }
            const String *found = _pagePaths.Find(NormalizePath(joined.AsView()));
            if (found == nullptr)
            {
                return {};
            }
            id = *found;
        }
        if (!anchor.IsEmpty())
        {
            const String withAnchor = Format("{0}#{1}", id, anchor);
            if (!_pages.Contains(withAnchor))
            {
                return {};
            }
            return Format("manual:{0}", withAnchor);
        }
        return Format("manual:{0}", id);
    }

    void ResolveText(StringView markdown, Span<const String> scopes, StringView language, StringView file, uint32 line,
                     StringView base, List<Link> &links)
    {
        HashSet<String> done;
        ForEachLink(markdown, !base.IsEmpty(), [&](const LinkOccurrence &link) {
            if (!done.Add(String(link.Target)))
            {
                return;
            }
            const uint32 at = line + LineOf(markdown, link.Begin);
            if (link.Kind != LinkKind::Symbol)
            {
                const String page = ResolvePage(link.Kind, link.Target, base);
                if (page.IsEmpty())
                {
                    _reporter->Warn(file, at, "broken-doc-link", Format("`{0}` names no page or heading", link.Target));
                    return;
                }
                links.Add({String(link.Target), page});
                return;
            }
            const Resolution found = Resolve(link.Target, scopes, language, false);
            if (found.Result == Outcome::Resolved)
            {
                links.Add({String(link.Target), found.Id});
            }
            else if (found.Result == Outcome::Ambiguous)
            {
                _reporter->Warn(file, at, "ambiguous-doc-link",
                                Format("`{0}` could be {1}; add the parameter types or cpp@ / mscript@", link.Target,
                                       found.Candidates));
            }
            else
            {
                _reporter->Warn(file, at, "broken-doc-link", Format("`{0}` names nothing", link.Target));
            }
        });
    }

    static uint32 LineOf(StringView text, usize offset)
    {
        uint32 line = 0;
        for (usize i = 0; i < offset && i < text.Length(); ++i)
        {
            line += text[i] == '\n' ? 1 : 0;
        }
        return line;
    }

    /// Splits the signature into text and the names it mentions, linked when they resolve to another item.
    void Tokenize(Item &item) const
    {
        const List<String> scopes = ScopesOf(item, false);
        const StringView text = item.Signature;
        StringBuilder<256> plain;
        auto flush = [&]() {
            if (!plain.IsEmpty())
            {
                item.SignatureTokens.Add({plain.ToString(), String()});
                plain.Clear();
            }
        };
        usize i = 0;
        while (i < text.Length())
        {
            const char c = text[i];
            const bool starts = (Ascii::IsLetter(c) || c == '_') && (i == 0 || !IsIdentifierChar(text[i - 1]));
            if (!starts)
            {
                plain.Append(c);
                ++i;
                continue;
            }
            usize end = i;
            while (true)
            {
                while (end < text.Length() && IsIdentifierChar(text[end]))
                {
                    ++end;
                }
                if (end + 2 < text.Length() && text[end] == ':' && text[end + 1] == ':' &&
                    IsIdentifierChar(text[end + 2]))
                {
                    end += 2;
                    continue;
                }
                if (item.Language != "cpp" && end + 1 < text.Length() && text[end] == '.' &&
                    Ascii::IsUpper(text[end + 1]))
                {
                    end += 1;
                    continue;
                }
                break;
            }
            const StringView name = text.Substring(i, end - i);
            String ref;
            // Types are PascalCase; lowercase names are parameters, keywords and built-in types.
            if (Ascii::IsUpper(name.First()) && name != item.Name)
            {
                const Resolution found = Resolve(name, scopes, item.Language, true);
                if (found.Result == Outcome::Resolved && found.Id != item.Id)
                {
                    ref = found.Id;
                }
            }
            if (ref.IsEmpty())
            {
                plain.Append(name);
            }
            else
            {
                flush();
                item.SignatureTokens.Add({String(name), Move(ref)});
            }
            i = end;
        }
        flush();
    }

    void AddRelations(const Item &item)
    {
        for (const String &base : item.BaseIds)
        {
            if (!base.IsEmpty())
            {
                _project->Derived.FindOrAdd(base).Add(item.Id);
            }
        }
        if (!item.ExtendsId.IsEmpty())
        {
            _project->Extensions.FindOrAdd(item.ExtendsId).Add(item.Id);
        }
    }

    Project *_project;
    Reporter *_reporter;
    HashMap<String, List<ItemRef>> _byName;
    HashMap<String, List<ItemRef>> _byLast;
    HashMap<String, String> _canonical;
    HashSet<String> _pages;
    HashMap<String, String> _pagePaths;
};
} // namespace

void ResolveLinks(Project &project, Reporter &reporter)
{
    Resolver resolver(project, reporter);
    resolver.Run();
}
} // namespace Marlin::Doc
