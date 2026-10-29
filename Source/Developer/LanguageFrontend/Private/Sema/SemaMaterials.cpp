#include "Sema/Lowering.h"
#include "Sema/SemaBuiltins.h"
#include "Sema/SemaHelpers.h"

#include "Math/Float16.h"

namespace Marlin
{
namespace
{
/// The declaration kind *target* names, for diagnostics: "a struct", "a field", ….
StringView TargetName(SemaAttributeTarget target)
{
    switch (target)
    {
    case SemaAttributeTarget::Struct:
        return "a struct";
    case SemaAttributeTarget::Field:
        return "a field";
    case SemaAttributeTarget::Function:
        return "a function";
    case SemaAttributeTarget::Parameter:
        return "a parameter";
    case SemaAttributeTarget::GenericParameter:
        return "a generic parameter";
    case SemaAttributeTarget::Enum:
        return "an enum";
    case SemaAttributeTarget::Interface:
        return "an interface";
    }
    return "a declaration";
}

/// Why a field of *type* cannot be a material parameter, or empty when it can (§9.4).
StringView WhyNotMaterialField(const SemaType *type)
{
    switch (type->GetKind())
    {
    case SemaTypeKind::Bool:
    case SemaTypeKind::Int:
    case SemaTypeKind::Float:
    case SemaTypeKind::Vector:
    case SemaTypeKind::Matrix:
    case SemaTypeKind::Enum:
    case SemaTypeKind::Sampler:
    case SemaTypeKind::AccelerationStructure:
        return {};
    case SemaTypeKind::Texture:
    case SemaTypeKind::Buffer:
        return type->IsWritable() ? StringView("material code cannot write to resources") : StringView();
    case SemaTypeKind::Array:
        return WhyNotMaterialField(type->GetElement());
    case SemaTypeKind::Struct:
        for (const SemaField &field : type->GetStruct()->Fields)
        {
            // A texture parameter's streaming feedback pointer is the engine's, which the sample writes.
            if (type->GetStruct()->Symbol->Name == "MaterialTexture2D" && field.Name == "Feedback")
            {
                continue;
            }
            const StringView reason = WhyNotMaterialField(field.Type);
            if (!reason.IsEmpty())
            {
                return reason;
            }
        }
        return {};
    default:
        return "a material parameter is a number, vector, matrix, enum, read-only resource or a struct of them";
    }
}
} // namespace

SemaSymbol *Sema::FindDeclaredAttribute(const AstAttribute &attribute, const SemaContext &context)
{
    // RECOVERY-DECISION lang-sema-D3: an attribute names a struct marked `[Attribute]`, by `Name` or `NameAttribute`;
    // names that find no such struct are the compiler's own attributes and are left to their checks.
    auto find = [&](StringView name) -> SemaSymbol * {
        SemaLookup lookup;
        if (name.Contains('.'))
        {
            if (SemaSymbol *symbol = FindQualified(name))
            {
                lookup.Symbols.Add(symbol);
            }
        }
        else
        {
            Lookup(context, name, lookup);
        }
        for (SemaSymbol *symbol : lookup.Symbols)
        {
            if (symbol->Kind == SemaSymbolKind::Struct && FindAttribute(symbol->Decl->Attributes, "Attribute") != nullptr)
            {
                return symbol;
            }
        }
        return nullptr;
    };
    if (SemaSymbol *symbol = find(attribute.Name))
    {
        return symbol;
    }
    return find(String(attribute.Name).Append("Attribute").AsView());
}

bool Sema::CheckDeclaredAttributes(const AstDecl *decl, SemaAttributeTarget target, const SemaContext &context)
{
    if (const bool *checked = _checkedAttributes.Find(decl))
    {
        return *checked;
    }
    _checkedAttributes.Add(decl, true);
    bool ok = true;
    for (const AstAttribute &attribute : decl->Attributes)
    {
        SemaSymbol *declaration = FindDeclaredAttribute(attribute, context);
        if (declaration == nullptr)
        {
            continue;
        }
        UseFile(declaration->File);
        const AstAttribute *usage = FindAttribute(declaration->Decl->Attributes, "Attribute");
        uint32 targets = ~0u;
        if (usage->Arguments.Count() > 0)
        {
            SemaConstant *value = EvaluateAttributeArgument(*usage, 0, {}, ContextOf(declaration));
            targets = value != nullptr ? static_cast<uint32>(value->Bits) : ~0u;
        }
        if ((targets & static_cast<uint32>(target)) == 0)
        {
            Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute.Range, "`{0}` cannot be put on {1}",
                              declaration->Name, TargetName(target))
                .WithSecondaryLabel(usage->Range, "its targets are declared here");
            ok = false;
            continue;
        }
        SemaAttributeValue value;
        ok = EvaluateDeclaredAttribute(attribute, declaration, context, value) && ok;
    }
    _checkedAttributes.Set(decl, ok);
    return ok;
}

