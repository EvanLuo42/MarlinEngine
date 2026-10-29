#include "LanguageFrontend/NameDependencies.h"

#include "Hash/Hash.h"
#include "Hash/Xxh3.h"
#include "LanguageFrontend/Ast.h"
#include "LanguageFrontend/Diagnostics.h"
#include "LanguageFrontend/Parser.h"
#include "Memory/Arena.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
bool ContainsSorted(Span<const uint64> sorted, uint64 value)
{
    usize low = 0;
    usize high = sorted.Count();
    while (low < high)
    {
        const usize middle = low + (high - low) / 2;
        if (sorted[middle] < value)
        {
            low = middle + 1;
        }
        else
        {
            high = middle;
        }
    }
    return low < sorted.Count() && sorted[low] == value;
}

void SortUnique(List<uint64> &values)
{
    values.Sort();
    usize count = 0;
    for (usize i = 0; i < values.Count(); ++i)
    {
        if (count == 0 || values[count - 1] != values[i])
        {
            values[count++] = values[i];
        }
    }
    values.Resize(count);
}

String Qualify(StringView container, StringView name)
{
    if (container.IsEmpty())
    {
        return String(name);
    }
    StringBuilder<256> text;
    text.Append(container).Append('.').Append(name);
    return text.ToString();
}

void DeclareType(StringView container, Span<AstDecl *> members, LanguageDeclaredNames &names);

/// Follows Sema::DeclareMembers: what it adds to which container.
void Declare(StringView container, Span<AstNode *> members, LanguageDeclaredNames &names)
{
    for (AstNode *member : members)
    {
        if (member->Kind == AstKind::UsingDecl && member->As<AstUsingDecl>()->Modifiers == AstModifiers::None)
        {
            continue;
        }
        if (member->Kind == AstKind::NamespaceDecl)
        {
            auto *ns = member->As<AstNamespaceDecl>();
            String target(container);
            for (const StringView part : ns->Name.Split('.'))
            {
                names.Namespaces.Add(HashLanguageName(target.AsView(), part));
                target = Qualify(target.AsView(), part);
            }
            Declare(target.AsView(), ns->Members, names);
            continue;
        }
        auto *decl = static_cast<AstDecl *>(member);
        if (decl->Kind == AstKind::ExtensionDecl)
        {
            names.Extensions = true;
            continue;
        }
        names.Names.Add(HashLanguageName(container, decl->Name));
        const String qualified = Qualify(container, decl->Name);
        if (auto *structDecl = decl->TryAs<AstStructDecl>())
        {
            DeclareType(qualified.AsView(), structDecl->Members, names);
        }
        else if (auto *interfaceDecl = decl->TryAs<AstInterfaceDecl>())
        {
            DeclareType(qualified.AsView(), interfaceDecl->Members, names);
        }
        else if (auto *enumDecl = decl->TryAs<AstEnumDecl>())
        {
            for (AstEnumMemberDecl *enumMember : enumDecl->Members)
            {
                names.Names.Add(HashLanguageName(qualified.AsView(), enumMember->Name));
            }
        }
    }
}

void DeclareType(StringView container, Span<AstDecl *> members, LanguageDeclaredNames &names)
{
    List<AstNode *> nodes;
    for (AstDecl *member : members)
    {
        nodes.Add(member);
    }
    Declare(container, nodes.AsSpan(), names);
}
} // namespace

void LanguageNameDependencies::Merge(const LanguageNameDependencies &other)
{
    Names.AddRange(other.Names.AsSpan());
    NamespaceNames.AddRange(other.NamespaceNames.AsSpan());
    SortUnique(Names);
    SortUnique(NamespaceNames);
    Extensions = Extensions || other.Extensions;
}

uint64 HashLanguageName(StringView container, StringView name)
{
    return HashCombine(Xxh3Hash64(container.Data(), container.Length()), Xxh3Hash64(name.Data(), name.Length()));
}

void GetDeclaredNames(StringView path, String text, LanguageDeclaredNames &names)
{
    LanguageDialect dialect = LanguageDialect::Common;
    (void)GetDialectOf(path, dialect);
    SourceFile file(1, path, Move(text), dialect);
    Arena arena;
    LanguageDiagnostics diagnostics;
    diagnostics.AddFile(file);
    const AstFile *tree = ParseFile(file, arena, diagnostics, {.SkipBodies = true});
    Declare({}, tree->Members, names);
    SortUnique(names.Names);
    SortUnique(names.Namespaces);
}

bool AffectsNames(const LanguageDeclaredNames &declared, const LanguageNameDependencies &dependencies)
{
    if (declared.Extensions && dependencies.Extensions)
    {
        return true;
    }
    for (const uint64 name : declared.Names)
    {
        if (ContainsSorted(dependencies.Names.AsSpan(), name) ||
            ContainsSorted(dependencies.NamespaceNames.AsSpan(), name))
        {
            return true;
        }
    }
    for (const uint64 name : declared.Namespaces)
    {
        if (ContainsSorted(dependencies.Names.AsSpan(), name))
        {
            return true;
        }
    }
    return false;
}
} // namespace Marlin
