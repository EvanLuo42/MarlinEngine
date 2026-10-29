#include "Sema/Lowering.h"
#include "Sema/Sema.h"

#include "Hash/Hash.h"
#include "Hash/Xxh3.h"
#include "Sema/SemaBuiltins.h"
#include "Sema/SemaHelpers.h"

namespace Marlin
{
LanguageCompilation::LanguageCompilation(ISourceProvider &sources, const LanguageCompileOptions &options)
    : _sema(MakeUnique<Sema>(sources, options))
{
}

LanguageCompilation::~LanguageCompilation() = default;

bool LanguageCompilation::Load()
{
    return _sema->Load();
}

LanguageDiagnostics &LanguageCompilation::GetDiagnostics()
{
    return _sema->Diagnostics;
}

bool LanguageCompilation::CompileEntryPoint(StringView qualifiedName, MirModule &module)
{
    return _sema->CompileEntryPoint(qualifiedName, module);
}

bool LanguageCompilation::CheckScriptPackage(StringView package)
{
    return _sema->CheckScriptPackage(package);
}

bool LanguageCompilation::CompileScriptPackage(StringView package, MirModule &module)
{
    return _sema->CompileScriptPackage(package, module);
}

void LanguageCompilation::GetDependencies(List<const SourceFile *> &files) const
{
    _sema->GetDependencies(files);
}

void LanguageCompilation::GetNameDependencies(LanguageNameDependencies &names) const
{
    _sema->GetNameDependencies(names);
}

void LanguageCompilation::GetSourceFiles(List<const SourceFile *> &files) const
{
    _sema->GetSourceFiles(files);
}

void LanguageCompilation::GetQueriedFeatures(List<String> &features) const
{
    for (const String &feature : _sema->QueriedFeatures)
    {
        features.Add(feature);
    }
    features.Sort([](const String &a, const String &b) { return a.AsView().CompareTo(b.AsView()) < 0; });
}

void LanguageCompilation::GetEntryPoints(StringView folder, List<String> &names) const
{
    _sema->GetEntryPoints(folder, names);
}

void LanguageCompilation::GetMaterials(StringView folder, List<String> &names) const
{
    _sema->GetMaterials(folder, names);
}

bool LanguageCompilation::ReflectMaterial(LanguageMaterialReflection &reflection)
{
    return _sema->ReflectMaterial(reflection);
}

bool SemaGenericArgument::operator==(const SemaGenericArgument &other) const
{
    if (Type != other.Type)
    {
        return false;
    }
    if ((Value == nullptr) != (other.Value == nullptr))
    {
        return false;
    }
    return Value == nullptr || (Value->Bits == other.Value->Bits && Value->Type == other.Value->Type);
}

namespace
{
uint64 HashArguments(Span<const SemaGenericArgument> arguments)
{
    uint64 hash = 0;
    for (const SemaGenericArgument &argument : arguments)
    {
        hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(argument.Type)));
        hash = HashCombine(hash, argument.Value != nullptr ? HashInteger(argument.Value->Bits) : 0);
    }
    return hash;
}

bool EqualArguments(Span<const SemaGenericArgument> a, Span<const SemaGenericArgument> b)
{
    if (a.Count() != b.Count())
    {
        return false;
    }
    for (usize i = 0; i < a.Count(); ++i)
    {
        if (!(a[i] == b[i]))
        {
            return false;
        }
    }
    return true;
}

SemaVisibility VisibilityOf(AstModifiers modifiers)
{
    if (HasAnyFlags(modifiers, AstModifiers::Public))
    {
        return SemaVisibility::Public;
    }
    if (HasAnyFlags(modifiers, AstModifiers::Private))
    {
        return SemaVisibility::Private;
    }
    if (HasAnyFlags(modifiers, AstModifiers::Protected))
    {
        return SemaVisibility::Protected;
    }
    return SemaVisibility::Internal;
}