bool Sema::EvaluateDeclaredAttribute(const AstAttribute &attribute, SemaSymbol *declaration,
                                     const SemaContext &context, SemaAttributeValue &value)
{
    value.Declaration = declaration;
    if (!GenericParametersOf(declaration->Decl).IsEmpty())
    {
        Diagnostics.Error(DiagnosticCode::InvalidAttribute, declaration->Decl->NameRange,
                          "an attribute cannot be generic");
        return false;
    }
    SemaStructInstance *instance = InstantiateStruct(declaration, {});
    if (!ResolveFields(instance))
    {
        return false;
    }
    List<bool> given;
    for (usize i = 0; i < instance->Fields.Count(); ++i)
    {
        value.Fields.Add({instance->Fields[i].Name, nullptr});
        given.Add(false);
    }
    bool ok = true;
    usize positional = 0;
    for (const AstAttributeArgument &argument : attribute.Arguments)
    {
        usize index = ~usize{0};
        if (argument.Name.IsEmpty())
        {
            index = positional++;
            if (index >= instance->Fields.Count())
            {
                Diagnostics
                    .Error(DiagnosticCode::WrongArgumentCount, argument.Value->Range,
                           "`{0}` takes {1} positional argument(s)", declaration->Name, instance->Fields.Count())
                    .WithNote("positional arguments set the fields in the order they are declared (§3.7)");
                ok = false;
                continue;
            }
        }
        else
        {
            for (usize i = 0; i < instance->Fields.Count(); ++i)
            {
                if (instance->Fields[i].Name == argument.Name)
                {
                    index = i;
                }
            }
            if (index == ~usize{0})
            {
                Diagnostics.Error(DiagnosticCode::UndefinedName, argument.Value->Range, "`{0}` is not a field or property",
                                  argument.Name)
                    .WithLabel("unknown field");
                ok = false;
                continue;
            }
        }
        if (given[index])
        {
            Diagnostics.Error(DiagnosticCode::InvalidAttribute, argument.Value->Range, "`{0}` is given twice",
                              instance->Fields[index].Name);
            ok = false;
            continue;
        }
        given[index] = true;
        value.Fields[index].Value = EvaluateConstantExpression(argument.Value, context, instance->Fields[index].Type);
        ok = ok && value.Fields[index].Value != nullptr;
    }
    // Fields the application leaves out take their initial value, or zero.
    for (usize i = 0; i < instance->Fields.Count(); ++i)
    {
        if (given[i])
        {
            continue;
        }
        const SemaField &field = instance->Fields[i];
        const AstNode *initializer =
            field.Symbol->Kind == SemaSymbolKind::Field ? field.Symbol->Decl->As<AstFieldDecl>()->Initializer : nullptr;
        value.Fields[i].Value =
            initializer != nullptr
                ? EvaluateConstantExpression(const_cast<AstNode *>(initializer), ContextOf(field.Symbol), field.Type)
                : NewZero(field.Type);
        ok = ok && value.Fields[i].Value != nullptr;
    }
    return ok;
}

