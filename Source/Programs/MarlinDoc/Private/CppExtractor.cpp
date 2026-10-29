#include "CppExtractor.h"

#include "Containers/HashSet.h"
#include "HAL/PlatformLibrary.h"
#include "Memory/UniquePtr.h"
#include "Misc/CommandLine.h"
#include "String/Format.h"
#include "Syntax.h"
#include "Text.h"

// The library is loaded at run time, so the declarations are only used for their types.
#define CINDEX_NO_EXPORTS
#include "clang-c/Index.h"

namespace Marlin::Doc
{
namespace
{
#define MARLIN_DOC_CLANG_FUNCTIONS(X)                                                                                  \
    X(clang_createIndex)                                                                                               \
    X(clang_disposeIndex)                                                                                              \
    X(clang_parseTranslationUnit2)                                                                                     \
    X(clang_disposeTranslationUnit)                                                                                    \
    X(clang_getTranslationUnitCursor)                                                                                  \
    X(clang_visitChildren)                                                                                             \
    X(clang_getCursorKind)                                                                                             \
    X(clang_getCursorSpelling)                                                                                         \
    X(clang_getCursorDisplayName)                                                                                      \
    X(clang_getCursorLocation)                                                                                         \
    X(clang_getCursorExtent)                                                                                           \
    X(clang_getRangeStart)                                                                                             \
    X(clang_getRangeEnd)                                                                                               \
    X(clang_getExpansionLocation)                                                                                      \
    X(clang_getSpellingLocation)                                                                                       \
    X(clang_getFileName)                                                                                               \
    X(clang_getCString)                                                                                                \
    X(clang_disposeString)                                                                                             \
    X(clang_getCursorSemanticParent)                                                                                   \
    X(clang_getCursorLexicalParent)                                                                                    \
    X(clang_equalCursors)                                                                                              \
    X(clang_isCursorDefinition)                                                                                        \
    X(clang_getCXXAccessSpecifier)                                                                                     \
    X(clang_Cursor_getNumArguments)                                                                                    \
    X(clang_Cursor_getArgument)                                                                                        \
    X(clang_getCursorType)                                                                                             \
    X(clang_getTypeSpelling)                                                                                           \
    X(clang_getNumDiagnostics)                                                                                         \
    X(clang_getDiagnostic)                                                                                             \
    X(clang_getDiagnosticSeverity)                                                                                     \
    X(clang_formatDiagnostic)                                                                                          \
    X(clang_disposeDiagnostic)                                                                                         \
    X(clang_getTemplateCursorKind)                                                                                     \
    X(clang_getSpecializedCursorTemplate)                                                                              \
    X(clang_Cursor_isAnonymous)                                                                                        \
    X(clang_Cursor_isMacroFunctionLike)                                                                                \
    X(clang_Cursor_isNull)

struct ClangApi
{
#define MARLIN_DOC_CLANG_POINTER(name) decltype(&::name) name = nullptr;
    MARLIN_DOC_CLANG_FUNCTIONS(MARLIN_DOC_CLANG_POINTER)
#undef MARLIN_DOC_CLANG_POINTER
};

LibraryHandle _library;
ClangApi _clang;

String TakeString(CXString text)
{
    const char *data = _clang.clang_getCString(text);
    String result(data != nullptr ? data : "");
    _clang.clang_disposeString(text);
    return result;
}

struct Position
{
    String File;
    uint32 Offset = 0;
};

Position ExpansionOf(CXSourceLocation location)
{
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    _clang.clang_getExpansionLocation(location, &file, &line, &column, &offset);
    return {file != nullptr ? NormalizePath(TakeString(_clang.clang_getFileName(file))) : String(), offset};
}

Position SpellingOf(CXSourceLocation location)
{
    CXFile file = nullptr;
    unsigned line = 0;
    unsigned column = 0;
    unsigned offset = 0;
    _clang.clang_getSpellingLocation(location, &file, &line, &column, &offset);
    return {file != nullptr ? NormalizePath(TakeString(_clang.clang_getFileName(file))) : String(), offset};
}

List<CXCursor> ChildrenOf(CXCursor cursor)
{
    List<CXCursor> children;
    _clang.clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor, CXClientData data) {
            static_cast<List<CXCursor> *>(data)->Add(child);
            return CXChildVisit_Continue;
        },
        &children);
    return children;
}

