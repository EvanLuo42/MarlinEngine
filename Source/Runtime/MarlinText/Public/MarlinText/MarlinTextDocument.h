#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "MarlinText/MarlinTextValue.h"
#include "Memory/Arena.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Optional.h"

namespace Marlin
{
class MarlinTextDocument;

struct MarlinTextDiagnostic
{
    uint32 Line = 0;
    String Message;
    bool IsError = true;
};

/// Errors and warnings of parsing or reading; lines are 1-based, 0 when unknown.
class MARLINTEXT_API MarlinTextDiagnostics
{
public:
    void Error(uint32 line, StringView message);
    void Warning(uint32 line, StringView message);

    [[nodiscard]] bool HasErrors() const
    {
        return _errorCount != 0;
    }

    [[nodiscard]] Span<const MarlinTextDiagnostic> GetAll() const
    {
        return _diagnostics.AsSpan();
    }

    /// One diagnostic per line, `path(line): error: message`.
    [[nodiscard]] String ToString(StringView path = {}) const;

private:
    List<MarlinTextDiagnostic> _diagnostics;
    uint32 _errorCount = 0;
};

/// One object of a document: `TypeName [: <Parent>] { fields }`. Fields can be changed in place, which is what
/// schema upgrades do; the document's text is changed through MarlinTextDocument instead.
class MARLINTEXT_API MarlinTextObject
{
public:
    MarlinTextObject() = default;

    /// The type name, or the section of a config object.
    [[nodiscard]] StringView GetTypeName() const
    {
        return _typeName;
    }

    /// The `: <Parent>` link this object overrides, if any.
    [[nodiscard]] const MarlinTextValue *GetParent() const
    {
        return _parent.Kind == MarlinTextValueKind::ObjectLink ? &_parent : nullptr;
    }

    [[nodiscard]] Span<const MarlinTextField> GetFields() const
    {
        return _fields.AsSpan();
    }

    /// The unconditional field or directive *key*.
    [[nodiscard]] const MarlinTextField *Find(StringView key, bool directive = false) const;

    /// `!UUID`, or an invalid Guid.
    [[nodiscard]] Guid GetUuid() const;
    /// `!Name`, or empty.
    [[nodiscard]] StringView GetName() const;
    /// `!Schema`, or 1.
    [[nodiscard]] uint32 GetSchema() const;

    [[nodiscard]] uint32 GetLine() const
    {
        return _line;
    }

    /// Renames field *from*; false if it is missing.
    bool Rename(StringView from, StringView to);
    /// Removes field *key*; false if it is missing.
    bool Remove(StringView key);
    /// Sets field or directive *key* to *valueText*, parsed as a MarlinText value; false if it does not parse.
    bool Set(StringView key, StringView valueText, bool directive = false);

private:
    friend class MarlinTextDocument;
    friend class MarlinTextParser;

    Arena *_arena = nullptr;
    StringView _typeName;
    MarlinTextValue _parent;
    List<MarlinTextField> _fields;
    const MarlinTextCondition *_condition = nullptr;
    bool _inElse = false;
    uint32 _line = 0;
    uint32 _begin = 0;
    uint32 _end = 0;
    uint32 _bodyBegin = 0;
    /// The line of _bodyBegin, known while the document is parsed, so parsing the body later need not count lines.
    uint32 _bodyLine = 0;
    bool _parsed = false;
};

struct MarlinTextParseOptions
{
    /// `@if` blocks, which only config files may have.
    bool AllowConditions = false;
};

/// A MarlinText file. Parsing finds the objects at once and parses each on first use. Edits change the text
/// itself, keeping comments, order and formatting everywhere else, so files diff by the lines that changed.
class MARLINTEXT_API MarlinTextDocument
{
public:
    MarlinTextDocument();
    ~MarlinTextDocument();
    MARLIN_NONCOPYABLE(MarlinTextDocument);

    /// Replaces the document with *text*. False, with the problems in *diagnostics*, if its structure is invalid;
    /// errors inside an object surface when it is parsed.
    bool Parse(StringView text, MarlinTextDiagnostics &diagnostics, const MarlinTextParseOptions &options = {});

    [[nodiscard]] StringView GetText() const
    {
        return _text.AsView();
    }

    /// `!Version`, or empty.
    [[nodiscard]] StringView GetVersion() const
    {
        return _version;
    }

    [[nodiscard]] usize GetObjectCount() const
    {
        return _objects.Count();
    }

    /// Parses object *index* on first use; null if it has errors, which go to *diagnostics*.
    [[nodiscard]] const MarlinTextObject *GetObject(usize index, MarlinTextDiagnostics &diagnostics);
    /// The objects whose conditions hold, by index.
    void GetActiveObjects(const MarlinTextConditions &conditions, List<usize> &indices) const;
    /// The index of the object with `!UUID` *uuid*, or NotFound.
    [[nodiscard]] usize FindObject(const Guid &uuid, MarlinTextDiagnostics &diagnostics);

    /// Sets field *key* of object *index* to *valueText*, replacing only the value of an unconditional field
    /// or adding a line at the end of the object. False if the object does not parse or the text is not a value.
    bool SetField(usize index, StringView key, StringView valueText, MarlinTextDiagnostics &diagnostics,
                  bool directive = false);
    /// Removes the lines of unconditional field *key* of object *index*.
    bool RemoveField(usize index, StringView key, MarlinTextDiagnostics &diagnostics);
    /// Appends *objectText*, a whole object, after the last one.
    bool AddObject(StringView objectText, MarlinTextDiagnostics &diagnostics);
    bool RemoveObject(usize index, MarlinTextDiagnostics &diagnostics);
    /// Replaces object *index* with *objectText*, a whole object.
    bool ReplaceObject(usize index, StringView objectText, MarlinTextDiagnostics &diagnostics);

    static constexpr usize NotFound = ~usize{0};

private:
    friend class MarlinTextParser;

    bool Replace(uint32 begin, uint32 end, StringView text, MarlinTextDiagnostics &diagnostics);
    [[nodiscard]] uint32 LineOf(uint32 offset) const;

    String _text;
    StringView _version;
    MarlinTextParseOptions _options;
    UniquePtr<Arena> _arena;
    List<UniquePtr<MarlinTextObject>> _objects;
};
} // namespace Marlin