bool Sema::CheckMaterial(SemaSymbol *symbol)
{
    if (const bool *checked = _checkedMaterials.Find(symbol))
    {
        return *checked;
    }
    _checkedMaterials.Add(symbol, false);
    UseFile(symbol->File);
    const AstAttribute *marker = FindAttribute(symbol->Decl->Attributes, "Material");
    const SourceRange range = marker != nullptr ? marker->Range : symbol->Decl->NameRange;
    if (symbol->Kind != SemaSymbolKind::Struct)
    {
        Diagnostics.Error(DiagnosticCode::InvalidMaterial, range, "`[Material]` goes on a struct")
            .WithNote("a material is a struct whose fields are its parameters (§9.4)");
        return false;
    }
    if (symbol->File->File->GetDialect() != LanguageDialect::Shader)
    {
        Diagnostics.Error(DiagnosticCode::NotInDialect, symbol->Decl->NameRange,
                          "materials are declared in .mshader files");
        return false;
    }
    bool ok = true;
    const SemaContext context = ContextOf(symbol);

    // The constant generic parameters are the variant switches: each has a default and a known set of values.
    uint64 variants = 1;
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(symbol->Decl);
    List<SemaGenericArgument> defaults;
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        const AstGenericParameterDecl *parameter = parameters[i];
        if (parameter->ValueType == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, parameter->NameRange,
                              "a material's generic parameters are constants, not types")
                .WithNote("they are the material's variant switches (§9.4)");
            ok = false;
            continue;
        }
        const SemaType *type = ResolveType(parameter->ValueType, context);
        if (type->IsError())
        {
            ok = false;
            continue;
        }
        if (!type->Is(SemaTypeKind::Bool) && !type->Is(SemaTypeKind::Enum))
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, parameter->ValueType->Range,
                              "a variant switch is a `bool` or an enum, not `{0}`", TypeNameOf(type).AsView())
                .WithNote("the compiler counts each material's variants, so every switch has a known set of values");
            ok = false;
            continue;
        }
        if (parameter->Default == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, parameter->NameRange,
                              "the variant switch `{0}` needs a default value", parameter->Name)
                .WithHelp(Format("write it in the declaration, such as `bool {0} = false`", parameter->Name).AsView());
            ok = false;
            continue;
        }
        SemaConstant *value = GetGenericDefault(symbol, i);
        if (value == nullptr)
        {
            ok = false;
            continue;
        }
        defaults.Add({nullptr, value});
        const usize values = type->Is(SemaTypeKind::Bool) ? 2 : type->GetSymbol()->Ordered.Count();
        variants *= values > 0 ? values : 1;
    }
    if (ok && variants > Options->VariantBudget)
    {
        Diagnostics.Error(DiagnosticCode::VariantBudgetExceeded, symbol->Decl->NameRange,
                          "`{0}` has more than {1} variants", symbol->Name, Options->VariantBudget)
            .WithNote("the budget is LanguageCompileOptions::VariantBudget; use fewer switches, or a specialization "
                      "constant for those that only pick branches (§9.4)");
        ok = false;
    }

    // The fields are the material's parameters, which the passes read and never write.
    if (ok)
    {
        SemaStructInstance *instance = InstantiateStruct(symbol, defaults.AsSpan());
        if (ResolveFields(instance))
        {
            for (const SemaField &field : instance->Fields)
            {
                const StringView reason = WhyNotMaterialField(field.Type);
                if (!reason.IsEmpty())
                {
                    Diagnostics.Error(DiagnosticCode::InvalidMaterial, field.Symbol->Decl->NameRange,
                                      "the parameter `{0}` cannot be a `{1}`", field.Name, TypeNameOf(field.Type).AsView())
                        .WithNote(reason);
                    ok = false;
                }
            }
        }
        else
        {
            ok = false;
        }
    }
    if (symbol->Decl->As<AstStructDecl>()->Interfaces.IsEmpty())
    {
        Diagnostics.Error(DiagnosticCode::InvalidMaterial, symbol->Decl->NameRange,
                          "the material `{0}` implements no interface", symbol->Name)
            .WithHelp("implement a material interface, such as `ISurfaceMaterial`, so passes can use it (§9.4)");
        ok = false;
    }
    _checkedMaterials.Set(symbol, ok);
    return ok;
}

const SemaType *Sema::GetMaterialType(SourceRange range)
{
    const StringView name = Options->Material.AsView();
    SemaSymbol *symbol = FindQualified(name);
    if (symbol == nullptr || symbol->Decl == nullptr || symbol->Kind != SemaSymbolKind::Struct)
    {
        Diagnostics.Error(DiagnosticCode::InvalidMaterial, range, "cannot find the material `{0}`", name)
            .WithNote("a material is a `[Material]` struct named by its fully qualified name, such as "
                      "`Project.Materials.Rock`");
        return nullptr;
    }
    // A pass generic over another kind of struct, such as a custom BSDF's lighting (07 §4.3), takes it as it is.
    if (FindAttribute(symbol->Decl->Attributes, "Material") == nullptr)
    {
        if (!GenericParametersOf(symbol->Decl).IsEmpty() || !Options->Variant.IsEmpty())
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, range,
                              "`{0}` is generic or given a variant, but only `[Material]` structs have variants", name);
            return nullptr;
        }
        return GetTypeOfSymbol(symbol, {}, ContextOf(symbol), range);
    }
    if (!CheckMaterial(symbol))
    {
        return nullptr;
    }
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(symbol->Decl);
    for (const LanguageVariantValue &variant : Options->Variant)
    {
        bool known = false;
        for (const AstGenericParameterDecl *parameter : parameters)
        {
            known = known || parameter->Name == variant.Name.AsView();
        }
        if (!known)
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, range, "the material `{0}` has no variant switch `{1}`",
                              name, variant.Name.AsView())
                .WithSecondaryLabel(symbol->Decl->NameRange, "material declared here");
            return nullptr;
        }
    }
    // Each switch takes the variant's value, or its default.
    List<SemaGenericArgument> arguments;
    const SemaContext context = ContextOf(symbol);
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        const LanguageVariantValue *chosen = nullptr;
        for (const LanguageVariantValue &variant : Options->Variant)
        {
            if (variant.Name.AsView() == parameters[i]->Name)
            {
                chosen = &variant;
            }
        }
        if (chosen == nullptr)
        {
            arguments.Add({nullptr, GetGenericDefault(symbol, i)});
            continue;
        }
        const SemaType *type = ResolveType(parameters[i]->ValueType, context);
        SemaConstant *value = type->Is(SemaTypeKind::Bool)
                                  ? (chosen->Value == 0 || chosen->Value == 1 ? NewBool(chosen->Value != 0) : nullptr)
                                  : ConvertConstant(NewScalar(Types.GetInt(64, true), static_cast<uint64>(chosen->Value)),
                                                    type);
        if (value == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, range, "the variant value {0} does not fit `{1}`",
                              chosen->Value, parameters[i]->Name)
                .WithSecondaryLabel(parameters[i]->NameRange, "declared here");
            return nullptr;
        }
        arguments.Add({nullptr, value});
    }
    return GetTypeOfSymbol(symbol, arguments.AsSpan(), context, range);
}

