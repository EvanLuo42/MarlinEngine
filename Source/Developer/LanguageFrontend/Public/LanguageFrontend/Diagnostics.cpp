#include "LanguageFrontend/Diagnostics.h"

#include "String/StringBuilder.h"

namespace Marlin
{
SourceRange LanguageDiagnostic::GetRange() const
{
    for (const DiagnosticLabel &label : Labels)
    {
        if (label.Primary)
        {
            return label.Range;
        }
    }
    return {};
}

LanguageDiagnostic &LanguageDiagnostic::WithLabel(StringView message)
{
    for (DiagnosticLabel &label : Labels)
    {
        if (label.Primary)
        {
            label.Message = String(message.Data(), message.Length());
        }
    }
    return *this;
}

LanguageDiagnostic &LanguageDiagnostic::WithSecondaryLabel(SourceRange range, StringView message)
{
    if (range.IsValid())
    {
        Labels.Add({range, String(message.Data(), message.Length()), false});
    }
    return *this;
}

LanguageDiagnostic &LanguageDiagnostic::WithNote(StringView message)
{
    Notes.Add({DiagnosticNoteKind::Note, {}, String(message.Data(), message.Length())});
    return *this;
}

LanguageDiagnostic &LanguageDiagnostic::WithNote(SourceRange range, StringView message)
{
    Notes.Add({DiagnosticNoteKind::Note, range, String(message.Data(), message.Length())});
    return *this;
}

LanguageDiagnostic &LanguageDiagnostic::WithHelp(StringView message)
{
    Notes.Add({DiagnosticNoteKind::Help, {}, String(message.Data(), message.Length())});
    return *this;
}

uint32 LanguageDiagnostics::AddFile(SourceFile &file)
{
    _files.Add(&file);
    return static_cast<uint32>(_files.Count());
}

const SourceFile *LanguageDiagnostics::GetFile(uint32 id) const
{
    return id != 0 && id <= _files.Count() ? _files[id - 1] : nullptr;
}

LanguageDiagnostic &LanguageDiagnostics::Report(DiagnosticSeverity severity, DiagnosticCode code, SourceRange range,
                                                StringView message)
{
    LanguageDiagnostic &diagnostic = _diagnostics.AddDefaulted();
    diagnostic.Severity = severity;
    diagnostic.Code = code;
    diagnostic.Message = String(message.Data(), message.Length());
    if (range.IsValid())
    {
        diagnostic.Labels.Add({range, {}, true});
    }
    _errorCount += severity == DiagnosticSeverity::Error ? 1 : 0;
    return diagnostic;
}

namespace
{
constexpr usize TabWidth = 4;

struct Position
{
    uint32 Line = 0;
    uint32 Column = 0;
};

uint32 DigitCount(uint32 value)
{
    uint32 digits = 1;
    while (value >= 10)
    {
        value /= 10;
        ++digits;
    }
    return digits;
}

StringView LineText(const SourceFile &file, uint32 offset)
{
    const StringView text = file.GetText();
    usize begin = offset < text.Length() ? offset : text.Length();
    while (begin > 0 && text[begin - 1] != '\n')
    {
        --begin;
    }
    usize end = begin;
    while (end < text.Length() && text[end] != '\n' && text[end] != '\r')
    {
        ++end;
    }
    return text.Substring(begin, end - begin);
}

/// Display columns of the first *bytes* bytes of *line*: tabs expand, UTF-8 continuation bytes take none.
usize DisplayWidth(StringView line, usize bytes)
{
    usize width = 0;
    for (usize i = 0; i < bytes && i < line.Length(); ++i)
    {
        const auto c = static_cast<uint8>(line[i]);
        if (line[i] == '\t')
        {
            width += TabWidth;
        }
        else if ((c & 0xC0) != 0x80)
        {
            ++width;
        }
    }
    return width;
}

class Renderer
{
public:
    Renderer(const LanguageDiagnostics &diagnostics, StringWriter &output)
        : _diagnostics(&diagnostics), _output(&output)
    {
    }

    void Render(const LanguageDiagnostic &diagnostic)
    {
        _gutter = 1;
        for (const DiagnosticLabel &label : diagnostic.Labels)
        {
            MeasureGutter(label.Range);
        }
        for (const DiagnosticNote &note : diagnostic.Notes)
        {
            MeasureGutter(note.Range);
        }

        FormatTo(*_output, "{0}[M{1}]: ",
                 StringView(diagnostic.Severity == DiagnosticSeverity::Error ? "error" : "warning"),
                 static_cast<uint32>(diagnostic.Code));
        _output->Append(diagnostic.Message.AsView()).Append('\n');

        List<const DiagnosticLabel *> labels;
        for (const DiagnosticLabel &label : diagnostic.Labels)
        {
            if (label.Primary && _diagnostics->GetFile(label.Range.File) != nullptr)
            {
                labels.Add(&label);
            }
        }
        for (const DiagnosticLabel &label : diagnostic.Labels)
        {
            if (!label.Primary && _diagnostics->GetFile(label.Range.File) != nullptr)
            {
                labels.Add(&label);
            }
        }
        RenderLabels(labels.AsSpan());

        bool barWritten = false;
        for (const DiagnosticNote &note : diagnostic.Notes)
        {
            if (HasSource(note.Range))
            {
                continue;
            }
            if (!barWritten && !labels.IsEmpty())
            {
                Gutter();
                _output->Append(" |\n");
                barWritten = true;
            }
            Gutter();
            FormatTo(*_output, " = {0}: ", NoteKindName(note.Kind));
            _output->Append(note.Message.AsView()).Append('\n');
        }
        for (const DiagnosticNote &note : diagnostic.Notes)
        {
            if (!HasSource(note.Range))
            {
                continue;
            }
            FormatTo(*_output, "{0}: ", NoteKindName(note.Kind));
            _output->Append(note.Message.AsView()).Append('\n');
            const DiagnosticLabel label{note.Range, {}, true};
            const DiagnosticLabel *single[] = {&label};
            RenderLabels(single);
        }
        _output->Append('\n');
    }

private:
    static StringView NoteKindName(DiagnosticNoteKind kind)
    {
        return kind == DiagnosticNoteKind::Help ? "help" : "note";
    }