bool IsOverloadable(SemaSymbolKind kind)
{
    return kind == SemaSymbolKind::Function || kind == SemaSymbolKind::Constructor ||
           kind == SemaSymbolKind::Operator || kind == SemaSymbolKind::BuiltinFunction;
}
} // namespace

uint64 Sema::StructKey::GetHash() const
{
    return HashCombine(HashInteger(reinterpret_cast<uintptr>(Symbol)), HashArguments(Arguments));
}

bool Sema::StructKey::operator==(const StructKey &other) const
{
    return Symbol == other.Symbol && EqualArguments(Arguments, other.Arguments);
}

uint64 Sema::FunctionKey::GetHash() const
{
    return HashCombine(HashCombine(HashInteger(reinterpret_cast<uintptr>(Symbol)),
                                   HashInteger(reinterpret_cast<uintptr>(Owner) + static_cast<uintptr>(Accessor))),
                       HashArguments(Arguments));
}

bool Sema::FunctionKey::operator==(const FunctionKey &other) const
{
    return Symbol == other.Symbol && Owner == other.Owner && Accessor == other.Accessor &&
           EqualArguments(Arguments, other.Arguments);
}

uint64 Sema::StaticKey::GetHash() const
{
    return HashCombine(HashInteger(reinterpret_cast<uintptr>(Field)), HashInteger(reinterpret_cast<uintptr>(Owner)));
}

Sema::Sema(ISourceProvider &sources, const LanguageCompileOptions &options)
    : Types(Memory), Options(&options), _sources(&sources)
{
    _global = NewSymbol(SemaSymbolKind::Namespace, {}, nullptr);
    _global->Visibility = SemaVisibility::Public;
    _marlin = GetOrAddNamespace(_global, "Marlin");
}

Sema::~Sema() = default;

SemaSymbol *Sema::NewSymbol(SemaSymbolKind kind, StringView name, SemaSymbol *parent)
{
    _symbols.Add(MakeUnique<SemaSymbol>());
    SemaSymbol *symbol = _symbols.Last().Get();
    symbol->Kind = kind;
    symbol->Name = name;
    symbol->Parent = parent;
    return symbol;
}

SemaSymbol *Sema::GetOrAddNamespace(SemaSymbol *parent, StringView name)
{
    for (SemaSymbol *existing : GetMembers(parent, name))
    {
        if (existing->Kind == SemaSymbolKind::Namespace)
        {
            return existing;
        }
    }
    SemaSymbol *ns = NewSymbol(SemaSymbolKind::Namespace, name, parent);
    ns->Visibility = SemaVisibility::Public;
    AddMember(parent, ns);
    return ns;
}

void Sema::AddMember(SemaSymbol *container, SemaSymbol *member)
{
    List<SemaSymbol *> &existing = container->Members.FindOrAdd(member->Name);
    for (SemaSymbol *other : existing)
    {
        const bool bothNamespaces =
            other->Kind == SemaSymbolKind::Namespace && member->Kind == SemaSymbolKind::Namespace;
        const bool typesOfOtherArity = other->IsAggregate() && member->IsAggregate() &&
                                       GenericParametersOf(other->Decl).Count() !=
                                           GenericParametersOf(member->Decl).Count();
        if (!bothNamespaces && !typesOfOtherArity && !(IsOverloadable(other->Kind) && IsOverloadable(member->Kind)))
        {
            const SourceRange range = member->Decl != nullptr ? member->Decl->NameRange : SourceRange{};
            Diagnostics.Error(DiagnosticCode::DuplicateDefinition, range, "the name `{0}` is defined more than once",
                              member->Name)
                .WithLabel("redefined here")
                .WithSecondaryLabel(other->Decl != nullptr ? other->Decl->NameRange : SourceRange{},
                                    "first defined here");
            return;
        }
    }
    existing.Add(member);
    container->Ordered.Add(member);
}