namespace
{
/// Writes *value*, of the MIR type *type*, into *out* as GPU memory holds it; handles are left zero.
void WriteConstant(const SemaConstant *value, const MirType *type, uint8 *out)
{
    if (value == nullptr)
    {
        return;
    }
    switch (type->GetKind())
    {
    case MirTypeKind::Float:
        if (type->GetWidth() == 16)
        {
            const uint16 bits = Float16(static_cast<float32>(value->Float)).Bits;
            __builtin_memcpy(out, &bits, sizeof(bits));
        }
        else if (type->GetWidth() == 32)
        {
            const auto single = static_cast<float32>(value->Float);
            __builtin_memcpy(out, &single, sizeof(single));
        }
        else
        {
            __builtin_memcpy(out, &value->Float, sizeof(value->Float));
        }
        return;
    case MirTypeKind::Int:
    case MirTypeKind::Bool:
        __builtin_memcpy(out, &value->Bits, Min<usize>(type->GetSize(), sizeof(value->Bits)));
        return;
    case MirTypeKind::Vector:
    case MirTypeKind::Matrix:
    case MirTypeKind::Array:
    {
        const MirType *element = type->GetElement();
        const uint32 stride = type->GetCount() != 0 ? type->GetSize() / type->GetCount() : 0;
        for (usize i = 0; i < value->Elements.Count() && i < type->GetCount(); ++i)
        {
            WriteConstant(value->Elements[i], element, out + i * stride);
        }
        return;
    }
    case MirTypeKind::Struct:
        for (usize i = 0; i < value->Elements.Count() && i < type->GetFields().Count(); ++i)
        {
            WriteConstant(value->Elements[i], type->GetFields()[i].Type, out + type->GetFields()[i].Offset);
        }
        return;
    default:
        return;
    }
}
} // namespace

void Sema::GetMaterials(StringView folder, List<String> &names) const
{
    auto visit = [&](auto &self, Span<AstNode *> members) -> void {
        for (AstNode *member : members)
        {
            if (const auto *ns = member->TryAs<AstNamespaceDecl>())
            {
                self(self, ns->Members);
                continue;
            }
            const auto *decl = member->TryAs<AstStructDecl>();
            if (decl != nullptr && FindAttribute(decl->Attributes, "Material") != nullptr)
            {
                if (const SemaSymbol *symbol = GetSymbolOf(decl))
                {
                    names.Add(GetQualifiedName(symbol));
                }
            }
        }
    };
    for (const AstFile *tree : _trees)
    {
        if (tree->File->GetPath().IndexOf(folder) != StringView::NotFound &&
            tree->File->GetDialect() == LanguageDialect::Shader)
        {
            visit(visit, tree->Members);
        }
    }
}

