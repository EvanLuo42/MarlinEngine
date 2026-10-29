#include "Sema/Binder.h"
#include "Sema/Lowering.h"
#include "Sema/SemaHelpers.h"

#include "LanguageIR/MirVerifier.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
/// Collects the functions of a script package to check, and checks its types, delegates and static fields.
class PackageChecker
{
public:
    PackageChecker(Sema &sema, uint32 package) : _sema(&sema), _package(package)
    {
    }

    void Container(SemaSymbol *container, SemaStructInstance *owner)
    {
        for (SemaSymbol *member : container->Ordered)
        {
            if (member->Kind != SemaSymbolKind::Namespace && member->Package != _package)
            {
                continue;
            }
            Member(member, owner);
        }
    }

    /// The members of the package's non-generic extensions (§3.5), with the conformances they declare.
    void Extensions(Span<SemaSymbol *const> extensions)
    {
        for (SemaSymbol *extension : extensions)
        {
            if (extension->Package != _package || !GenericParametersOf(extension->Decl).IsEmpty())
            {
                continue;
            }
            SemaStructInstance *instance = _sema->InstantiateStruct(extension, {});
            const SemaType *extended = _sema->GetOwnerType(instance);
            if (extended->IsError())
            {
                continue;
            }
            (void)_sema->GetExtensions(extended);
            Container(extension, instance);
        }
    }

    void BindAll()
    {
        HashSet<SemaFunction *> seen;
        for (usize i = 0; i < _pending.Count(); ++i)
        {
            SemaFunction *function = _pending[i];
            if (!seen.Add(function))
            {
                continue;
            }
            if (!_sema->BindBody(function))
            {
                continue;
            }
            Bound.Add(function);
            for (const SemaCallSite &site : function->Callees)
            {
                _pending.Add(site.Callee);
            }
        }
    }

    /// The functions that were bound, the package's first, then the ones they call in other packages.
    List<SemaFunction *> Bound;
    /// The non-generic structs, classes and interfaces of the package.
    List<SemaStructInstance *> Types;
    /// The static fields with initial values, in declaration order.
    List<SemaGlobal *> Statics;

private:
    void Member(SemaSymbol *member, SemaStructInstance *owner)
    {
        const bool generic = member->Decl != nullptr && !GenericParametersOf(member->Decl).IsEmpty();
        switch (member->Kind)
        {
        case SemaSymbolKind::Namespace:
            Container(member, nullptr);
            break;
        case SemaSymbolKind::Struct:
        case SemaSymbolKind::Class:
        case SemaSymbolKind::Interface:
        {
            if (generic)
            {
                break;
            }
            SemaStructInstance *instance = _sema->InstantiateStruct(member, {});
            Types.Add(instance);
            _sema->ResolveHierarchy(instance);
            if (member->Kind != SemaSymbolKind::Interface)
            {
                _sema->ResolveFields(instance);
            }
            Container(member, instance);
            break;
        }
        case SemaSymbolKind::Delegate:
            if (!generic)
            {
                SemaSignature signature;
                _sema->GetDelegateSignature(_sema->InstantiateStruct(member, {}), signature);
            }
            break;
        case SemaSymbolKind::Enum:
            for (SemaSymbol *value : member->Ordered)
            {
                _sema->GetEnumMemberValue(value);
            }
            break;
        case SemaSymbolKind::Function:
        case SemaSymbolKind::Constructor:
        case SemaSymbolKind::Operator:
            if (!generic && !(owner != nullptr && owner->Symbol->Kind == SemaSymbolKind::Interface))
            {
                _pending.Add(_sema->InstantiateFunction(member, owner, {}));
            }
            break;
        case SemaSymbolKind::Property:
        {
            if (owner != nullptr && owner->Symbol->Kind == SemaSymbolKind::Interface)
            {
                break;
            }
            const auto *property = member->Decl->As<AstPropertyDecl>();
            if (property->HasGetter)
            {
                _pending.Add(_sema->InstantiateFunction(member, owner, {}, SemaAccessor::Getter));
            }
            if (property->Setter.Present)
            {
                _pending.Add(_sema->InstantiateFunction(member, owner, {}, SemaAccessor::Setter));
            }
            if (property->Initializer != nullptr && !_sema->IsAutoProperty(member))
            {
                _sema->Diagnostics.Error(DiagnosticCode::InvalidStatement, property->Initializer->Range,
                                         "only an auto-property can have an initial value");
            }
            if (property->Initializer != nullptr && member->IsStatic())
            {
                StaticInitializer(member, owner, property->Initializer);
            }
            break;
        }
        case SemaSymbolKind::Field:
        {
            const auto *field = member->Decl->As<AstFieldDecl>();
            if (HasAnyFlags(field->Modifiers, AstModifiers::Const))
            {
                _sema->GetStaticValue(member, owner);
            }
            else if (HasAnyFlags(field->Modifiers, AstModifiers::Static) && field->Initializer != nullptr)
            {
                StaticInitializer(member, owner, field->Initializer);
            }
            break;
        }
        default:
            break;
        }
    }