Span<SemaSymbol *const> Sema::GetMembers(const SemaSymbol *container, StringView name) const
{
    const List<SemaSymbol *> *members = container->Members.Find(name);
    if (_recordNames && container->Kind != SemaSymbolKind::Extension)
    {
        const uint64 *containerName = _containerNames.Find(container);
        if (containerName == nullptr)
        {
            const String qualified = GetQualifiedName(container);
            containerName = &_containerNames.Add(container, Xxh3Hash64(qualified.Data(), qualified.Length()));
        }
        bool foundNamespace = false;
        for (const SemaSymbol *member : members != nullptr ? members->AsSpan() : Span<SemaSymbol *const>())
        {
            foundNamespace = foundNamespace || member->Kind == SemaSymbolKind::Namespace;
        }
        // HashLanguageName(container, name), with the container's part computed once.
        const uint64 key = HashCombine(*containerName, Xxh3Hash64(name.Data(), name.Length()));
        _queriedNames.FindOrAdd(key) |= foundNamespace;
    }
    return members != nullptr ? members->AsSpan() : Span<SemaSymbol *const>();
}

SemaSymbol *Sema::GetSymbolOf(const AstDecl *decl) const
{
    SemaSymbol *const *symbol = _declarations.Find(decl);
    return symbol != nullptr ? *symbol : nullptr;
}

SemaSymbol *Sema::FindQualified(StringView name) const
{
    SemaSymbol *current = _global;
    for (const StringView part : name.Split('.'))
    {
        const Span<SemaSymbol *const> members = GetMembers(current, part);
        if (members.IsEmpty())
        {
            return nullptr;
        }
        current = members[0];
    }
    return current;
}

uint32 Sema::GetPackage(StringView path)
{
    StringView rest = path.RemoveStart(path.StartsWith('/') ? 1 : 0);
    const usize end = rest.IndexOf('/');
    const StringView root = end == StringView::NotFound ? rest : rest.Left(end);
    if (const uint32 *found = _packages.Find(root))
    {
        return *found;
    }
    const auto id = static_cast<uint32>(_packages.Count() + 1);
    _packages.Add(root, id);
    return id;
}

bool Sema::Load()
{
    List<String> paths;
    _sources->ListFiles([&](StringView path) { paths.Add(String(path.Data(), path.Length())); });
    paths.Sort([](const String &a, const String &b) { return a.AsView().CompareTo(b.AsView()) < 0; });
    bool ok = true;
    for (const String &path : paths)
    {
        LanguageDialect dialect;
        if (!GetDialectOf(path.AsView(), dialect) || !IsInSourceSet(Options->Language, dialect))
        {
            continue;
        }
        LanguageDialect folder;
        if (!GetFolderDialectOf(path.AsView(), folder) || folder != dialect)
        {
            Diagnostics.Error(DiagnosticCode::UnexpectedToken, {}, "'{0}' is outside the folder of its dialect",
                              path.AsView())
                .WithNote(Format("`{0}` files go in Language/{1}/ of their mount root (§1.5)",
                                 path.AsView().Substring(path.AsView().LastIndexOf('.')), GetDialectFolder(dialect))
                              .AsView());
            ok = false;
            continue;
        }
        String text;
        if (!_sources->ReadFile(path.AsView(), text))
        {
            Diagnostics.Error(DiagnosticCode::UnexpectedToken, {}, "cannot read '{0}'", path.AsView());
            ok = false;
            continue;
        }
        const auto id = static_cast<uint32>(_files.Count() + 1);
        _files.Add(MakeUnique<SourceFile>(id, path.AsView(), Move(text), dialect));
        SourceFile &file = *_files.Last();
        Diagnostics.AddFile(file);
        AstFile *tree = ParseFile(file, Memory, Diagnostics, {.SkipBodies = true});
        _trees.Add(tree);
        if (IsCoreLibraryPath(file.GetPath()))
        {
            _coreFolders |= 1u << static_cast<uint32>(dialect);
        }
        const uint32 package = GetPackage(file.GetPath());
        GetUsingScope(tree).File = tree;
        DeclareMembers(_global, tree->Members, tree, package);
    }
    ResolveUsings();
    const LanguageDialect needed[] = {LanguageDialect::Common, Options->Language};
    for (const LanguageDialect dialect : needed)
    {
        if ((_coreFolders & (1u << static_cast<uint32>(dialect))) == 0)
        {
            Diagnostics.Error(DiagnosticCode::UndefinedName, {}, "the core library is missing from the source set")
                .WithNote(Format("the compiler needs /Engine/Language/{0}/Core/ (§9.10, MarlinScript §10)",
                                 GetDialectFolder(dialect))
                              .AsView());
            ok = false;
        }
    }
    _recordNames = true;
    return ok && !Diagnostics.HasErrors();
}

