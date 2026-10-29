#include "Books.h"

#include "Containers/HashSet.h"
#include "HAL/PlatformFile.h"
#include "String/Format.h"
#include "String/Number.h"
#include "Text.h"

namespace Marlin::Doc
{
namespace
{
/// Adds the path of every .md file below *directory*, relative to *base*, to *paths*.
void ListMarkdown(const String &directory, StringView base, List<String> &paths)
{
    DirectoryIterator iterator;
    if (!iterator.Open(directory.CStr()))
    {
        return;
    }
    DirectoryEntry entry;
    while (iterator.Next(entry))
    {
        const String path = Format("{0}/{1}", directory, StringView(entry.Name));
        if (entry.Info.IsDirectory)
        {
            ListMarkdown(path, base, paths);
        }
        else if (StringView(entry.Name).EndsWith(".md"))
        {
            paths.Add(RelativePath(path, base));
        }
    }
}
} // namespace

List<Heading> ReadHeadings(StringView markdown)
{
    List<Heading> headings;
    HashMap<String, uint32> used;
    bool fenced = false;
    for (StringView line : markdown.Split('\n'))
    {
        if (IsFence(line))
        {
            fenced = !fenced;
            continue;
        }
        line = line.TrimEnd();
        if (fenced || !line.StartsWith('#'))
        {
            continue;
        }
        uint32 level = 0;
        while (level < line.Length() && line[level] == '#')
        {
            ++level;
        }
        if (level > 6 || level >= line.Length() || line[level] != ' ')
        {
            continue;
        }
        Heading heading;
        heading.Level = level;
        heading.Text = line.RemoveStart(level + 1).Trim();
        heading.Anchor = Slug(heading.Text);
        uint32 &count = used.FindOrAdd(heading.Anchor);
        if (count++ > 0)
        {
            heading.Anchor = Format("{0}-{1}", heading.Anchor, count - 1);
        }
        headings.Add(Move(heading));
    }
    return headings;
}

bool ReadBook(StringView directory, StringView root, Book &book, Reporter &reporter)
{
    book.Directory = RelativePath(directory, root);
    StringView parent;
    StringView name;
    String normalized(directory);
    normalized.Replace('\\', '/');
    book.Name = normalized.AsView().SplitLast('/', parent, name) ? String(name) : normalized;
    book.Title = book.Name;
    const String summaryPath = Format("{0}/Summary.md", normalized);
    String summary;
    if (!ReadFile(summaryPath, summary))
    {
        reporter.Error(Format("{0} cannot be read", summaryPath));
        return false;
    }
    const String summaryFile = RelativePath(summaryPath, root);
    uint32 lineNumber = 0;
    HashSet<String> listed;
    for (StringView line : summary.AsView().Split('\n'))
    {
        ++lineNumber;
        const StringView trimmed = line.Trim();
        if (trimmed.StartsWith("# ") && book.Pages.IsEmpty())
        {
            book.Title = trimmed.RemoveStart(2).Trim();
            continue;
        }
        if (trimmed.StartsWith("<!--") && trimmed.Contains("order:"))
        {
            StringView value = trimmed.Substring(trimmed.IndexOf("order:") + 6);
            value = value.Left(value.IndexOf("-->")).Trim();
            book.HasOrder = TryParse(value, book.Order);
            continue;
        }
        if (!trimmed.StartsWith("- ") && !trimmed.StartsWith("* "))
        {
            continue;
        }
        const usize open = trimmed.IndexOf('[');
        const usize middle = trimmed.IndexOf("](");
        const usize close = middle != NotFound ? trimmed.IndexOf(')', middle) : NotFound;
        if (open == NotFound || middle == NotFound || close == NotFound)
        {
            reporter.Warn(summaryFile, lineNumber, "bad-summary-entry", "an entry is `- [Title](page.md)`");
            continue;
        }
        Page page;
        page.Title = trimmed.Substring(open + 1, middle - open - 1);
        page.Path = trimmed.Substring(middle + 2, close - middle - 2);
        usize indent = 0;
        while (indent < line.Length() && (line[indent] == ' ' || line[indent] == '\t'))
        {
            indent += line[indent] == '\t' ? 4 : 1;
        }
        page.Level = static_cast<uint32>(indent / 2);
        if (!page.Path.AsView().EndsWith(".md"))
        {
            reporter.Warn(summaryFile, lineNumber, "bad-summary-entry", Format("{0} is not a .md page", page.Path));
            continue;
        }
        if (!ReadFile(Format("{0}/{1}", normalized, page.Path), page.Markdown))
        {
            reporter.Warn(summaryFile, lineNumber, "broken-doc-link", Format("{0} does not exist", page.Path));
            continue;
        }
        page.Id = Format("{0}/{1}", book.Name, page.Path.AsView().RemoveEnd(3));
        page.Headings = ReadHeadings(page.Markdown);
        listed.Add(page.Path);
        book.Pages.Add(Move(page));
    }
    List<String> files;
    ListMarkdown(normalized, normalized, files);
    files.Sort();
    for (const String &file : files)
    {
        if (file != "Summary.md" && !listed.Contains(file))
        {
            reporter.Warn(Format("{0}/{1}", book.Directory, file), 0, "unlisted-manual-page",
                          "Summary.md does not list this page, so it is not published");
        }
    }
    return true;
}
} // namespace Marlin::Doc