    /// Checks the initial value of a static field, run when the module loads (MarlinScript §6.3).
    void StaticInitializer(SemaSymbol *member, SemaStructInstance *owner, AstNode *initializer)
    {
        SemaGlobal *global = _sema->GetStaticField(member, owner);
        if (global->Type->IsError())
        {
            return;
        }
        Binder binder(*_sema, nullptr, _sema->ContextOf(member, {}, owner));
        global->Initializer = binder.ConvertImplicit(binder.BindExpression(initializer, global->Type), global->Type);
        Statics.Add(global);
    }

    Sema *_sema;
    uint32 _package;
    List<SemaFunction *> _pending;
};

/// The text a MirAttributeArgument holds for *value*; false for a value that has none, such as a composite.
bool AttributeText(const SemaConstant *value, String &text)
{
    if (value == nullptr || value->Type == nullptr)
    {
        return false;
    }
    const SemaType *type = value->Type;
    if (!value->Text.IsEmpty() || type->IsReference())
    {
        text = String(value->Text);
        return true;
    }
    if (type->Is(SemaTypeKind::Bool))
    {
        text = String(value->Bits != 0 ? "true" : "false");
        return true;
    }
    if (type->Is(SemaTypeKind::Float))
    {
        text = Format("{0}", value->Float);
        return true;
    }
    if (type->Is(SemaTypeKind::Int) || type->Is(SemaTypeKind::Enum))
    {
        text = type->IsSigned() ? Format("{0}", value->GetInt()) : Format("{0}", value->Bits);
        return true;
    }
    return false;
}
} // namespace

void Sema::RecordScriptAttributes(const AstDecl *decl, const SemaContext &context, MirModule &module,
                                  const MirType *type, MirFunction *function)
{
    for (const AstAttribute &attribute : decl->Attributes)
    {
        SemaSymbol *declaration = FindDeclaredAttribute(attribute, context);
        SemaAttributeValue value;
        if (declaration == nullptr || !EvaluateDeclaredAttribute(attribute, declaration, context, value))
        {
            continue;
        }
        MirAttribute &record = module.AddAttribute(GetQualifiedName(declaration).AsView());
        record.Type = type;
        record.Function = function;
        for (const SemaAttributeField &field : value.Fields)
        {
            String text;
            if (AttributeText(field.Value, text))
            {
                record.Arguments.Add({module.Intern(field.Name), module.Intern(text.AsView())});
            }
        }
    }
}

bool Sema::CheckScriptPackage(StringView package)
{
    const uint32 errors = Diagnostics.GetErrorCount();
    if (Options->Language != LanguageDialect::Script)
    {
        Diagnostics.Error(DiagnosticCode::NotInDialect, {}, "script packages are checked by a Script compilation")
            .WithHelp("set LanguageCompileOptions.Language to Script");
        return false;
    }
    const uint32 *id = _packages.Find(package);
    if (id == nullptr)
    {
        Diagnostics.Error(DiagnosticCode::UndefinedName, {}, "the source set has no package `{0}`", package)
            .WithNote("a package is a mount root such as `Engine` or `Project` (MarlinScript §1.2)");
        return false;
    }
    PackageChecker checker(*this, *id);
    checker.Container(_global, nullptr);
    checker.Extensions(GetExtensionDeclarations());
    checker.BindAll();
    return Diagnostics.GetErrorCount() == errors;
}