/// A header of the module: its text, for comments and signatures.
struct HeaderText
{
    String Path;
    String Text;
    UniquePtr<LineTable> Lines;
};

/// Types and namespaces, whose members are visited.
enum class Scope : uint8
{
    Namespace,
    Class,
    Enum,
};

class CppVisitor
{
public:
    CppVisitor(const CppModuleJob &job, StringView root, Module &module, Reporter &reporter)
        : _job(&job), _root(root), _module(&module), _reporter(&reporter)
    {
    }

    bool Run()
    {
        if (!ReadHeaders())
        {
            return false;
        }
        StringBuilder<4096> umbrella;
        for (const String &header : _job->Headers)
        {
            FormatTo(umbrella, "#include \"{0}\"\n", header);
        }
        const String umbrellaPath = Format("{0}/Intermediate/MarlinDoc-{1}.cpp", _root, _job->Name);
        List<String> arguments;
        CommandLine::Split(_job->Arguments, arguments);
        arguments.RemoveAll([](const String &argument) { return argument == "-Werror"; });
        arguments.Add("-Wno-everything");
        arguments.Add("-ferror-limit=20");
        List<const char *> argv;
        for (const String &argument : arguments)
        {
            argv.Add(argument.CStr());
        }
        CXUnsavedFile unsaved{umbrellaPath.CStr(), umbrella.Data(), static_cast<unsigned long>(umbrella.Length())};
        CXIndex index = _clang.clang_createIndex(0, 0);
        CXTranslationUnit unit = nullptr;
        const unsigned options = CXTranslationUnit_DetailedPreprocessingRecord | CXTranslationUnit_SkipFunctionBodies |
                                 CXTranslationUnit_KeepGoing;
        const CXErrorCode code = _clang.clang_parseTranslationUnit2(
            index, umbrellaPath.CStr(), argv.Data(), static_cast<int>(argv.Count()), &unsaved, 1, options, &unit);
        if (code != CXError_Success || unit == nullptr)
        {
            _reporter->Error(Format("{0}: libclang could not parse the module's headers (error {1})", _job->Name,
                                    static_cast<int32>(code)));
            _clang.clang_disposeIndex(index);
            return false;
        }
        ReportDiagnostics(unit);
        const CXCursor top = _clang.clang_getTranslationUnitCursor(unit);
        VisitScope(top, String(), String(), Scope::Namespace);
        _clang.clang_disposeTranslationUnit(unit);
        _clang.clang_disposeIndex(index);
        FinishItems(*_module);
        return true;
    }

private:
    bool ReadHeaders()
    {
        for (const String &header : _job->Headers)
        {
            UniquePtr<HeaderText> text = MakeUnique<HeaderText>();
            text->Path = RelativePath(header, _root);
            if (!ReadFile(header, text->Text))
            {
                _reporter->Error(Format("{0} cannot be read", header));
                return false;
            }
            text->Lines = MakeUnique<LineTable>(text->Text.AsView());
            FileDocs docs;
            docs.File = text->Path;
            docs.Docs = ScanFileDocs(text->Text);
            if (!docs.Docs.IsEmpty())
            {
                _module->Files.Add(Move(docs));
            }
            _headerIndex.Add(NormalizePath(header), _headers.Count());
            _headers.Add(Move(text));
        }
        return true;
    }

    void ReportDiagnostics(CXTranslationUnit unit)
    {
        const unsigned count = _clang.clang_getNumDiagnostics(unit);
        uint32 shown = 0;
        for (unsigned i = 0; i < count; ++i)
        {
            CXDiagnostic diagnostic = _clang.clang_getDiagnostic(unit, i);
            if (_clang.clang_getDiagnosticSeverity(diagnostic) >= CXDiagnostic_Error && shown++ < 5)
            {
                const String text = TakeString(_clang.clang_formatDiagnostic(
                    diagnostic, CXDiagnostic_DisplaySourceLocation | CXDiagnostic_DisplayColumn));
                _reporter->Warn(_job->Directory, 0, "cpp-parse-error", text);
            }
            _clang.clang_disposeDiagnostic(diagnostic);
        }
    }