Sema::UsingScope &Sema::GetUsingScope(const void *key)
{
    UniquePtr<UsingScope> &scope = _usings.FindOrAdd(key);
    if (!scope)
    {
        scope = MakeUnique<UsingScope>();
    }
    return *scope;
}

void Sema::DeclareMembers(SemaSymbol *container, Span<AstNode *> members, const AstFile *file, uint32 package)
{
    for (AstNode *member : members)
    {
        switch (member->Kind)
        {
        case AstKind::UsingDecl:
            if (member->As<AstUsingDecl>()->Modifiers != AstModifiers::None)
            {
                goto declare;
            }
            {
            auto *usingDecl = member->As<AstUsingDecl>();
            const void *key = usingDecl->Parent != nullptr ? static_cast<const void *>(usingDecl->Parent)
                                                           : static_cast<const void *>(file);
            UsingScope &scope = GetUsingScope(key);
            scope.File = file;
            scope.Declarations.Add(usingDecl);
            }
            break;
        case AstKind::NamespaceDecl:
        {
            auto *ns = member->As<AstNamespaceDecl>();
            SemaSymbol *target = container;
            for (const StringView part : ns->Name.Split('.'))
            {
                target = GetOrAddNamespace(target, part);
            }
            _declarations.Add(ns, target);
            GetUsingScope(ns).File = file;
            DeclareMembers(target, ns->Members, file, package);
            break;
        }
        default:
        declare:
        {
            auto *decl = static_cast<AstDecl *>(member);
            SemaSymbolKind kind;
            switch (member->Kind)
            {
            case AstKind::StructDecl:
                kind = HasAnyFlags(decl->Modifiers, AstModifiers::Class) ? SemaSymbolKind::Class : SemaSymbolKind::Struct;
                break;
            case AstKind::DelegateDecl:
                kind = SemaSymbolKind::Delegate;
                break;
            case AstKind::InterfaceDecl:
                kind = SemaSymbolKind::Interface;
                break;
            case AstKind::EnumDecl:
                kind = SemaSymbolKind::Enum;
                break;
            case AstKind::FunctionDecl:
                kind = SemaSymbolKind::Function;
                break;
            case AstKind::FieldDecl:
                kind = SemaSymbolKind::Field;
                break;
            case AstKind::PropertyDecl:
                kind = SemaSymbolKind::Property;
                break;
            case AstKind::OperatorDecl:
                kind = SemaSymbolKind::Operator;
                break;
            case AstKind::ConstructorDecl:
                kind = SemaSymbolKind::Constructor;
                break;
            case AstKind::UsingDecl:
                kind = SemaSymbolKind::Alias;
                break;
            case AstKind::TypeDecl:
                kind = SemaSymbolKind::AssociatedType;
                break;
            case AstKind::ExtensionDecl:
                kind = SemaSymbolKind::Extension;
                break;
            default:
                continue;
            }
            if (!CheckMemberPlacement(container, decl))
            {
                continue;
            }
            SemaSymbol *symbol = NewSymbol(kind, decl->Name, container);
            symbol->Decl = decl;
            symbol->File = file;
            symbol->Package = package;
            symbol->Visibility = container->Kind == SemaSymbolKind::Interface ? SemaVisibility::Public
                                                                              : VisibilityOf(decl->Modifiers);
            symbol->IsCore = IsCoreLibraryPath(file->File->GetPath());
            if (const AstAttribute *builtin = FindAttribute(decl->Attributes, "Builtin"))
            {
                const AstLiteralExpr *literal =
                    builtin->Arguments.Count() == 1 ? builtin->Arguments[0].Value->TryAs<AstLiteralExpr>() : nullptr;
                const auto id = literal != nullptr && literal->Literal.Is(TokenKind::StringLiteral)
                                    ? FindBuiltinType(DecodeStringLiteral(*file->File, literal->Literal).AsView())
                                    : BuiltinTypeId::None;
                if (!symbol->IsCore || id == BuiltinTypeId::None)
                {
                    Diagnostics.Error(DiagnosticCode::InvalidAttribute, builtin->Range,
                                      "`[Builtin]` names a magic type of the core library")
                        .WithNote(symbol->IsCore ? "the name is not a known magic type"
                                                 : "only the core library may declare built-ins");
                }
                if (id == BuiltinTypeId::Object)
                {
                    _objectSymbol = symbol;
                }
                symbol->BuiltinId = static_cast<uint32>(id);
                if (id != BuiltinTypeId::None)
                {
                    _magicSymbols.Set(symbol->BuiltinId, symbol);
                }
            }
            _declarations.Add(decl, symbol);
            if (auto *extension = decl->TryAs<AstExtensionDecl>())
            {
                // An extension is found through the types it extends, never by a name of its namespace.
                _extensions.Add(symbol);
                DeclareTypeMembers(symbol, extension->Members, file, package);
                break;
            }
            AddMember(container, symbol);
            if (auto *structDecl = decl->TryAs<AstStructDecl>())
            {
                DeclareTypeMembers(symbol, structDecl->Members, file, package);
                if (kind == SemaSymbolKind::Class && GetMembers(symbol, symbol->Name).IsEmpty() &&
                    !HasAnyFlags(decl->Modifiers, AstModifiers::Static))
                {
                    AstNode *constructor[] = {MakeDefaultConstructor(structDecl)};
                    DeclareMembers(symbol, Span<AstNode *>(constructor), file, package);
                }
            }
            else if (auto *interfaceDecl = decl->TryAs<AstInterfaceDecl>())
            {
                DeclareTypeMembers(symbol, interfaceDecl->Members, file, package);
            }
            else if (auto *enumDecl = decl->TryAs<AstEnumDecl>())
            {
                for (AstEnumMemberDecl *enumMember : enumDecl->Members)
                {
                    SemaSymbol *memberSymbol = NewSymbol(SemaSymbolKind::EnumMember, enumMember->Name, symbol);
                    memberSymbol->Decl = enumMember;
                    memberSymbol->File = file;
                    memberSymbol->Package = package;
                    memberSymbol->Visibility = SemaVisibility::Public;
                    _declarations.Add(enumMember, memberSymbol);
                    AddMember(symbol, memberSymbol);
                }
            }
            break;
        }
        }
    }
}

