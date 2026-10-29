#include "LanguageExtractor.h"

#include "LanguageFrontend/Parser.h"
#include "Memory/Arena.h"
#include "String/Format.h"
#include "Syntax.h"
#include "Text.h"

namespace Marlin::Doc
{
namespace
{
class LanguageVisitor
{
public:
    LanguageVisitor(const LanguageLibraryJob &job, StringView root, Module &module, Reporter &reporter)
        : _job(&job), _root(root), _module(&module), _reporter(&reporter)
    {
    }

    bool Run()
    {
        LanguageDialect dialect = LanguageDialect::Script;
        if (_job->Language == "mshader")
        {
            dialect = LanguageDialect::Shader;
        }
        else if (_job->Language == "mcommon")
        {
            dialect = LanguageDialect::Common;
        }
        uint32 id = 0;
        for (const String &path : _job->Files)
        {
            String text;
            if (!ReadFile(path, text))
            {
                _reporter->Error(Format("{0} cannot be read", path));
                return false;
            }
            SourceFile file(++id, path, Move(text), dialect);
            Arena memory;
            LanguageDiagnostics diagnostics;
            diagnostics.AddFile(file);
            const AstFile *tree = ParseFile(file, memory, diagnostics, {.SkipBodies = true});
            if (tree == nullptr)
            {
                continue;
            }
            _file = &file;
            _path = RelativePath(path, _root);
            FileDocs docs;
            docs.File = _path;
            docs.Docs = ScanFileDocs(file.GetText());
            for (AstNode *member : tree->Members)
            {
                // The comment of a file-scoped namespace, the first declaration, is about the file.
                if (const AstNamespaceDecl *ns = member->TryAs<AstNamespaceDecl>(); ns != nullptr && ns->FileScoped)
                {
                    if (docs.Docs.IsEmpty())
                    {
                        docs.Docs = ScanPreamble(file.GetText(), ns->Range.Begin, false).Docs;
                    }
                }
                Visit(member, String(), String());
            }
            if (!docs.Docs.IsEmpty())
            {
                _module->Files.Add(Move(docs));
            }
        }
        FinishItems(*_module);
        return true;
    }

private:
    StringView TextOf(SourceRange range) const
    {
        const StringView text = _file->GetText();
        return range.End > range.Begin && range.End <= text.Length()
                   ? text.Substring(range.Begin, range.End - range.Begin)
                   : StringView();
    }

    StringView TextOf(const AstNode *node) const
    {
        return node != nullptr ? TextOf(node->Range) : StringView();
    }

    static String Join(StringView scope, StringView name)
    {
        return scope.IsEmpty() ? String(name) : Format("{0}.{1}", scope, name);
    }

    /// The visibility of *decl*, or empty for one left out of the docs.
    static StringView VisibilityOf(const AstDecl &decl, bool alwaysPublic)
    {
        if (alwaysPublic || HasAnyFlags(decl.Modifiers, AstModifiers::Public))
        {
            return "public";
        }
        if (HasAnyFlags(decl.Modifiers, AstModifiers::Private))
        {
            return {};
        }
        if (HasAnyFlags(decl.Modifiers, AstModifiers::Protected))
        {
            return "protected";
        }
        return "internal";
    }