    /// The header *cursor* is declared in, or null if it is not one of the module's.
    HeaderText *HeaderOf(CXCursor cursor) const
    {
        const CXSourceLocation location = _clang.clang_getCursorLocation(cursor);
        const Position name = SpellingOf(location);
        const Position expansion = ExpansionOf(location);
        if (expansion.File != name.File || expansion.Offset != name.Offset)
        {
            // Written by a macro, such as the members MARLIN_NONCOPYABLE or MGENERATED_BODY declare.
            return nullptr;
        }
        const usize *index = _headerIndex.Find(name.File);
        return index != nullptr ? _headers[*index].Get() : nullptr;
    }

    void VisitScope(CXCursor scope, const String &qualified, const String &parentId, Scope kind)
    {
        for (const CXCursor &child : ChildrenOf(scope))
        {
            Visit(child, qualified, parentId, kind, false);
        }
    }

    void Visit(CXCursor cursor, const String &scope, const String &parentId, Scope scopeKind, bool isFriend)
    {
        const CXCursorKind kind = _clang.clang_getCursorKind(cursor);
        if (kind == CXCursor_LinkageSpec)
        {
            VisitScope(cursor, scope, parentId, scopeKind);
            return;
        }
        if (kind == CXCursor_FriendDecl)
        {
            for (const CXCursor &child : ChildrenOf(cursor))
            {
                Visit(child, scope, parentId, scopeKind, true);
            }
            return;
        }
        if (kind == CXCursor_MacroDefinition)
        {
            VisitMacro(cursor);
            return;
        }
        StringView itemKind = KindOf(cursor, kind, scopeKind);
        if (itemKind.IsEmpty() || (isFriend && itemKind != "function"))
        {
            return;
        }
        HeaderText *header = HeaderOf(cursor);
        if (header == nullptr)
        {
            return;
        }
        if (!isFriend && !_clang.clang_equalCursors(_clang.clang_getCursorSemanticParent(cursor),
                                                    _clang.clang_getCursorLexicalParent(cursor)))
        {
            // An out-of-line definition of something declared in its class or namespace.
            return;
        }
        String visibility("public");
        if (scopeKind == Scope::Class)
        {
            const CX_CXXAccessSpecifier access = _clang.clang_getCXXAccessSpecifier(cursor);
            if (access == CX_CXXPrivate)
            {
                return;
            }
            if (access == CX_CXXProtected)
            {
                visibility = "protected";
            }
        }
        const bool isType = itemKind == "class" || itemKind == "struct" || itemKind == "union" || itemKind == "enum";
        if (isType && !_clang.clang_isCursorDefinition(cursor))
        {
            return;
        }
        String name = TakeString(_clang.clang_getCursorSpelling(cursor));
        if (kind == CXCursor_Namespace)
        {
            if (name.IsEmpty() || name == "Private")
            {
                return;
            }
            const String qualified = scope.IsEmpty() ? name : Format("{0}::{1}", scope, name);
            String id;
            if (qualified != "Marlin")
            {
                id = Format("cpp:{0}", qualified);
                if (!_seen.Contains(id))
                {
                    AddItem(cursor, *header, itemKind, name, qualified, parentId, visibility);
                }
            }
            VisitScope(cursor, qualified, id, Scope::Namespace);
            return;
        }
        if (isType && _clang.clang_Cursor_isAnonymous(cursor))
        {
            return;
        }
        if (isType && !_clang.clang_Cursor_isNull(_clang.clang_getSpecializedCursorTemplate(cursor)))
        {
            name = TakeString(_clang.clang_getCursorDisplayName(cursor));
        }
        if (name.IsEmpty())
        {
            return;
        }
        const String qualified = scope.IsEmpty() ? name : Format("{0}::{1}", scope, name);
        Item &item = AddItem(cursor, *header, itemKind, name, qualified, parentId, visibility);
        if (isType)
        {
            const String id = item.Id;
            for (const CXCursor &child : ChildrenOf(cursor))
            {
                if (_clang.clang_getCursorKind(child) == CXCursor_CXXBaseSpecifier)
                {
                    String base = TakeString(_clang.clang_getTypeSpelling(_clang.clang_getCursorType(child)));
                    Find(id)->Bases.Add(Move(base));
                }
            }
            VisitScope(cursor, qualified, id, itemKind == "enum" ? Scope::Enum : Scope::Class);
        }
    }