bool Sema::CheckMemberPlacement(const SemaSymbol *container, const AstDecl *decl)
{
    if (container->Kind == SemaSymbolKind::Extension)
    {
        const bool isField = decl->Kind == AstKind::FieldDecl &&
                             !HasAnyFlags(decl->Modifiers, AstModifiers::Static | AstModifiers::Const);
        StringView what;
        StringView help = "an extension adds methods, properties, static members, operators and interface "
                          "implementations (§3.5)";
        switch (decl->Kind)
        {
        case AstKind::ConstructorDecl:
            what = "constructors";
            help = "declare a static method that returns `this`";
            break;
        case AstKind::StructDecl:
        case AstKind::InterfaceDecl:
        case AstKind::EnumDecl:
        case AstKind::DelegateDecl:
            what = "types";
            break;
        default:
            what = isField ? StringView("fields") : StringView();
            break;
        }
        if (!what.IsEmpty())
        {
            Diagnostics
                .Error(DiagnosticCode::InvalidExtensionMember, decl->NameRange, "an extension cannot add {0}", what)
                .WithLabel("declared in an extension")
                .WithHelp(help);
            return false;
        }
    }
    if (const auto *type = decl->TryAs<AstTypeDecl>())
    {
        const bool inInterface = container->Kind == SemaSymbolKind::Interface;
        if (inInterface && type->Target != nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidStatement, type->Target->Range,
                              "an interface declares an associated type without binding it")
                .WithHelp(Format("write `type {0};` or `type {0} : IBound;`; each implementation binds it", type->Name)
                              .AsView());
            return false;
        }
        if (!inInterface && (type->Target == nullptr || !type->Bounds.IsEmpty()))
        {
            Diagnostics.Error(DiagnosticCode::InvalidStatement, decl->NameRange,
                              "an associated type is bound with `type {0} = Type;`", type->Name)
                .WithNote("`type Name : IBound;` declares the requirement in an interface (§3.5)");
            return false;
        }
    }
    return true;
}