bool Sema::CompileScriptPackage(StringView package, MirModule &module)
{
    const uint32 errors = Diagnostics.GetErrorCount();
    if (Options->Language != LanguageDialect::Script)
    {
        Diagnostics.Error(DiagnosticCode::NotInDialect, {}, "script packages are compiled by a Script compilation")
            .WithHelp("set LanguageCompileOptions.Language to Script");
        return false;
    }
    const uint32 *id = _packages.Find(package);
    if (id == nullptr)
    {
        Diagnostics.Error(DiagnosticCode::UndefinedName, {}, "the source set has no package `{0}`", package)
            .WithNote("a package is a mount root such as `Engine` or `Project` (MarlinScript §1.2)");
        return false;
    }
    PackageChecker checker(*this, *id);
    checker.Container(_global, nullptr);
    checker.Extensions(GetExtensionDeclarations());
    checker.BindAll();
    if (Diagnostics.GetErrorCount() != errors)
    {
        return false;
    }

    // One module per package (MarlinScript §1.2): its functions are defined, those of other packages it calls are
    // imported, and its static fields get their initial values in one static initializer (§6.3).
    module.SetName(package);
    Lowering lowering(*this, module);
    lowering.SetScriptPackage(*id);
    for (SemaStructInstance *type : checker.Types)
    {
        lowering.ScriptTypeOf(GetInstanceType(type));
    }
    for (SemaFunction *function : checker.Bound)
    {
        lowering.LowerFunction(function);
    }
    // The engine reads user-declared attributes of the package's structs and functions from the module, such as the
    // ECS's `[Component]` and `[System]` (MarlinScript §11.2).
    for (SemaStructInstance *type : checker.Types)
    {
        if (type->Symbol->Kind == SemaSymbolKind::Struct && !type->Symbol->Decl->Attributes.IsEmpty())
        {
            RecordScriptAttributes(type->Symbol->Decl, ContextOf(type->Symbol), module,
                                   lowering.LowerType(GetInstanceType(type)), nullptr);
        }
    }
    for (SemaFunction *function : checker.Bound)
    {
        const SemaSymbol *symbol = function->Symbol;
        if (symbol->Package == *id && symbol->Decl != nullptr && !symbol->Decl->Attributes.IsEmpty() &&
            function->Accessor == SemaAccessor::None)
        {
            RecordScriptAttributes(symbol->Decl, ContextOf(symbol, {}, function->Owner), module, nullptr,
                                   lowering.LowerFunction(function));
        }
    }
    // RECOVERY-DECISION lang-lower-D9: one static initializer per source file, `<path>$static`, in file order, each
    // running its file's initializers in declaration order (the recovered test looks for `Test.mscript$static`).
    List<const AstFile *> files;
    for (const SemaGlobal *global : checker.Statics)
    {
        if (!files.Contains(global->Symbol->File))
        {
            files.Add(global->Symbol->File);
        }
    }
    for (const AstFile *file : files)
    {
        List<SemaGlobal *> statics;
        for (SemaGlobal *global : checker.Statics)
        {
            if (global->Symbol->File == file)
            {
                statics.Add(global);
            }
        }
        StringBuilder<256> name;
        name.Append(file != nullptr ? file->File->GetPath() : package).Append("$static");
        lowering.LowerStaticInitializer(name.AsView(), statics.AsSpan());
    }
    if (Diagnostics.GetErrorCount() != errors)
    {
        return false;
    }
    List<String> problems;
    if (!VerifyMirModule(module, problems))
    {
        LanguageDiagnostic &error =
            Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, {}, "internal compiler error: the lowered MIR is invalid");
        for (const String &problem : problems)
        {
            error.WithNote(problem.AsView());
        }
        return false;
    }
    return true;
}
} // namespace Marlin