    void Visit(AstNode *node, const String &scope, const String &parentId, bool membersArePublic = false)
    {
        switch (node->Kind)
        {
        case AstKind::NamespaceDecl: {
            const auto *ns = node->As<AstNamespaceDecl>();
            const String qualified = Join(scope, ns->Name);
            for (AstNode *member : ns->Members)
            {
                Visit(member, qualified, parentId);
            }
            return;
        }
        case AstKind::StructDecl: {
            const auto *decl = node->As<AstStructDecl>();
            Item *item = Add(*decl, HasAnyFlags(decl->Modifiers, AstModifiers::Class) ? "class" : "struct", scope,
                             parentId, membersArePublic, true, false);
            if (item != nullptr)
            {
                AddBases(*item, decl->Interfaces);
                const String qualified = item->QualifiedName;
                const String id = item->Id;
                VisitMembers(decl->Members, qualified, id, false);
            }
            return;
        }
        case AstKind::InterfaceDecl: {
            const auto *decl = node->As<AstInterfaceDecl>();
            Item *item = Add(*decl, "interface", scope, parentId, membersArePublic, true, false);
            if (item != nullptr)
            {
                AddBases(*item, decl->Bases);
                const String qualified = item->QualifiedName;
                const String id = item->Id;
                VisitMembers(decl->Members, qualified, id, true);
            }
            return;
        }
        case AstKind::ExtensionDecl: {
            const auto *decl = node->As<AstExtensionDecl>();
            Item *item = Add(*decl, "extension", scope, parentId, membersArePublic, true, false);
            if (item != nullptr)
            {
                item->Extends = CollapseWhitespace(TextOf(decl->ExtendedType));
                AddBases(*item, decl->Interfaces);
                // Members belong to the extended type's name, so `Vector.Luma` finds them.
                String extended = item->Extends;
                const usize generic = extended.AsView().IndexOf('<');
                if (generic != NotFound)
                {
                    extended.Remove(generic);
                }
                const String id = item->Id;
                VisitMembers(decl->Members, Join(scope, extended), id, false);
            }
            return;
        }
        case AstKind::EnumDecl: {
            const auto *decl = node->As<AstEnumDecl>();
            Item *item = Add(*decl, "enum", scope, parentId, membersArePublic, true, false);
            if (item != nullptr)
            {
                const String qualified = item->QualifiedName;
                const String id = item->Id;
                for (AstEnumMemberDecl *member : decl->Members)
                {
                    Add(*member, "enumerator", qualified, id, true, false, false);
                }
            }
            return;
        }
        case AstKind::FunctionDecl: {
            const auto *decl = node->As<AstFunctionDecl>();
            Item *item =
                Add(*decl, parentId.IsEmpty() ? "function" : "method", scope, parentId, membersArePublic, false, true);
            if (item != nullptr)
            {
                SetParameters(*item, decl->Parameters);
            }
            return;
        }
        case AstKind::ConstructorDecl: {
            const auto *decl = node->As<AstConstructorDecl>();
            Item *item = Add(*decl, "constructor", scope, parentId, membersArePublic, false, true);
            if (item != nullptr)
            {
                SetParameters(*item, decl->Parameters);
            }
            return;
        }
        case AstKind::OperatorDecl: {
            const auto *decl = node->As<AstOperatorDecl>();
            Item *item = Add(*decl, "operator", scope, parentId, membersArePublic, false, true);
            if (item != nullptr)
            {
                SetParameters(*item, decl->Parameters);
            }
            return;
        }
        case AstKind::PropertyDecl: {
            const auto *decl = node->As<AstPropertyDecl>();
            Item *item =
                Add(*decl, decl->IsIndexer ? "indexer" : "property", scope, parentId, membersArePublic, false, true);
            if (item != nullptr)
            {
                StringBuilder<64> accessors;
                accessors.Append(" {");
                if (decl->HasGetter || !decl->Setter.Present)
                {
                    accessors.Append(" get;");
                }
                if (decl->Setter.Present)
                {
                    accessors.Append(decl->SetterIsInit ? " init;" : " set;");
                }
                accessors.Append(" }");
                item->Signature.Append(accessors.AsView());
            }
            return;
        }
        case AstKind::FieldDecl: {
            const auto *decl = node->As<AstFieldDecl>();
            const bool constant = HasAnyFlags(decl->Modifiers, AstModifiers::Const);
            Add(*decl, constant ? "constant" : "field", scope, parentId, membersArePublic, false, false);
            return;
        }
        case AstKind::DelegateDecl: {
            const auto *decl = node->As<AstDelegateDecl>();
            Item *item = Add(*decl, "delegate", scope, parentId, membersArePublic, false, false);
            if (item != nullptr)
            {
                SetParameters(*item, decl->Parameters);
            }
            return;
        }
        case AstKind::TypeDecl: {
            const auto *decl = node->As<AstTypeDecl>();
            Add(*decl, "associated-type", scope, parentId, membersArePublic, false, false);
            return;
        }
        case AstKind::UsingDecl: {
            const auto *decl = node->As<AstUsingDecl>();
            // Only an alias with a visibility is a member of its namespace (MarlinLanguage §1.1).
            if (!decl->Alias.IsEmpty() && HasAnyFlags(decl->Modifiers, AstModifiers::Public | AstModifiers::Internal))
            {
                Item *item = Add(*decl, "alias", scope, parentId, false, false, false);
                if (item != nullptr)
                {
                    item->Name = decl->Alias;
                    item->QualifiedName = Join(scope, decl->Alias);
                    item->Id = Format("{0}:{1}", _job->Language, item->QualifiedName);
                }
            }
            return;
        }
        default:
            return;
        }
    }