bool Sema::ReflectMaterial(LanguageMaterialReflection &reflection)
{
    const SemaType *type = GetMaterialType({});
    if (type == nullptr || !type->Is(SemaTypeKind::Struct))
    {
        return false;
    }
    SemaStructInstance *instance = type->GetStruct();
    SemaSymbol *symbol = instance->Symbol;
    if (!ResolveFields(instance))
    {
        return false;
    }
    reflection.Name = GetQualifiedName(symbol);
    const SemaContext context = ContextOf(symbol);
    if (const AstAttribute *id = FindAttribute(symbol->Decl->Attributes, "Id"))
    {
        if (const SemaConstant *text = EvaluateAttributeArgument(*id, 0, {}, context))
        {
            reflection.Id = String(text->Text);
        }
    }
    // The variant switches, with their defaults and how many values each takes.
    const Span<AstGenericParameterDecl *> switches = GenericParametersOf(symbol->Decl);
    for (usize i = 0; i < switches.Count(); ++i)
    {
        LanguageMaterialSwitch &entry = reflection.Switches.AddDefaulted();
        entry.Name = String(switches[i]->Name);
        if (const SemaConstant *value = GetGenericDefault(symbol, i))
        {
            entry.Default = static_cast<int64>(value->Bits);
        }
        const SemaType *switchType = ResolveType(switches[i]->ValueType, context);
        entry.ValueCount = switchType->Is(SemaTypeKind::Bool)
                               ? 2
                               : static_cast<uint32>(switchType->GetSymbol()->Ordered.Count());
    }
    // The layout the material table holds, from the lowered struct.
    MirModule module;
    Lowering lowering(*this, module);
    const MirType *lowered = lowering.LowerType(type);
    if (lowered == nullptr || !lowered->Is(MirTypeKind::Struct) ||
        lowered->GetFields().Count() != instance->Fields.Count())
    {
        return false;
    }
    reflection.Size = lowered->GetSize();
    reflection.Defaults.Resize(reflection.Size, 0);
    List<SemaGenericBinding> bindings;
    GetBindings(instance, bindings);
    bool ok = true;
    for (usize i = 0; i < instance->Fields.Count(); ++i)
    {
        const SemaField &field = instance->Fields[i];
        const MirStructField &place = lowered->GetFields()[i];
        LanguageMaterialParameter &parameter = reflection.Parameters.AddDefaulted();
        parameter.Name = String(field.Name);
        parameter.Type = TypeNameOf(field.Type);
        parameter.Offset = place.Offset;
        parameter.Size = place.Type->GetSize();
        const SemaContext fieldContext = ContextOf(field.Symbol, Freeze(bindings), instance);
        const AstFieldDecl *decl = field.Symbol->Decl->TryAs<AstFieldDecl>();
        const SemaConstant *initial = nullptr;
        if (decl != nullptr && decl->Initializer != nullptr)
        {
            initial = EvaluateConstantExpression(decl->Initializer, fieldContext, field.Type);
            ok = ok && initial != nullptr;
            WriteConstant(initial, place.Type, reflection.Defaults.Data() + place.Offset);
        }
        // A texture parameter is a Marlin.Shading.MaterialTexture2D: texture, sampler, then its default texture.
        if (field.Type->Is(SemaTypeKind::Struct) && field.Type->GetStruct()->Symbol->Name == "MaterialTexture2D")
        {
            parameter.IsTexture = true;
            if (initial != nullptr && initial->Elements.Count() >= 3 && initial->Elements[2] != nullptr)
            {
                parameter.DefaultTexture = static_cast<uint32>(initial->Elements[2]->Bits);
            }
        }
        const Span<AstAttribute> attributes = field.Symbol->Decl->Attributes;
        if (const AstAttribute *range = FindAttribute(attributes, "Range"))
        {
            const SemaConstant *low = EvaluateAttributeArgument(*range, 0, {}, fieldContext);
            const SemaConstant *high = EvaluateAttributeArgument(*range, 1, {}, fieldContext);
            if (low != nullptr && high != nullptr)
            {
                auto number = [](const SemaConstant *c) {
                    return c->Type->Is(SemaTypeKind::Float) ? c->Float : static_cast<double>(static_cast<int64>(c->Bits));
                };
                parameter.HasRange = true;
                parameter.RangeMin = number(low);
                parameter.RangeMax = number(high);
            }
        }
        parameter.IsColor = FindAttribute(attributes, "Color") != nullptr;
    }
    // The custom BSDFs: those whose closure type its material pass asks for, binding the material's code.
    if (ok && !Diagnostics.HasErrors() && FindQualified("Marlin.Passes.Material.Resolve") != nullptr)
    {
        MirModule pass;
        QueriedTypeIds.Clear();
        ok = CompileEntryPoint("Marlin.Passes.Material.Resolve", pass);
        for (const String &bsdf : QueriedTypeIds)
        {
            reflection.CustomBsdfs.Add(bsdf);
        }
    }
    return ok && !Diagnostics.HasErrors();
}
} // namespace Marlin