    StringView KindOf(CXCursor cursor, CXCursorKind kind, Scope scope) const
    {
        switch (kind)
        {
        case CXCursor_Namespace:
            return "namespace";
        case CXCursor_ClassDecl:
            return "class";
        case CXCursor_StructDecl:
            return "struct";
        case CXCursor_UnionDecl:
            return "union";
        case CXCursor_EnumDecl:
            return "enum";
        case CXCursor_ClassTemplate:
            return _clang.clang_getTemplateCursorKind(cursor) == CXCursor_ClassDecl ? "class" : "struct";
        case CXCursor_EnumConstantDecl:
            return "enumerator";
        case CXCursor_FieldDecl:
            return "field";
        case CXCursor_VarDecl:
            return scope == Scope::Class ? "field" : "variable";
        case CXCursor_FunctionDecl:
        case CXCursor_FunctionTemplate:
            return scope == Scope::Class ? "method" : "function";
        case CXCursor_CXXMethod:
            return "method";
        case CXCursor_Constructor:
            return "constructor";
        case CXCursor_Destructor:
            return "destructor";
        case CXCursor_ConversionFunction:
            return "conversion";
        case CXCursor_TypedefDecl:
        case CXCursor_TypeAliasDecl:
        case CXCursor_TypeAliasTemplateDecl:
            return "typedef";
        case CXCursor_ConceptDecl:
            return "concept";
        default:
            return {};
        }
    }

    Item &AddItem(CXCursor cursor, HeaderText &header, StringView kind, StringView name, const String &qualified,
                  const String &parentId, const String &visibility)
    {
        const Position begin = ExpansionOf(_clang.clang_getRangeStart(_clang.clang_getCursorExtent(cursor)));
        const Position end = ExpansionOf(_clang.clang_getRangeEnd(_clang.clang_getCursorExtent(cursor)));
        const Position at = SpellingOf(_clang.clang_getCursorLocation(cursor));
        const StringView text = header.Text.AsView();
        uint32 first = begin.Offset;
        uint32 last = end.Offset;
        if (begin.File != at.File || end.File != at.File || first > last || last > text.Length())
        {
            first = at.Offset;
            last = at.Offset;
        }
        const bool function = kind == "function" || kind == "method" || kind == "constructor" || kind == "destructor" ||
                              kind == "conversion";
        Item item;
        item.Language = "cpp";
        item.Kind = kind;
        item.Name = name;
        item.QualifiedName = qualified;
        item.Parent = parentId;
        item.Visibility = visibility;
        item.File = header.Path;
        header.Lines->Locate(at.Offset, item.Line, item.Column);
        const bool type = kind == "class" || kind == "struct" || kind == "union" || kind == "enum";
        item.Signature =
            CutDeclaration(text.Substring(first, last - first), function, type || kind == "namespace", true);
        if (kind == "namespace")
        {
            item.Signature = Format("namespace {0}", qualified);
        }
        const Preamble preamble = ScanPreamble(text, first, true);
        item.Docs = preamble.Docs;
        item.ReflectionMacro = preamble.Macro;
        item.ReflectionArguments = preamble.Arguments;
        if (function)
        {
            item.HasParameters = true;
            const int32 count = _clang.clang_Cursor_getNumArguments(cursor);
            for (int32 i = 0; i < count; ++i)
            {
                const CXCursor argument = _clang.clang_Cursor_getArgument(cursor, static_cast<unsigned>(i));
                if (i > 0)
                {
                    item.Parameters.Append(',');
                }
                item.Parameters.Append(
                    NormalizeType(TakeString(_clang.clang_getTypeSpelling(_clang.clang_getCursorType(argument)))));
            }
        }
        item.Id = Format("cpp:{0}", qualified);
        // Redeclarations are the same item; a later one may bring the comment.
        const String key = Format("{0}|{1}|{2}", item.Id, item.Kind, item.Parameters);
        if (const usize *existing = _seen.Find(key))
        {
            Item &previous = _module->Items[*existing];
            if (previous.Docs.IsEmpty() && !item.Docs.IsEmpty())
            {
                previous.Docs = Move(item.Docs);
            }
            return previous;
        }
        _seen.Add(key, _module->Items.Count());
        // Overloads share the id until FinishItems; the first one stands for them here.
        _seen.TryAdd(item.Id, _module->Items.Count());
        return _module->Items.Add(Move(item));
    }