    void VisitMembers(Span<AstDecl *> members, const String &scope, const String &parentId, bool arePublic)
    {
        for (AstDecl *member : members)
        {
            Visit(member, scope, parentId, arePublic);
        }
    }

    Item *Add(const AstDecl &decl, StringView kind, const String &scope, const String &parentId, bool alwaysPublic,
              bool type, bool function)
    {
        const StringView visibility = VisibilityOf(decl, alwaysPublic);
        if (visibility.IsEmpty())
        {
            return nullptr;
        }
        Item item;
        item.Language = _job->Language;
        item.Kind = kind;
        item.Name = kind == "extension" ? Format("extension {0}", decl.Name) : String(decl.Name);
        if (kind == "constructor")
        {
            item.Name = "this";
        }
        item.QualifiedName = Join(scope, item.Name);
        item.Parent = parentId;
        item.Visibility = visibility;
        item.File = _path;
        _file->GetLineAndColumn(decl.NameRange.IsValid() ? decl.NameRange.Begin : decl.Range.Begin, item.Line,
                                item.Column);
        uint32 begin = decl.Range.Begin;
        for (const AstAttribute &attribute : decl.Attributes)
        {
            item.Attributes.Add(CollapseWhitespace(TextOf(attribute.Range)));
            begin = attribute.Range.End > begin ? attribute.Range.End : begin;
        }
        const StringView text = _file->GetText();
        while (begin < decl.Range.End && (Ascii::IsWhiteSpace(text[begin]) || text[begin] == ']'))
        {
            ++begin;
        }
        if (decl.Range.End > begin)
        {
            item.Signature = CutDeclaration(text.Substring(begin, decl.Range.End - begin), function, type, false);
        }
        item.Docs = ScanPreamble(text, decl.Range.Begin, false).Docs;
        item.Id = Format("{0}:{1}", item.Language, item.QualifiedName);
        if (kind == "extension")
        {
            // Two extensions of one type are told apart by where they are.
            item.Id = Format("{0}@{1}:{2}", item.Id, _path, item.Line);
        }
        return &_module->Items.Add(Move(item));
    }

    void AddBases(Item &item, Span<AstNode *> bases)
    {
        for (const AstNode *base : bases)
        {
            item.Bases.Add(CollapseWhitespace(TextOf(base)));
        }
    }

    void SetParameters(Item &item, Span<AstParameterDecl *> parameters)
    {
        item.HasParameters = true;
        for (usize i = 0; i < parameters.Count(); ++i)
        {
            if (i > 0)
            {
                item.Parameters.Append(',');
            }
            item.Parameters.Append(NormalizeType(TextOf(parameters[i]->Type)));
        }
    }

    const LanguageLibraryJob *_job;
    StringView _root;
    Module *_module;
    Reporter *_reporter;
    const SourceFile *_file = nullptr;
    String _path;
};
} // namespace

bool ExtractLanguage(const LanguageLibraryJob &job, StringView root, Module &module, Reporter &reporter)
{
    LanguageVisitor extractor(job, root, module, reporter);
    return extractor.Run();
}
} // namespace Marlin::Doc
