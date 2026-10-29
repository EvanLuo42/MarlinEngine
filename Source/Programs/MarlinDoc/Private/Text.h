#pragma once

#include "Containers/List.h"
#include "String/Ascii.h"
#include "String/String.h"
#include "String/StringBuilder.h"

namespace Marlin::Doc
{
inline constexpr usize NotFound = StringView::NotFound;

bool ReadFile(StringView path, String &text);
/// Writes *text* to *path*, creating its folder, unless the file already holds it.
bool WriteFile(StringView path, StringView text);

/// *path* with '/' separators, lowercase on Windows: for comparing paths.
String NormalizePath(StringView path);
/// *path* relative to *root* with '/' separators, or *path* itself if it is not below *root*.
String RelativePath(StringView path, StringView root);

/// The 1-based line and column of byte offsets into a text.
class LineTable
{
public:
    explicit LineTable(StringView text);
    void Locate(uint32 offset, uint32 &line, uint32 &column) const;

private:
    List<uint32> _starts;
};

/// What precedes a declaration: its `///` comment and, in C++, its reflection marker (Documentation/Architecture/16
/// §3.1). Attributes (`[[nodiscard]]`, `[Flags]`) and `*_API` macros between them are skipped.
struct Preamble
{
    String Docs;
    String Macro;
    String Arguments;
};

/// Scans backwards from the declaration at *offset* of *text*. A blank line ends the comment.
Preamble ScanPreamble(StringView text, uint32 offset, bool cpp);
/// The `//!` lines of *text*: the first run of them.
String ScanFileDocs(StringView text);

/// Whitespace runs as single spaces, trimmed.
String CollapseWhitespace(StringView text);
/// The first paragraph of *markdown*, on one line.
String FirstParagraph(StringView markdown);
/// *markdown* as plain text: marks and link targets left out.
String PlainText(StringView markdown);

[[nodiscard]] inline bool IsIdentifierChar(char c)
{
    return Ascii::IsLetterOrDigit(c) || c == '_';
}

/// Whether *line* opens or closes a fenced code block.
[[nodiscard]] inline bool IsFence(StringView line)
{
    line = line.TrimStart();
    return line.StartsWith("```") || line.StartsWith("~~~");
}

void AppendHtml(StringWriter &output, StringView text);
/// A heading's anchor: lowercase, spaces as '-', punctuation left out; other characters are kept.
String Slug(StringView text);
/// *text* usable in a file name: letters, digits, '.', '-' and '_', anything else as '_'.
String FileNameOf(StringView text);
} // namespace Marlin::Doc