void Sema::DeclareTypeMembers(SemaSymbol *type, Span<AstDecl *> members, const AstFile *file, uint32 package)
{
    List<AstNode *> nodes;
    for (AstDecl *member : members)
    {
        nodes.Add(member);
    }
    DeclareMembers(type, nodes.AsSpan(), file, package);
}

void Sema::ResolveUsings()
{
    for (auto &[key, scope] : _usings)
    {
        for (AstUsingDecl *usingDecl : scope->Declarations)
        {
            if (!usingDecl->Alias.IsEmpty())
            {
                if (!scope->Aliases.TryAdd(usingDecl->Alias, usingDecl))
                {
                    Diagnostics.Error(DiagnosticCode::DuplicateDefinition, usingDecl->NameRange,
                                      "the alias '{0}' is already defined", usingDecl->Alias);
                }
                continue;
            }
            const StringView name = TextOf(usingDecl->Target->Range);
            SemaSymbol *target = FindQualified(name);
            for (const AstDecl *parent = usingDecl->Parent; target == nullptr && parent != nullptr;
                 parent = parent->Parent)
            {
                if (SemaSymbol *ns = GetSymbolOf(parent))
                {
                    SemaSymbol *current = ns;
                    for (const StringView part : name.Split('.'))
                    {
                        const Span<SemaSymbol *const> members = current != nullptr ? GetMembers(current, part)
                                                                                   : Span<SemaSymbol *const>();
                        current = members.IsEmpty() ? nullptr : members[0];
                    }
                    target = current;
                }
            }
            if (usingDecl->IsStatic)
            {
                if (target == nullptr ||
                    (target->Kind != SemaSymbolKind::Struct && target->Kind != SemaSymbolKind::Class &&
                     target->Kind != SemaSymbolKind::Enum && target->Kind != SemaSymbolKind::BuiltinStatic))
                {
                    Diagnostics.Error(DiagnosticCode::UndefinedName, usingDecl->Target->Range,
                                      "'{0}' is not a type", name);
                    continue;
                }
                scope->StaticTypes.Add(target);
            }
            else
            {
                if (target == nullptr || target->Kind != SemaSymbolKind::Namespace)
                {
                    Diagnostics.Error(DiagnosticCode::UndefinedName, usingDecl->Target->Range,
                                      "the namespace '{0}' does not exist", name);
                    continue;
                }
                scope->Namespaces.Add(target);
            }
        }
    }
}