    [[nodiscard]] bool HasSource(SourceRange range) const
    {
        return range.IsValid() && _diagnostics->GetFile(range.File) != nullptr;
    }

    static Position PositionOf(const SourceFile &file, uint32 offset)
    {
        Position position;
        file.GetLineAndColumn(offset, position.Line, position.Column);
        return position;
    }

    void MeasureGutter(SourceRange range)
    {
        if (const SourceFile *file = _diagnostics->GetFile(range.File))
        {
            const uint32 digits = DigitCount(PositionOf(*file, range.Begin).Line);
            _gutter = digits > _gutter ? digits : _gutter;
        }
    }

    void Gutter(StringView number = {})
    {
        _output->Append(_gutter - number.Length(), ' ').Append(number);
    }

    /// The labels, grouped by file in order of appearance and by line within a file.
    void RenderLabels(Span<const DiagnosticLabel *const> labels)
    {
        List<uint32> files;
        for (const DiagnosticLabel *label : labels)
        {
            if (!files.Contains(label->Range.File))
            {
                files.Add(label->Range.File);
            }
        }
        for (usize f = 0; f < files.Count(); ++f)
        {
            const SourceFile &file = *_diagnostics->GetFile(files[f]);
            List<const DiagnosticLabel *> inFile;
            for (const DiagnosticLabel *label : labels)
            {
                if (label->Range.File == files[f])
                {
                    inFile.Add(label);
                }
            }
            const Position first = PositionOf(file, inFile[0]->Range.Begin);
            Gutter();
            FormatTo(*_output, "{0} {1}:{2}:{3}\n", StringView(f == 0 ? "-->" : ":::"), file.GetPath(), first.Line,
                     first.Column);
            Gutter();
            _output->Append(" |\n");

            List<uint32> lines;
            for (const DiagnosticLabel *label : inFile)
            {
                const uint32 line = PositionOf(file, label->Range.Begin).Line;
                if (!lines.Contains(line))
                {
                    lines.Add(line);
                }
            }
            lines.Sort();
            for (const uint32 line : lines)
            {
                RenderLine(file, line, inFile.AsSpan());
            }
        }
    }

    void RenderLine(const SourceFile &file, uint32 line, Span<const DiagnosticLabel *const> labels)
    {
        List<const DiagnosticLabel *> onLine;
        for (const DiagnosticLabel *label : labels)
        {
            if (PositionOf(file, label->Range.Begin).Line == line)
            {
                onLine.Add(label);
            }
        }
        onLine.Sort(
            [](const DiagnosticLabel *a, const DiagnosticLabel *b) { return a->Range.Begin < b->Range.Begin; });

        const StringView text = LineText(file, onLine[0]->Range.Begin);
        StringBuilder<16> number;
        FormatTo(number, "{0}", line);
        Gutter(number.AsView());
        _output->Append(" | ");
        for (const char c : text)
        {
            if (c == '\t')
            {
                _output->Append(TabWidth, ' ');
            }
            else
            {
                _output->Append(c);
            }
        }
        _output->Append('\n');

        for (const DiagnosticLabel *label : onLine)
        {
            const Position begin = PositionOf(file, label->Range.Begin);
            const Position end = PositionOf(file, label->Range.End);
            const usize startColumn = DisplayWidth(text, begin.Column - 1);
            const usize endColumn =
                end.Line == begin.Line ? DisplayWidth(text, end.Column - 1) : DisplayWidth(text, text.Length());
            const usize width = endColumn > startColumn ? endColumn - startColumn : 1;
            Gutter();
            _output->Append(" | ").Append(startColumn, ' ').Append(width, label->Primary ? '^' : '-');
            if (!label->Message.IsEmpty())
            {
                _output->Append(' ').Append(label->Message.AsView());
            }
            _output->Append('\n');
        }
    }

    const LanguageDiagnostics *_diagnostics;
    StringWriter *_output;
    uint32 _gutter = 1;
};
} // namespace

String LanguageDiagnostics::ToString() const
{
    StringBuilder<2048> output;
    Renderer renderer(*this, output);
    for (const LanguageDiagnostic &diagnostic : _diagnostics)
    {
        renderer.Render(diagnostic);
    }
    return output.ToString();
}
} // namespace Marlin