    Item *Find(StringView id)
    {
        const usize *index = _seen.Find(id);
        return index != nullptr ? &_module->Items[*index] : nullptr;
    }

    void VisitMacro(CXCursor cursor)
    {
        HeaderText *header = HeaderOf(cursor);
        if (header == nullptr)
        {
            return;
        }
        const Position at = SpellingOf(_clang.clang_getCursorLocation(cursor));
        const StringView text = header->Text.AsView();
        usize lineStart = at.Offset;
        while (lineStart > 0 && text[lineStart - 1] != '\n')
        {
            --lineStart;
        }
        const Preamble preamble = ScanPreamble(text, static_cast<uint32>(lineStart), true);
        if (preamble.Docs.IsEmpty())
        {
            return;
        }
        Item item;
        item.Language = "cpp";
        item.Kind = "macro";
        item.Name = TakeString(_clang.clang_getCursorSpelling(cursor));
        item.QualifiedName = item.Name;
        item.Id = Format("cpp:{0}", item.Name);
        item.File = header->Path;
        header->Lines->Locate(at.Offset, item.Line, item.Column);
        item.Docs = preamble.Docs;
        StringView rest = text.Substring(at.Offset + item.Name.Length());
        item.Signature = Format("#define {0}", item.Name);
        if (_clang.clang_Cursor_isMacroFunctionLike(cursor) && rest.StartsWith('('))
        {
            const usize close = rest.IndexOf(')');
            if (close != NotFound)
            {
                item.Signature.Append(CollapseWhitespace(rest.Left(close + 1)));
            }
        }
        if (_seen.Contains(item.Id))
        {
            return;
        }
        _seen.Add(item.Id, _module->Items.Count());
        _module->Items.Add(Move(item));
    }

    const CppModuleJob *_job;
    StringView _root;
    Module *_module;
    Reporter *_reporter;
    List<UniquePtr<HeaderText>> _headers;
    HashMap<String, usize> _headerIndex;
    HashMap<String, usize> _seen;
};
} // namespace

bool LoadLibClang(StringView path, Reporter &reporter)
{
    const String native(path);
    _library = PlatformLibrary::Load(native.CStr());
    if (!_library.IsValid())
    {
        reporter.Error(Format("libclang cannot be loaded from {0}", path));
        return false;
    }
#define MARLIN_DOC_CLANG_LOAD(name)                                                                                    \
    _clang.name = reinterpret_cast<decltype(_clang.name)>(PlatformLibrary::FindSymbol(_library, #name));               \
    if (_clang.name == nullptr)                                                                                        \
    {                                                                                                                  \
        reporter.Error(Format("{0} does not export " #name "; it is older than MarlinDoc needs", path));               \
        return false;                                                                                                  \
    }
    MARLIN_DOC_CLANG_FUNCTIONS(MARLIN_DOC_CLANG_LOAD)
#undef MARLIN_DOC_CLANG_LOAD
    return true;
}

void UnloadLibClang()
{
    if (_library.IsValid())
    {
        PlatformLibrary::Unload(_library);
        _library = {};
    }
}

bool ExtractCpp(const CppModuleJob &job, StringView root, Module &module, Reporter &reporter)
{
    CppVisitor extractor(job, root, module, reporter);
    return extractor.Run();
}
} // namespace Marlin::Doc