void Sema::LookupIn(SemaSymbol *container, StringView name, SemaLookup &result, const SemaContext &context)
{
    for (SemaSymbol *symbol : GetMembers(container, name))
    {
        if (IsAccessible(context, symbol) && !result.Symbols.Contains(symbol))
        {
            result.Symbols.Add(symbol);
        }
    }
}

void Sema::LookupUsings(const void *key, StringView name, SemaLookup &result, const SemaContext &context)
{
    const UniquePtr<UsingScope> *found = _usings.Find(key);
    if (found == nullptr)
    {
        return;
    }
    const UsingScope &scope = **found;
    if (AstUsingDecl *const *alias = scope.Aliases.Find(name))
    {
        result.Alias = *alias;
        result.AliasFile = scope.File;
        return;
    }
    SemaSymbol *firstOwner = nullptr;
    for (SemaSymbol *ns : scope.Namespaces)
    {
        const usize before = result.Symbols.Count();
        LookupIn(ns, name, result, context);
        if (result.Symbols.Count() == before)
        {
            continue;
        }
        if (firstOwner != nullptr)
        {
            bool allFunctions = true;
            for (const SemaSymbol *symbol : result.Symbols)
            {
                allFunctions = allFunctions && IsOverloadable(symbol->Kind);
            }
            result.Ambiguous = result.Ambiguous || !allFunctions;
        }
        firstOwner = ns;
    }
    for (SemaSymbol *type : scope.StaticTypes)
    {
        for (SemaSymbol *symbol : GetMembers(type, name))
        {
            if (symbol->IsStatic() || symbol->Kind == SemaSymbolKind::EnumMember ||
                type->Kind == SemaSymbolKind::BuiltinStatic)
            {
                result.Symbols.Add(symbol);
            }
        }
    }
}

void Sema::Lookup(const SemaContext &context, StringView name, SemaLookup &result)
{
    for (const SemaGenericBinding &binding : context.Generics)
    {
        if (binding.Name == name)
        {
            result.Generic = &binding.Argument;
            return;
        }
    }
    for (SemaSymbol *type = context.TypeSymbol; type != nullptr && type->IsTypeScope(); type = type->Parent)
    {
        LookupIn(type, name, result, context);
        if (!result.IsEmpty())
        {
            return;
        }
        if (context.Owner != nullptr && context.Owner->Symbol == type && type->Kind == SemaSymbolKind::Class &&
            context.Owner->BasesResolved)
        {
            List<SemaSymbol *> inherited;
            GetInstanceMembers(context.Owner, name, inherited);
            for (SemaSymbol *symbol : inherited)
            {
                if (IsAccessible(context, symbol))
                {
                    result.Symbols.Add(symbol);
                }
            }
            if (!result.IsEmpty())
            {
                return;
            }
        }
    }

    const AstDecl *block = context.Decl;
    while (block != nullptr && block->Kind != AstKind::NamespaceDecl)
    {
        block = block->Parent;
    }
    while (block != nullptr)
    {
        SemaSymbol *ns = GetSymbolOf(block);
        const AstDecl *outer = block->Parent;
        while (outer != nullptr && outer->Kind != AstKind::NamespaceDecl)
        {
            outer = outer->Parent;
        }
        SemaSymbol *stop = outer != nullptr ? GetSymbolOf(outer) : _global;
        LookupIn(ns, name, result, context);
        if (!result.IsEmpty())
        {
            return;
        }
        LookupUsings(block, name, result, context);
        if (!result.IsEmpty())
        {
            return;
        }
        for (SemaSymbol *parent = ns->Parent; parent != nullptr && parent != stop; parent = parent->Parent)
        {
            LookupIn(parent, name, result, context);
            if (!result.IsEmpty())
            {
                return;
            }
        }
        block = outer;
    }
    LookupIn(_global, name, result, context);
    if (!result.IsEmpty())
    {
        return;
    }
    LookupUsings(context.File, name, result, context);
    if (!result.IsEmpty())
    {
        return;
    }
    LookupIn(_marlin, name, result, context);
}

SemaSymbol *Sema::GetMagicSymbol(uint32 id) const
{
    SemaSymbol *const *found = _magicSymbols.Find(id);
    return found != nullptr ? *found : nullptr;
}

bool Sema::IsAccessible(const SemaContext &context, const SemaSymbol *symbol) const
{
    switch (symbol->Visibility)
    {
    case SemaVisibility::Public:
        return true;
    case SemaVisibility::Internal:
    {
        if (symbol->File == nullptr || context.File == nullptr)
        {
            return true;
        }
        const SemaSymbol *contextSymbol = nullptr;
        for (const AstDecl *decl = context.Decl; decl != nullptr && contextSymbol == nullptr; decl = decl->Parent)
        {
            contextSymbol = GetSymbolOf(decl);
        }
        return contextSymbol == nullptr || contextSymbol->Package == 0 || symbol->Package == contextSymbol->Package ||
               symbol->Package == 0;
    }
    case SemaVisibility::Private:
        for (const SemaSymbol *type = context.TypeSymbol; type != nullptr; type = type->Parent)
        {
            if (type == symbol->Parent)
            {
                return true;
            }
        }
        return false;
    case SemaVisibility::Protected:
        for (const SemaSymbol *type = context.TypeSymbol; type != nullptr; type = type->Parent)
        {
            if (type == symbol->Parent)
            {
                return true;
            }
        }
        for (const SemaStructInstance *base = context.Owner; base != nullptr; base = base->Base)
        {
            if (base->Symbol == symbol->Parent)
            {
                return true;
            }
        }
        return false;
    }
    return false;
}

void Sema::UseFile(const AstFile *file)
{
    if (file != nullptr)
    {
        _usedFiles.Add(file);
    }
}

void Sema::EnsureBodyParsed(const AstFile *file, AstBody &body)
{
    ParseBody(*file, body, Memory, Diagnostics);
}

SemaContext Sema::ContextOf(const SemaSymbol *symbol, Span<const SemaGenericBinding> generics,
                            SemaStructInstance *owner)
{
    SemaContext context;
    context.File = symbol->File;
    context.Decl = symbol->Decl;
    context.Generics = generics;
    context.Owner = owner;
    context.Dialect = symbol->File != nullptr ? symbol->File->File->GetDialect() : LanguageDialect::Shader;
    const bool isScope = symbol->IsAggregate() || symbol->Kind == SemaSymbolKind::Extension;
    for (SemaSymbol *parent = isScope ? const_cast<SemaSymbol *>(symbol) : symbol->Parent; parent != nullptr;
         parent = parent->Parent)
    {
        if (parent->IsTypeScope())
        {
            context.TypeSymbol = parent;
            break;
        }
        if (parent->Kind == SemaSymbolKind::Namespace)
        {
            break;
        }
    }
    return context;
}

StringView Sema::TextOf(SourceRange range) const
{
    const SourceFile *file = Diagnostics.GetFile(range.File);
    return file != nullptr ? file->GetText().Substring(range.Begin, range.End - range.Begin) : StringView();
}

void Sema::GetDependencies(List<const SourceFile *> &files) const
{
    for (const AstFile *tree : _trees)
    {
        if (_usedFiles.Contains(tree))
        {
            files.Add(tree->File);
        }
    }
}

void Sema::GetNameDependencies(LanguageNameDependencies &names) const
{
    for (const auto &[name, foundNamespace] : _queriedNames)
    {
        (foundNamespace ? names.NamespaceNames : names.Names).Add(name);
    }
    names.Names.Sort();
    names.NamespaceNames.Sort();
    names.Extensions = _queriedExtensions;
}

void Sema::GetSourceFiles(List<const SourceFile *> &files) const
{
    for (const AstFile *tree : _trees)
    {
        files.Add(tree->File);
    }
}
} // namespace Marlin
