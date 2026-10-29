#include "Sema/Sema.h"
#include "Sema/SemaHelpers.h"
#include "Sema/SemaBuiltins.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
Span<AstConstraint> ConstraintsOf(const AstDecl *decl)
{
    if (const auto *structDecl = decl->TryAs<AstStructDecl>())
    {
        return structDecl->Constraints;
    }
    if (const auto *interfaceDecl = decl->TryAs<AstInterfaceDecl>())
    {
        return interfaceDecl->Constraints;
    }
    if (const auto *delegate = decl->TryAs<AstDelegateDecl>())
    {
        return delegate->Constraints;
    }
    if (const auto *function = decl->TryAs<AstFunctionDecl>())
    {
        return function->Constraints;
    }
    if (const auto *extension = decl->TryAs<AstExtensionDecl>())
    {
        return extension->Constraints;
    }
    return {};
}

} // namespace

const SemaType *Sema::ResolveDeclaredType(AstNode *node, Span<AstAttribute> attributes, const SemaContext &context)
{
    const SemaType *type = ResolveType(node, context);
    const AstAttribute *attribute = FindAttribute(attributes, "Format");
    if (attribute == nullptr || type->IsError())
    {
        return type;
    }
    Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute->Range, "a texture's format is its type argument")
        .WithLabel("this attribute")
        .WithHelp("write `Texture2D<float4, Access.Write, ImageFormat.Rgba16f>` (§4.4)");
    return type;
}

bool Sema::FormatHoldsTexel(MirImageFormat format, const SemaType *texel)
{
    const SemaType *scalar = texel->GetScalar();
    const bool isFloat = format <= MirImageFormat::R8Snorm;
    const bool isSigned = format >= MirImageFormat::Rgba32i && format <= MirImageFormat::R64i;
    return scalar != nullptr && format <= MirImageFormat::R64u &&
           (isFloat ? scalar->Is(SemaTypeKind::Float) : scalar->Is(SemaTypeKind::Int) && scalar->IsSigned() == isSigned);
}

MirImageFormat Sema::InferImageFormat(const SemaType *texel)
{
    const SemaType *scalar = texel->GetScalar();
    if (scalar == nullptr || texel->Is(SemaTypeKind::Matrix))
    {
        return MirImageFormat::Unknown;
    }
    const uint32 count = texel->Is(SemaTypeKind::Vector) ? texel->GetCount() : 1;
    if (count == 3)
    {
        return MirImageFormat::Unknown;
    }
    const uint32 shape = count == 1 ? 2 : count == 2 ? 1 : 0;
    auto pick = [&](MirImageFormat rgba, MirImageFormat rg, MirImageFormat r) {
        const MirImageFormat formats[] = {rgba, rg, r};
        return formats[shape];
    };
    using enum MirImageFormat;
    if (scalar->Is(SemaTypeKind::Float))
    {
        switch (scalar->GetWidth())
        {
        case 32:
            return pick(Rgba32f, Rg32f, R32f);
        case 16:
            return pick(Rgba16f, Rg16f, R16f);
        default:
            return Unknown;
        }
    }
    if (!scalar->Is(SemaTypeKind::Int))
    {
        return Unknown;
    }
    const bool isSigned = scalar->IsSigned();
    switch (scalar->GetWidth())
    {
    case 64:
        return count == 1 ? (isSigned ? R64i : R64u) : Unknown;
    case 32:
        return isSigned ? pick(Rgba32i, Rg32i, R32i) : pick(Rgba32u, Rg32u, R32u);
    case 16:
        return isSigned ? pick(Rgba16i, Rg16i, R16i) : pick(Rgba16u, Rg16u, R16u);
    case 8:
        return isSigned ? pick(Rgba8i, Rg8i, R8i) : pick(Rgba8u, Rg8u, R8u);
    default:
        return Unknown;
    }
}

const AstAttribute *Sema::FindAttribute(Span<AstAttribute> attributes, StringView name)
{
    for (const AstAttribute &attribute : attributes)
    {
        if (attribute.Name == name)
        {
            return &attribute;
        }
    }
    return nullptr;
}

SemaConstant *Sema::EvaluateAttributeArgument(const AstAttribute &attribute, usize index, StringView name,
                                              const SemaContext &context)
{
    const AstAttributeArgument *found = nullptr;
    usize positional = 0;
    for (const AstAttributeArgument &argument : attribute.Arguments)
    {
        if (!name.IsEmpty() && argument.Name == name)
        {
            found = &argument;
            break;
        }
        if (argument.Name.IsEmpty() && positional++ == index && found == nullptr)
        {
            found = &argument;
        }
    }
    return found != nullptr ? EvaluateConstantExpression(found->Value, context, nullptr) : nullptr;
}

const SemaType *Sema::ResolveType(AstNode *node, const SemaContext &context)
{
    const SemaType *type = ResolveTypeNode(node, context);
    const bool scriptOnly =
        type->IsReference() && !type->Is(SemaTypeKind::String) && !type->Is(SemaTypeKind::Interface);
    if (scriptOnly && context.Dialect != LanguageDialect::Script)
    {
        Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "`{0}` exists only in scripts",
                          TypeNameOf(type).AsView())
            .WithNote("classes, delegates, arrays of run-time length and nullable references are script types "
                      "(MarlinScript §4.4)");
        return Types.GetError();
    }
    return type;
}

const SemaType *Sema::ResolveTypeNode(AstNode *node, const SemaContext &context)
{
    if (node == nullptr)
    {
        return Types.GetError();
    }
    switch (node->Kind)
    {
    case AstKind::PredefinedType:
        switch (node->As<AstPredefinedType>()->Keyword)
        {
        case TokenKind::KeywordVoid:
            return Types.GetVoid();
        case TokenKind::KeywordBool:
            return Types.GetBool();
        case TokenKind::KeywordShort:
            return Types.GetInt(16, true);
        case TokenKind::KeywordUshort:
            return Types.GetInt(16, false);
        case TokenKind::KeywordInt:
            return Types.GetInt(32, true);
        case TokenKind::KeywordUint:
            return Types.GetInt(32, false);
        case TokenKind::KeywordLong:
            return Types.GetInt(64, true);
        case TokenKind::KeywordUlong:
            return Types.GetInt(64, false);
        case TokenKind::KeywordHalf:
            return Types.GetFloat(16);
        case TokenKind::KeywordFloat:
            return Types.GetFloat(32);
        case TokenKind::KeywordDouble:
            return Types.GetFloat(64);
        default:
            return Types.GetError();
        }
    case AstKind::ArrayType:
    {
        const auto *array = node->As<AstArrayType>();
        const SemaType *element = ResolveType(array->Element, context);
        if (element->IsError())
        {
            return element;
        }
        if (array->Size == nullptr)
        {
            if (context.Dialect == LanguageDialect::Script)
            {
                if (element->Is(SemaTypeKind::Void))
                {
                    Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Range, "`{0}` has no size in memory",
                                      TypeNameOf(element).AsView());
                    return Types.GetError();
                }
                return Types.GetGcArray(element);
            }
            Diagnostics.Error(DiagnosticCode::NotConstant, node->Range, "an array needs a constant length")
                .WithLabel("missing length")
                .WithHelp("arrays of run-time length exist only in GPU memory: use Span<T>");
            return Types.GetError();
        }
        SemaConstant *size = EvaluateConstantExpression(array->Size, context, Types.GetInt(32, false));
        if (size == nullptr)
        {
            return Types.GetError();
        }
        if (size->Bits == 0)
        {
            Diagnostics.Error(DiagnosticCode::IndexOutOfRange, array->Size->Range, "an array cannot be empty");
            return Types.GetError();
        }
        return Types.GetArray(element, static_cast<uint32>(size->Bits));
    }
    case AstKind::NullableType:
    {
        const auto *nullable = node->As<AstNullableType>();
        const SemaType *element = ResolveType(nullable->Element, context);
        if (element->IsError())
        {
            return element;
        }
        if (context.Dialect != LanguageDialect::Script)
        {
            Diagnostics.Error(DiagnosticCode::NotInDialect, node->Range, "nullable types exist only in scripts")
                .WithNote("classes, delegates, arrays of run-time length and nullable references are script types "
                          "(MarlinScript §4.4)");
            return Types.GetError();
        }
        if (element->Is(SemaTypeKind::Void))
        {
            Diagnostics.Error(DiagnosticCode::TypeMismatch, node->Range, "`void` cannot be nullable");
            return Types.GetError();
        }
        return Types.GetNullable(element);
    }
    case AstKind::SelfType:
    {
        if (context.SelfType != nullptr)
        {
            return context.SelfType;
        }
        if (context.Owner != nullptr && context.Owner->Symbol->Kind == SemaSymbolKind::Extension)
        {
            return GetOwnerType(context.Owner);
        }
        bool inInterface = false;
        for (const SemaSymbol *scope = context.TypeSymbol; scope != nullptr && scope->IsTypeScope();
             scope = scope->Parent)
        {
            inInterface = inInterface || scope->Kind == SemaSymbolKind::Interface ||
                          scope->Kind == SemaSymbolKind::Extension;
        }
        if (inInterface)
        {
            Diagnostics
                .Error(DiagnosticCode::NotAType, node->Range, "the type implementing the interface is not known here")
                .WithLabel("`this` stands for each implementing type")
                .WithHelp("use the member through a type parameter: `void F<T>(T value) where T : IFoo`");
            return Types.GetError();
        }
        Diagnostics.Error(DiagnosticCode::SelfTypeOutsideInterface, node->Range,
                          "`this` names a type only in an interface or an extension")
            .WithLabel("not in an interface or extension")
            .WithHelp("write the type's name");
        return Types.GetError();
    }
    case AstKind::NamedType:
    {
        const auto *named = node->As<AstNamedType>();
        List<SemaSymbol *> candidates;
        if (named->Qualifier != nullptr)
        {
            SemaSymbol *container = nullptr;
            // `T.Name` with a type before the dot names an associated type of that type (§3.5).
            bool qualifierIsType = named->Qualifier->Kind == AstKind::SelfType;
            if (auto *qualifier = named->Qualifier->TryAs<AstNamedType>())
            {
                SemaLookup lookup;
                if (qualifier->Qualifier == nullptr)
                {
                    Lookup(context, qualifier->Name, lookup);
                    container = lookup.Symbols.IsEmpty() ? nullptr : lookup.Symbols[0];
                    qualifierIsType = (lookup.Generic != nullptr && lookup.Generic->Type != nullptr) ||
                                      lookup.Alias != nullptr;
                }
                else
                {
                    container = FindQualified(TextOf(qualifier->Range));
                }
                if (container != nullptr && container->IsType() && container->Kind != SemaSymbolKind::Enum)
                {
                    // A type declared inside the container keeps its meaning; any other name after a type is one of
                    // its associated types, bound by the type itself or by an extension.
                    bool nested = false;
                    for (const SemaSymbol *member : GetMembers(container, named->Name))
                    {
                        nested = nested || (member->IsType() && member->Kind != SemaSymbolKind::AssociatedType);
                    }
                    qualifierIsType = qualifierIsType || !nested;
                }
            }
            if (qualifierIsType)
            {
                const SemaType *qualifierType = ResolveTypeNode(named->Qualifier, context);
                if (qualifierType->IsError())
                {
                    return qualifierType;
                }
                SemaStructInstance *owner = nullptr;
                if (SemaSymbol *binding = FindAssociatedBinding(qualifierType, named->Name, owner))
                {
                    return ResolveAssociatedType(binding, owner, qualifierType, node->Range);
                }
                Diagnostics.Error(DiagnosticCode::UndefinedName, node->Range, "`{0}` has no associated type `{1}`",
                                  TypeNameOf(qualifierType).AsView(), named->Name)
                    .WithLabel("not bound by the type or its extensions");
                return Types.GetError();
            }
            if (container == nullptr)
            {
                Diagnostics.Error(DiagnosticCode::UndefinedName, named->Qualifier->Range, "cannot find `{0}`",
                                  TextOf(named->Qualifier->Range))
                    .WithLabel("not a namespace or type");
                return Types.GetError();
            }
            for (SemaSymbol *member : GetMembers(container, named->Name))
            {
                if (IsAccessible(context, member))
                {
                    candidates.Add(member);
                }
            }
        }
        else
        {
            SemaLookup lookup;
            Lookup(context, named->Name, lookup);
            if (lookup.Generic != nullptr)
            {
                if (lookup.Generic->Type == nullptr)
                {
                    Diagnostics.Error(DiagnosticCode::NotAType, node->Range, "`{0}` is a value, not a type",
                                      named->Name)
                        .WithLabel("a constant generic parameter");
                    return Types.GetError();
                }
                return lookup.Generic->Type;
            }
            if (lookup.Alias != nullptr)
            {
                SemaContext aliasContext;
                aliasContext.File = lookup.AliasFile;
                aliasContext.Decl = lookup.Alias->Parent;
                aliasContext.Dialect = lookup.AliasFile->File->GetDialect();
                return ResolveType(lookup.Alias->Target, aliasContext);
            }
            if (lookup.Ambiguous)
            {
                Diagnostics.Error(DiagnosticCode::AmbiguousName, node->Range, "`{0}` is ambiguous", named->Name)
                    .WithLabel("found in more than one imported namespace");
                return Types.GetError();
            }
            candidates.AddRange(lookup.Symbols.AsSpan());
        }
        // Types of the same name may differ in their number of generic parameters, such as `Task` and `Task<T>`:
        // take the one the written arguments fit, counting parameters with defaults as optional.
        SemaSymbol *typeSymbol = nullptr;
        for (SemaSymbol *&candidate : candidates)
        {
            if (candidate->Kind == SemaSymbolKind::Constructor)
            {
                candidate = candidate->Parent;
            }
            if (!candidate->IsType())

            {
                continue;
            }
            const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(candidate->Decl);
            usize required = 0;
            while (required < parameters.Count() && parameters[required]->Default == nullptr)
            {
                ++required;
            }
            const usize written = named->GenericArguments.Count();
            if (written >= required && written <= parameters.Count())
            {
                typeSymbol = candidate;
                break;
            }
            if (typeSymbol == nullptr)
            {
                typeSymbol = candidate;
            }
        }
        // A name that only found the enclosing type, through its constructors, with other generic arguments names
        // a type outside it, as `Task` in `class Task<T> : Task`.
        if (typeSymbol != nullptr && named->Qualifier == nullptr && context.TypeSymbol != nullptr &&
            typeSymbol == context.TypeSymbol &&
            GenericParametersOf(typeSymbol->Decl).Count() != named->GenericArguments.Count())
        {
            SemaContext outer = context;
            outer.TypeSymbol = nullptr;
            outer.Owner = nullptr;
            SemaLookup lookup;
            Lookup(outer, named->Name, lookup);
            for (SemaSymbol *candidate : lookup.Symbols)
            {
                if (candidate->IsType() &&
                    GenericParametersOf(candidate->Decl).Count() == named->GenericArguments.Count())
                {
                    typeSymbol = candidate;
                    break;
                }
            }
        }
        // lang-lower: a member that hides a type of the same name, as `CommittedStatus CommittedStatus()` in
        // RayQuery, leaves the type to be found outside the enclosing types.
        if (typeSymbol == nullptr && !candidates.IsEmpty() && named->Qualifier == nullptr &&
            context.TypeSymbol != nullptr)
        {
            SemaContext outer = context;
            outer.TypeSymbol = nullptr;
            outer.Owner = nullptr;
            SemaLookup lookup;
            Lookup(outer, named->Name, lookup);
            for (SemaSymbol *candidate : lookup.Symbols)
            {
                if (candidate->IsType() &&
                    GenericParametersOf(candidate->Decl).Count() == named->GenericArguments.Count())
                {
                    typeSymbol = candidate;
                    break;
                }
            }
        }
        if (typeSymbol == nullptr)
        {
            if (candidates.IsEmpty())
            {
                Diagnostics
                    .Error(DiagnosticCode::UndefinedName, node->Range, "cannot find type `{0}` in this scope",
                           named->Name)
                    .WithLabel("not found in this scope");
            }
            else
            {
                Diagnostics.Error(DiagnosticCode::NotAType, node->Range, "`{0}` is not a type", named->Name)
                    .WithLabel("not a type");
            }
            return Types.GetError();
        }
        return ResolveSymbolType(typeSymbol, named->GenericArguments, context, node->Range);
    }
    default:
        Diagnostics.Error(DiagnosticCode::NotAType, node->Range, "expected a type").WithLabel("this is an expression");
        return Types.GetError();
    }
}

bool Sema::ResolveGenericArguments(SemaSymbol *symbol, Span<AstNode *> nodes, const SemaContext &context,
                                   List<SemaGenericArgument> &arguments)
{
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(symbol->Decl);
    for (usize i = 0; i < nodes.Count(); ++i)
    {
        SemaGenericArgument argument;
        const bool wantValue = i < parameters.Count() && parameters[i]->ValueType != nullptr;
        if (!ResolveGenericArgument(nodes[i], context, wantValue, argument))
        {
            return false;
        }
        if (wantValue)
        {
            const SemaType *valueType = ResolveType(parameters[i]->ValueType, ContextOf(symbol));
            argument.Value = valueType->IsError() ? nullptr : ConvertConstant(argument.Value, valueType);
            if (argument.Value == nullptr)
            {
                Diagnostics.Error(DiagnosticCode::TypeMismatch, nodes[i]->Range,
                                  "the generic argument does not fit the parameter `{0}`", parameters[i]->Name);
                return false;
            }
        }
        arguments.Add(argument);
    }
    // Trailing parameters with defaults may be left out (§9.4).
    for (usize i = nodes.Count(); i < parameters.Count() && parameters[i]->Default != nullptr; ++i)
    {
        SemaConstant *value = GetGenericDefault(symbol, i);
        if (value == nullptr)
        {
            return false;
        }
        arguments.Add({nullptr, value});
    }
    return true;
}

SemaConstant *Sema::GetGenericDefault(SemaSymbol *symbol, usize index)
{
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(symbol->Decl);
    if (index >= parameters.Count() || parameters[index]->Default == nullptr)
    {
        return nullptr;
    }
    const AstGenericParameterDecl *parameter = parameters[index];
    const SemaContext context = ContextOf(symbol);
    if (parameter->ValueType == nullptr)
    {
        // RECOVERY-DECISION lang-sema-D11: type parameters have no defaults; the note text is not recovered.
        Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, parameter->Default->Range,
                          "the default value does not fit the parameter `{0}`", parameter->Name)
            .WithNote("only constant generic parameters have defaults");
        return nullptr;
    }
    const SemaType *valueType = ResolveType(parameter->ValueType, context);
    if (valueType->IsError())
    {
        return nullptr;
    }
    return EvaluateConstantExpression(parameter->Default, context, valueType);
}

const SemaType *Sema::ResolveSymbolType(SemaSymbol *symbol, Span<AstNode *> genericArguments,
                                        const SemaContext &context, SourceRange range)
{
    UseFile(symbol->File);
    List<SemaGenericArgument> arguments;
    if (!ResolveGenericArguments(symbol, genericArguments, context, arguments))
    {
        return Types.GetError();
    }
    return GetTypeOfSymbol(symbol, arguments.AsSpan(), context, range);
}

bool Sema::ResolveGenericArgument(AstNode *node, const SemaContext &context, bool wantValue,
                                  SemaGenericArgument &argument)
{
    if (!wantValue)
    {
        argument.Type = ResolveType(node, context);
        return !argument.Type->IsError();
    }
    argument.Value = EvaluateConstantExpression(node, context, nullptr);
    return argument.Value != nullptr;
}

const SemaType *Sema::GetTypeOfSymbol(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments,
                                      const SemaContext &context, SourceRange range)
{
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(symbol->Decl);
    if (arguments.Count() != parameters.Count())
    {
        Diagnostics
            .Error(DiagnosticCode::InvalidGenericArguments, range, "`{0}` takes {1} generic argument(s) but {2} were given",
                   symbol->Name, parameters.Count(), arguments.Count())
            .WithSecondaryLabel(symbol->Decl != nullptr ? symbol->Decl->NameRange : SourceRange{}, "declared here");
        return Types.GetError();
    }
    switch (symbol->Kind)
    {
    case SemaSymbolKind::Alias:
    {
        if (symbol->Type != nullptr)
        {
            return symbol->Type;
        }
        if (symbol->TypeResolving)
        {
            Diagnostics.Error(DiagnosticCode::NotAType, range, "the alias `{0}` refers to itself", symbol->Name);
            return Types.GetError();
        }
        symbol->TypeResolving = true;
        symbol->Type = ResolveType(symbol->Decl->As<AstUsingDecl>()->Target, ContextOf(symbol));
        symbol->TypeResolving = false;
        return symbol->Type;
    }
    case SemaSymbolKind::Enum:
        return GetEnumType(symbol);
    case SemaSymbolKind::AssociatedType:
    {
        // Named inside its interface, struct or extension: the binding of the code's own type.
        const SemaType *self = context.SelfType;
        if (self == nullptr && context.Owner != nullptr && context.Owner->Symbol->Kind == SemaSymbolKind::Extension)
        {
            self = GetOwnerType(context.Owner);
        }
        return ResolveAssociatedType(symbol, context.Owner, self, range);
    }
    case SemaSymbolKind::Struct:
    case SemaSymbolKind::Class:
    case SemaSymbolKind::Interface:
    case SemaSymbolKind::Delegate:
        break;
    default:
        return Types.GetError();
    }

    List<SemaGenericBinding> bindings;
    for (usize i = 0; i < parameters.Count(); ++i)
    {
        bindings.Add({parameters[i]->Name, arguments[i]});
    }
    if (!CheckConstraints(symbol, bindings.AsSpan(), range))
    {
        return Types.GetError();
    }
    const auto builtin = static_cast<BuiltinTypeId>(symbol->BuiltinId);
    if (builtin != BuiltinTypeId::None && !IsPlainBuiltinStruct(builtin))
    {
        const SemaType *type = InstantiateBuiltinType(symbol, arguments, range);
        return type != nullptr ? type : Types.GetError();
    }
    return GetInstanceType(InstantiateStruct(symbol, arguments));
}

bool Sema::CheckConstraints(SemaSymbol *symbol, Span<const SemaGenericBinding> bindings, SourceRange range,
                            bool report)
{
    const SemaContext context = ContextOf(symbol, bindings);
    bool ok = true;
    for (const AstConstraint &constraint : ConstraintsOf(symbol->Decl))
    {
        const SemaType *type = nullptr;
        for (const SemaGenericBinding &binding : bindings)
        {
            if (binding.Name == constraint.Parameter)
            {
                type = binding.Argument.Type;
            }
        }
        if (type == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::UndefinedName, constraint.Range,
                              "`{0}` is not a type parameter of `{1}`", constraint.Parameter, symbol->Name);
            ok = false;
            continue;
        }
        if (type->IsError())
        {
            ok = false;
            continue;
        }
        const bool isValueType = !type->IsReference() && !type->Is(SemaTypeKind::Nullable) &&
                                 !type->Is(SemaTypeKind::Null) && !type->Is(SemaTypeKind::Void);
        if (constraint.Struct && !isValueType)
        {
            if (report)
            {
                Diagnostics.Error(DiagnosticCode::ConstraintNotSatisfied, range, "`{0}` must be a value type",
                                  TypeNameOf(type).AsView())
                    .WithSecondaryLabel(constraint.Range, "required by this constraint");
            }
            ok = false;
        }
        if (constraint.Class && !type->IsReference() &&
            !(type->Is(SemaTypeKind::Nullable) && type->GetElement()->IsReference()))
        {
            if (report)
            {
                Diagnostics.Error(DiagnosticCode::ConstraintNotSatisfied, range, "`{0}` must be a reference type",
                                  TypeNameOf(type).AsView())
                    .WithSecondaryLabel(constraint.Range, "required by this constraint");
            }
            ok = false;
        }
        if (constraint.Unmanaged && !type->IsStorable())
        {
            if (report)
            {
                Diagnostics.Error(DiagnosticCode::ConstraintNotSatisfied, range,
                                  "`{0}` must be made of numbers, vectors, matrices and enums",
                                  TypeNameOf(type).AsView());
            }
            ok = false;
        }
        if (constraint.New)
        {
            bool constructible = isValueType;
            if (type->Is(SemaTypeKind::Class) && type->GetStruct() != nullptr &&
                !HasAnyFlags(type->GetSymbol()->Decl->Modifiers, AstModifiers::Abstract | AstModifiers::Static))
            {
                for (SemaSymbol *member : GetMembers(type->GetSymbol(), type->GetSymbol()->Name))
                {
                    constructible = constructible || (member->Kind == SemaSymbolKind::Constructor &&
                                                      member->Visibility == SemaVisibility::Public &&
                                                      ParametersOf(member->Decl).IsEmpty());
                }
            }
            if (!constructible)
            {
                if (report)
                {
                    Diagnostics.Error(DiagnosticCode::ConstraintNotSatisfied, range,
                                      "`{0}` must have a public constructor without parameters",
                                      TypeNameOf(type).AsView())
                        .WithSecondaryLabel(constraint.Range, "required by this constraint");
                }
                ok = false;
            }
        }
        for (AstNode *bound : constraint.Bounds)
        {
            const SemaType *boundType = ResolveType(bound, context);
            if (boundType->IsError())
            {
                ok = false;
                continue;
            }
            if (boundType->Is(SemaTypeKind::Class))
            {
                if (!IsImplicitReference(type->Is(SemaTypeKind::Nullable) ? type->GetElement() : type, boundType))
                {
                    if (report)
                    {
                        Diagnostics
                            .Error(DiagnosticCode::ConstraintNotSatisfied, range, "`{0}` does not derive from `{1}`",
                                   TypeNameOf(type).AsView(), TypeNameOf(boundType).AsView())
                            .WithSecondaryLabel(constraint.Range, "required by this constraint");
                    }
                    ok = false;
                }
                continue;
            }
            if (!boundType->Is(SemaTypeKind::Interface))
            {
                Diagnostics.Error(DiagnosticCode::NotAType, bound->Range, "a constraint must be an interface or a class");
                ok = false;
                continue;
            }
            if (!ConformsTo(type, boundType))
            {
                if (report)
                {
                    Diagnostics
                        .Error(DiagnosticCode::ConstraintNotSatisfied, range, "`{0}` does not satisfy `{1}`",
                               TypeNameOf(type).AsView(), TypeNameOf(boundType).AsView())
                        .WithSecondaryLabel(constraint.Range, "required by this constraint");
                }
                ok = false;
            }
        }
    }
    return ok;
}

bool Sema::Satisfies(const SemaType *type, SemaSymbol *interfaceSymbol)
{
    const SemaType *scalar = type->Is(SemaTypeKind::Vector) || type->IsScalar() ? type->GetScalar() : nullptr;
    // lang-lower: a cooperative vector is numeric by its elements, so `max` and friends take it (§9.9).
    const SemaType *element = scalar != nullptr ? scalar
                              : type->Is(SemaTypeKind::CoopVector) ? type->GetElement()
                                                                   : nullptr;
    switch (static_cast<BuiltinTypeId>(interfaceSymbol->BuiltinId))
    {
    case BuiltinTypeId::IScalar:
        return type->IsScalar();
    case BuiltinTypeId::IScalarOrVector:
        return scalar != nullptr;
    case BuiltinTypeId::INumeric:
        return element != nullptr && element->IsNumeric();
    case BuiltinTypeId::IInteger:
        return element != nullptr && element->Is(SemaTypeKind::Int);
    case BuiltinTypeId::IFloatingPoint:
        return element != nullptr && element->Is(SemaTypeKind::Float);
    case BuiltinTypeId::ILogical:
        return scalar != nullptr && scalar->Is(SemaTypeKind::Bool);
    case BuiltinTypeId::IDifferentiable:
    {
        const bool differentiable = IsDifferentiable(type);
        if (!differentiable && type->Is(SemaTypeKind::Struct))
        {
            // Reports a struct that says it implements IDifferentiable but is not differentiable.
            ResolveHierarchy(type->GetStruct());
        }
        return differentiable;
    }
    case BuiltinTypeId::IComparable:
        if (type->IsScalar() && type->IsNumeric())
        {
            return true;
        }
        if (type->Is(SemaTypeKind::String))
        {
            return true;
        }
        break;
    default:
        break;
    }
    if (type->Is(SemaTypeKind::String))
    {
        SemaSymbol *stringSymbol = GetMagicSymbol(static_cast<uint32>(BuiltinTypeId::String));
        type = stringSymbol != nullptr && stringSymbol->Kind == SemaSymbolKind::Class
                   ? GetInstanceType(InstantiateStruct(stringSymbol, {}))
                   : type;
    }
    if (!type->HasInstance())
    {
        return false;
    }
    SemaStructInstance *instance = type->GetStruct();
    if (instance->Symbol == interfaceSymbol)
    {
        return true;
    }
    if (!ResolveHierarchy(instance))
    {
        return false;
    }
    for (const SemaStructInstance *implemented : instance->Interfaces)
    {
        if (implemented->Symbol == interfaceSymbol)
        {
            return true;
        }
    }
    return false;
}

bool Sema::IsDifferentiable(const SemaType *type)
{
    switch (type->GetKind())
    {
    case SemaTypeKind::Float:
        return true;
    case SemaTypeKind::Vector:
    case SemaTypeKind::Matrix:
        return type->GetElement()->Is(SemaTypeKind::Float);
    case SemaTypeKind::Array:
        return IsDifferentiable(type->GetElement());
    case SemaTypeKind::Struct:
    {
        const SemaField *blocking = nullptr;
        return IsDifferentiableStruct(type, blocking);
    }
    default:
        return false;
    }
}

bool Sema::IsDifferentiableStruct(const SemaType *type, const SemaField *&blocking)
{
    blocking = nullptr;
    SemaStructInstance *instance = type->GetStruct();
    if (instance->FieldsResolving || !ResolveFields(instance))
    {
        return false;
    }
    bool any = false;
    for (const SemaField &field : instance->Fields)
    {
        if (IsNoDiff(field.Symbol->Decl->Attributes))
        {
            continue;
        }
        if (!IsDifferentiable(field.Type))
        {
            blocking = &field;
            return false;
        }
        any = true;
    }
    return any;
}

bool Sema::IsNoDiff(Span<AstAttribute> attributes)
{
    return FindAttribute(attributes, "NoDiff") != nullptr;
}

bool Sema::IsMarkedDifferentiable(const SemaSymbol *symbol, SemaStructInstance *owner)
{
    if (FindAttribute(symbol->Decl->Attributes, "Differentiable") != nullptr)
    {
        return true;
    }
    if (owner == nullptr || symbol->Kind != SemaSymbolKind::Function || !ResolveHierarchy(owner))
    {
        return false;
    }
    for (const SemaStructInstance *implemented : owner->Interfaces)
    {
        const List<SemaSymbol *> *members = implemented->Symbol->Members.Find(symbol->Name);
        for (usize i = 0; members != nullptr && i < members->Count(); ++i)
        {
            const SemaSymbol *member = (*members)[i];
            if (member->Kind == SemaSymbolKind::Function &&
                FindAttribute(member->Decl->Attributes, "Differentiable") != nullptr)
            {
                return true;
            }
        }
    }
    return false;
}

SemaStructInstance *Sema::InstantiateStruct(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments)
{
    if (SemaStructInstance **found = _structInstances.Find(StructKey{symbol, arguments}))
    {
        return *found;
    }
    _structs.Add(MakeUnique<SemaStructInstance>());
    SemaStructInstance *instance = _structs.Last().Get();
    instance->Symbol = symbol;
    List<SemaGenericArgument> copy;
    copy.AddRange(arguments);
    instance->Arguments = Freeze(copy);
    _structInstances.Add(StructKey{symbol, instance->Arguments}, instance);
    return instance;
}

void Sema::GetBindings(SemaStructInstance *instance, List<SemaGenericBinding> &bindings)
{
    if (instance == nullptr)
    {
        return;
    }
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(instance->Symbol->Decl);
    for (usize i = 0; i < parameters.Count() && i < instance->Arguments.Count(); ++i)
    {
        bindings.Add({parameters[i]->Name, instance->Arguments[i]});
    }
}

void Sema::GetBindings(SemaFunction *function, List<SemaGenericBinding> &bindings)
{
    GetBindings(function->Owner, bindings);
    const Span<AstGenericParameterDecl *> parameters = GenericParametersOf(function->Symbol->Decl);
    for (usize i = 0; i < parameters.Count() && i < function->GenericArguments.Count(); ++i)
    {
        bindings.Add({parameters[i]->Name, function->GenericArguments[i]});
    }
}

bool Sema::ResolveFields(SemaStructInstance *instance)
{
    if (instance->FieldsResolved)
    {
        return true;
    }
    SemaSymbol *symbol = instance->Symbol;
    if (symbol->Kind == SemaSymbolKind::Interface || symbol->Kind == SemaSymbolKind::Delegate ||
        symbol->Kind == SemaSymbolKind::Extension)
    {
        instance->FieldsResolved = true;
        return true;
    }
    if (instance->FieldsResolving)
    {
        Diagnostics.Error(DiagnosticCode::LayoutViolation, symbol->Decl->NameRange,
                          "the struct `{0}` contains itself", symbol->Name)
            .WithLabel("a struct cannot hold a value of its own type");
        return false;
    }
    instance->FieldsResolving = true;
    UseFile(symbol->File);
    List<SemaGenericBinding> bindings;
    GetBindings(instance, bindings);
    const SemaContext context = ContextOf(symbol, bindings.AsSpan(), instance);
    bool ok = CheckDeclaredAttributes(symbol->Decl, SemaAttributeTarget::Struct, context);
    if (symbol->Kind == SemaSymbolKind::Class)
    {
        // A class's fields start with those of its base, so a base reference sees them at the same index.
        if (!ResolveHierarchy(instance))
        {
            ok = false;
        }
        else if (instance->Base != nullptr)
        {
            if (ResolveFields(instance->Base))
            {
                instance->Fields.AddRange(instance->Base->Fields.AsSpan());
            }
            else
            {
                ok = false;
            }
        }
    }
    for (SemaSymbol *member : symbol->Ordered)
    {
        const bool autoProperty = member->Kind == SemaSymbolKind::Property && IsAutoProperty(member);
        if ((member->Kind != SemaSymbolKind::Field && !autoProperty) || member->IsStatic())
        {
            continue;
        }
        AstNode *typeNode = autoProperty ? member->Decl->As<AstPropertyDecl>()->Type
                                         : member->Decl->As<AstFieldDecl>()->Type;
        SemaField semaField;
        semaField.Name = member->Name;
        semaField.Symbol = member;
        semaField.Type = ResolveDeclaredType(typeNode, member->Decl->Attributes, context);
        ok = CheckDeclaredAttributes(member->Decl, SemaAttributeTarget::Field, context) && ok;
        if (semaField.Type->IsError())
        {
            ok = false;
            continue;
        }
        if (semaField.Type->Is(SemaTypeKind::Struct) && !ResolveFields(semaField.Type->GetStruct()))
        {
            ok = false;
            continue;
        }
        if (const AstAttribute *offset = FindAttribute(member->Decl->Attributes, "Offset"))
        {
            if (SemaConstant *value = EvaluateAttributeArgument(*offset, 0, {}, context))
            {
                semaField.Offset = static_cast<uint32>(value->Bits);
            }
        }
        instance->Fields.Add(semaField);
    }
    if (const AstAttribute *layout = FindAttribute(symbol->Decl->Attributes, "StructLayout"))
    {
        if (SemaConstant *size = EvaluateAttributeArgument(*layout, 0, "Size", context))
        {
            instance->Size = static_cast<uint32>(size->Bits);
        }
    }
    instance->FieldsResolving = false;
    instance->FieldsResolved = ok;
    return ok;
}

SemaFunction *Sema::InstantiateFunction(SemaSymbol *symbol, SemaStructInstance *owner,
                                        Span<const SemaGenericArgument> arguments, SemaAccessor accessor)
{
    if (accessor == SemaAccessor::None && symbol->Kind == SemaSymbolKind::Property)
    {
        accessor = SemaAccessor::Getter;
    }
    if (SemaFunction **found = _functionInstances.Find(FunctionKey{symbol, owner, arguments, accessor}))
    {
        return *found;
    }
    _functions.Add(MakeUnique<SemaFunction>());
    SemaFunction *function = _functions.Last().Get();
    function->Symbol = symbol;
    function->Owner = owner;
    function->Accessor = accessor;
    List<SemaGenericArgument> copy;
    copy.AddRange(arguments);
    function->GenericArguments = Freeze(copy);
    function->IsConstFunction = HasAnyFlags(symbol->Decl->Modifiers, AstModifiers::Const);
    _functionInstances.Add(FunctionKey{symbol, owner, function->GenericArguments, accessor}, function);
    return function;
}

const SemaType *Sema::GetOwnerType(SemaStructInstance *owner)
{
    if (owner->Symbol->Kind == SemaSymbolKind::Extension)
    {
        if (owner->Extended == nullptr)
        {
            // The extended type is written outside the extension's body: its members do not hide other names.
            List<SemaGenericBinding> bindings;
            GetBindings(owner, bindings);
            SemaContext context = ContextOf(owner->Symbol, Freeze(bindings));
            context.TypeSymbol = nullptr;
            owner->Extended = ResolveType(owner->Symbol->Decl->As<AstExtensionDecl>()->ExtendedType, context);
        }
        return owner->Extended;
    }
    const auto builtin = static_cast<BuiltinTypeId>(owner->Symbol->BuiltinId);
    if (builtin != BuiltinTypeId::None && !IsPlainBuiltinStruct(builtin))
    {
        const SemaType *type = InstantiateBuiltinType(owner->Symbol, owner->Arguments, owner->Symbol->Decl->NameRange);
        return type != nullptr ? type : Types.GetError();
    }
    return owner->Symbol->Kind == SemaSymbolKind::Struct ? Types.GetStruct(owner) : GetInstanceType(owner);
}

bool Sema::ResolveSignature(SemaFunction *function)
{
    if (function->SignatureResolved)
    {
        return function->ReturnType != nullptr && !function->ReturnType->IsError();
    }
    function->SignatureResolved = true;
    SemaSymbol *symbol = function->Symbol;
    UseFile(symbol->File);
    List<SemaGenericBinding> bindings;
    GetBindings(function, bindings);
    const Span<const SemaGenericBinding> frozen = Freeze(bindings);
    const SemaContext context = ContextOf(symbol, frozen, function->Owner);
    AstDecl *decl = symbol->Decl;

    Span<AstParameterDecl *> parameters;
    const AstBody *body = nullptr;
    const SemaType *valueType = nullptr;
    switch (decl->Kind)
    {
    case AstKind::FunctionDecl:
    {
        auto *functionDecl = decl->As<AstFunctionDecl>();
        function->ReturnType = ResolveType(functionDecl->ReturnType, context);
        parameters = functionDecl->Parameters;
        body = &functionDecl->Body;
        break;
    }
    case AstKind::ConstructorDecl:
    {
        auto *constructor = decl->As<AstConstructorDecl>();
        if (function->Owner == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidStatement, decl->NameRange,
                              "only classes and structs have constructors");
            function->ReturnType = Types.GetError();
            return false;
        }
        function->ReturnType = GetOwnerType(function->Owner);
        parameters = constructor->Parameters;
        body = &constructor->Body;
        break;
    }
    case AstKind::OperatorDecl:
    {
        auto *op = decl->As<AstOperatorDecl>();
        function->ReturnType = ResolveType(op->ReturnType, context);
        parameters = op->Parameters;
        body = &op->Body;
        break;
    }
    case AstKind::PropertyDecl:
    {
        auto *property = decl->As<AstPropertyDecl>();
        const SemaType *propertyType = ResolveType(property->Type, context);
        parameters = property->Parameters;
        if (function->Accessor == SemaAccessor::Setter)
        {
            function->ReturnType = propertyType->IsError() ? propertyType : Types.GetVoid();
            valueType = propertyType;
            body = &property->Setter.Body;
        }
        else
        {
            function->ReturnType = propertyType;
            body = &property->Getter;
        }
        break;
    }
    default:
        function->ReturnType = Types.GetError();
        return false;
    }

    bool ok = !function->ReturnType->IsError();
    const bool isInstance = function->Owner != nullptr && decl->Kind != AstKind::OperatorDecl &&
                            (decl->Kind != AstKind::ConstructorDecl ||
                             function->Owner->Symbol->Kind == SemaSymbolKind::Class) &&
                            !HasAnyFlags(decl->Modifiers, AstModifiers::Static | AstModifiers::Const);
    if (isInstance)
    {
        // A class's `this` is a reference passed by value; a struct's is the value passed by reference. An extension's
        // is the extended value, passed by value and read-only like an `in` parameter (§3.5).
        const SemaType *ownerType = GetOwnerType(function->Owner);
        const bool inExtension = function->Owner->Symbol->Kind == SemaSymbolKind::Extension;
        function->This = New<SemaLocal>();
        function->This->Name = "this";
        function->This->Type = ownerType;
        function->This->Mode =
            ownerType->IsReference() || inExtension ? SemaParameterMode::Value : SemaParameterMode::Ref;
        function->This->IsParameter = true;
        function->This->Function = function;
        // Material code reads its parameters and never changes them (§9.4).
        function->This->IsReadOnly =
            inExtension || FindAttribute(function->Owner->Symbol->Decl->Attributes, "Material") != nullptr;
        ok = ok && !ownerType->IsError();
    }
    ok = CheckDeclaredAttributes(decl, SemaAttributeTarget::Function, context) && ok;
    for (AstGenericParameterDecl *parameter : GenericParametersOf(decl))
    {
        ok = CheckDeclaredAttributes(parameter, SemaAttributeTarget::GenericParameter, context) && ok;
    }
    for (AstParameterDecl *parameter : parameters)
    {
        SemaLocal *local = New<SemaLocal>();
        local->Name = parameter->Name;
        local->Range = parameter->NameRange;
        local->IsParameter = true;
        local->Attributes = parameter->Attributes;
        local->Function = function;
        local->Type = ResolveDeclaredType(parameter->Type, parameter->Attributes, context);
        local->Mode = HasAnyFlags(parameter->Modifiers, AstModifiers::Ref)   ? SemaParameterMode::Ref
                      : HasAnyFlags(parameter->Modifiers, AstModifiers::Out) ? SemaParameterMode::Out
                      : HasAnyFlags(parameter->Modifiers, AstModifiers::In)  ? SemaParameterMode::In
                                                                             : SemaParameterMode::Value;
        local->IsReadOnly = local->Mode == SemaParameterMode::In;
        ok = CheckDeclaredAttributes(parameter, SemaAttributeTarget::Parameter, context) && ok;
        ok = ok && !local->Type->IsError();

        function->Parameters.Add(local);
    }
    if (valueType != nullptr)
    {
        SemaLocal *value = New<SemaLocal>();
        value->Name = "value";
        value->Range = decl->NameRange;
        value->IsParameter = true;
        value->Function = function;
        value->Type = valueType;
        ok = ok && !valueType->IsError();
        function->Parameters.Add(value);
    }

    for (const AstAttribute &attribute : decl->Attributes)
    {
        if (attribute.Name == "Requires")
        {
            for (usize i = 0; i < attribute.Arguments.Count(); ++i)
            {
                SemaConstant *feature = EvaluateAttributeArgument(attribute, i, {}, context);
                if (feature != nullptr && feature->Type->Is(SemaTypeKind::Feature))
                {
                    function->Requires.Add(feature->Text);
                }
                else if (feature != nullptr)
                {
                    Diagnostics.Error(DiagnosticCode::InvalidAttribute, attribute.Arguments[i].Value->Range,
                                      "`[Requires]` takes features")
                        .WithHelp("features are constants such as `Features.CooperativeVector` (§9.7)");
                }
            }
        }
        else if (attribute.Name == "Stage")
        {
            if (SemaConstant *stages = EvaluateAttributeArgument(attribute, 0, {}, context))
            {
                function->AllowedStages = static_cast<uint32>(stages->Bits);
                function->StageUse = attribute.Range;
            }
        }
    }

    // lang-lower: the bodyless constructors of the core library's MatrixRef and VectorRef build the view from their
    // arguments (lowering, lang-spirv-D4).
    const bool isViewConstructor = symbol->IsCore && decl->Kind == AstKind::ConstructorDecl &&
                                   function->Owner != nullptr &&
                                   (function->ReturnType->Is(SemaTypeKind::MatrixRef) ||
                                    function->ReturnType->Is(SemaTypeKind::VectorRef));
    const bool isBuiltin = FindAttribute(decl->Attributes, "Intrinsic") != nullptr ||
                           FindAttribute(decl->Attributes, "SystemValue") != nullptr ||
                           FindAttribute(decl->Attributes, "CompileTime") != nullptr ||
                           FindAttribute(decl->Attributes, "Native") != nullptr || isViewConstructor;
    if (isBuiltin && !symbol->IsCore)
    {
        Diagnostics.Error(DiagnosticCode::InvalidAttribute, decl->NameRange, "only the core library declares built-ins")
            .WithNote("`[Intrinsic]`, `[SystemValue]`, `[CompileTime]` and `[Native]` are allowed in the core library "
                      "(§9.10, MarlinScript §10)");
        ok = false;
    }
    // Virtual members, their bodies and generic parameters are checked with the class (ResolveHierarchy).
    const bool inInterface = symbol->Parent != nullptr && symbol->Parent->Kind == SemaSymbolKind::Interface;
    const bool isVirtual = HasAnyFlags(decl->Modifiers, AstModifiers::Virtual | AstModifiers::Abstract |
                                                            AstModifiers::Override);
    const bool isAuto = decl->Kind == AstKind::PropertyDecl && IsAutoProperty(symbol);
    if (body != nullptr && body->IsMissing() && !isBuiltin && !inInterface && !isVirtual && !isAuto)
    {
        Diagnostics.Error(DiagnosticCode::Expected, decl->NameRange, "`{0}` has no body", symbol->Name)
            .WithLabel("needs a block or `=> expression`");
        ok = false;
    }
    if (!ok)
    {
        function->State = SemaFunctionState::Failed;
    }
    return ok;
}

const SemaType *Sema::GetEnumType(SemaSymbol *symbol)
{
    if (symbol->Type != nullptr)
    {
        return symbol->Type;
    }
    const auto *decl = symbol->Decl->As<AstEnumDecl>();
    const SemaType *underlying = Types.GetInt(32, true);
    if (decl->UnderlyingType != nullptr)
    {
        underlying = ResolveType(decl->UnderlyingType, ContextOf(symbol));
        if (!underlying->Is(SemaTypeKind::Int))
        {
            Diagnostics.Error(DiagnosticCode::TypeMismatch, decl->UnderlyingType->Range,
                              "the underlying type of an enum must be an integer");
            underlying = Types.GetInt(32, true);
        }
    }
    symbol->Type = Types.GetEnum(symbol, underlying);
    return symbol->Type;
}

SemaConstant *Sema::GetEnumMemberValue(SemaSymbol *member)
{
    if (member->Value != nullptr)
    {
        return member->Value;
    }
    SemaSymbol *enumSymbol = member->Parent;
    const SemaType *enumType = GetEnumType(enumSymbol);
    if (member->ValueResolving)
    {
        Diagnostics.Error(DiagnosticCode::NotConstant, member->Decl->NameRange,
                          "the value of `{0}` depends on itself", member->Name);
        return nullptr;
    }
    member->ValueResolving = true;
    const auto *decl = member->Decl->As<AstEnumMemberDecl>();
    uint64 bits = 0;
    if (decl->Value != nullptr)
    {
        SemaContext context = ContextOf(member);
        context.TypeSymbol = enumSymbol;
        SemaConstant *value = EvaluateConstantExpression(decl->Value, context, nullptr);
        if (value == nullptr)
        {
            member->ValueResolving = false;
            return nullptr;
        }
        if (value->Type == enumType)
        {
            bits = value->Bits;
        }
        else if (SemaConstant *converted = ConvertConstant(value, enumType->GetElement()))
        {
            bits = converted->Bits;
        }
        else
        {
            Diagnostics.Error(DiagnosticCode::TypeMismatch, decl->Value->Range,
                              "an enum member's value must be an integer constant");
        }
    }
    else
    {
        const List<SemaSymbol *> &members = enumSymbol->Ordered;
        const usize index = members.IndexOf(member);
        if (index > 0)
        {
            if (SemaConstant *previous = GetEnumMemberValue(members[index - 1]))
            {
                bits = previous->Bits + 1;
            }
        }
    }
    member->ValueResolving = false;
    member->Value = NewScalar(enumType, bits);
    return member->Value;
}

SemaConstant *Sema::GetStaticValue(SemaSymbol *field, SemaStructInstance *owner)
{
    const bool cacheable = owner == nullptr || owner->Arguments.IsEmpty();
    if (cacheable && field->Value != nullptr)
    {
        return field->Value;
    }
    if (field->ValueResolving)
    {
        Diagnostics.Error(DiagnosticCode::NotConstant, field->Decl->NameRange, "the value of `{0}` depends on itself",
                          field->Name);
        return nullptr;
    }
    const auto *decl = field->Decl->As<AstFieldDecl>();
    if (decl->Initializer == nullptr)
    {
        Diagnostics.Error(DiagnosticCode::NotConstant, field->Decl->NameRange, "`{0}` needs a value", field->Name)
            .WithLabel("a `const` or `static readonly` field must be initialized");
        return nullptr;
    }
    UseFile(field->File);
    field->ValueResolving = true;
    List<SemaGenericBinding> bindings;
    GetBindings(owner, bindings);
    const SemaContext context = ContextOf(field, bindings.AsSpan(), owner);
    const SemaType *type = ResolveType(decl->Type, context);
    SemaConstant *value = type->IsError() ? nullptr : EvaluateConstantExpression(decl->Initializer, context, type);
    field->ValueResolving = false;
    if (cacheable)
    {
        field->Value = value;
    }
    return value;
}

SemaGlobal *Sema::GetGlobal(SemaSymbol *field)
{
    if (SemaGlobal **found = _globalsBySymbol.Find(field))
    {
        return *found;
    }
    const auto *decl = field->Decl->As<AstFieldDecl>();
    _globals.Add(MakeUnique<SemaGlobal>());
    SemaGlobal *global = _globals.Last().Get();
    global->Symbol = field;
    _globalsBySymbol.Add(field, global);
    UseFile(field->File);
    const SemaContext context = ContextOf(field);
    global->Type = ResolveDeclaredType(decl->Type, decl->Attributes, context);
    const AstAttribute *specialization = FindAttribute(decl->Attributes, "Specialization");
    const AstAttribute *groupShared = FindAttribute(decl->Attributes, "GroupShared");
    if ((specialization != nullptr || groupShared != nullptr) && context.Dialect == LanguageDialect::Script)
    {
        if (specialization != nullptr)
        {
            Diagnostics.Error(DiagnosticCode::NotInDialect, specialization->Range,
                              "specialization constants exist only in shaders");
        }
        else
        {
            Diagnostics.Error(DiagnosticCode::NotInDialect, groupShared->Range, "groupshared memory exists only in shaders");
        }
        global->Type = Types.GetError();
    }
    else if (specialization != nullptr)
    {
        global->Kind = SemaGlobalKind::Specialization;
        const SemaType *type = global->Type;
        if (!type->IsError() && !type->Is(SemaTypeKind::Bool) && !type->Is(SemaTypeKind::Int) &&
            !type->Is(SemaTypeKind::Float))
        {
            Diagnostics.Error(DiagnosticCode::InvalidAttribute, specialization->Range,
                              "a specialization constant is a `bool`, integer or floating-point scalar, not `{0}`",
                              TypeNameOf(type).AsView())
                .WithLabel("this attribute");
            global->Type = Types.GetError();
        }
        else if (decl->Initializer == nullptr)
        {
            Diagnostics.Error(DiagnosticCode::NotConstant, decl->NameRange,
                              "a specialization constant needs a default value")
                .WithLabel("declared here");
        }
        else if (!type->IsError())
        {
            global->Default = EvaluateConstantExpression(decl->Initializer, context, type);
        }
    }
    else if (groupShared != nullptr)
    {
        global->Kind = SemaGlobalKind::GroupShared;
        if (decl->Initializer != nullptr)
        {
            Diagnostics.Error(DiagnosticCode::InvalidStatement, decl->Initializer->Range,
                              "groupshared memory cannot have an initial value");
        }
    }
    else
    {
        Diagnostics.Error(DiagnosticCode::InvalidStatement, decl->NameRange,
                          "a static field must be `const`, `static readonly`, `[GroupShared]` or `[Specialization]`")
            .WithNote("shaders have no mutable global variables (§3.1)");
        global->Type = Types.GetError();
    }
    return global;
}

const SemaType *Sema::InstantiateBuiltinType(SemaSymbol *symbol, Span<const SemaGenericArgument> arguments,
                                             SourceRange range)
{
    auto typeArgument = [&](usize index) { return index < arguments.Count() ? arguments[index].Type : nullptr; };
    auto valueArgument = [&](usize index) -> uint32 {
        return index < arguments.Count() && arguments[index].Value != nullptr
                   ? static_cast<uint32>(arguments[index].Value->Bits)
                   : 0;
    };
    auto checkCount = [&](uint32 value, StringView what) {
        if (value < 2 || value > 4)
        {
            Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, range, "a {0} has 2 to 4 {1}, not {2}",
                              symbol->Name, what, value);
            return false;
        }
        return true;
    };
    // Texture<T, Access A, ImageFormat F> (§4.4): Access.Read is 0, and ImageFormat.Inferred (0) picks the format
    // from the texel type for a writable texture.
    auto texture = [&](MirTextureDim dim, bool arrayed, bool multisampled) -> const SemaType * {
        const SemaType *texel = typeArgument(0);
        const bool writable = valueArgument(1) != 0;
        const auto declared = static_cast<MirImageFormat>(valueArgument(2));
        if (!writable || texel == nullptr)
        {
            if (declared != MirImageFormat::Unknown)
            {
                Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, range,
                                  "only a writable texture has a storage format")
                    .WithHelp("give it `Access.Write` or `Access.ReadWrite`, or leave the format out");
                return nullptr;
            }
            return Types.GetTexture(texel, dim, arrayed, multisampled, false, MirImageFormat::Unknown);
        }
        if (declared == MirImageFormat::Unknown)
        {
            return Types.GetTexture(texel, dim, arrayed, multisampled, true, InferImageFormat(texel));
        }
        if (!FormatHoldsTexel(declared, texel))
        {
            Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, range, "the format does not hold `{0}` texels",
                              TypeNameOf(texel).AsView())
                .WithNote("float, unorm and snorm formats hold floating-point texels; `i` formats hold signed and `u` "
                          "formats unsigned integers");
            return nullptr;
        }
        return Types.GetTexture(texel, dim, arrayed, multisampled, true, declared);
    };
    switch (static_cast<BuiltinTypeId>(symbol->BuiltinId))
    {
    case BuiltinTypeId::IScalar:
    case BuiltinTypeId::IScalarOrVector:
    case BuiltinTypeId::INumeric:
    case BuiltinTypeId::IInteger:
    case BuiltinTypeId::IFloatingPoint:
    case BuiltinTypeId::ILogical:
    case BuiltinTypeId::IComparable:
    case BuiltinTypeId::IDifferentiable:
        return Types.GetInterface(symbol);
    case BuiltinTypeId::ScalarOf:
        if (typeArgument(0) == nullptr || typeArgument(0)->GetScalar() == nullptr ||
            typeArgument(0)->Is(SemaTypeKind::Matrix))
        {
            Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, range,
                              "`ScalarOf` takes a scalar or vector type");
            return nullptr;
        }
        return typeArgument(0)->GetScalar();
    case BuiltinTypeId::Vector:
        return checkCount(valueArgument(1), "components") ? Types.GetVector(typeArgument(0), valueArgument(1))
                                                           : nullptr;
    case BuiltinTypeId::Matrix:
        return checkCount(valueArgument(1), "rows") && checkCount(valueArgument(2), "columns")
                   ? Types.GetMatrix(typeArgument(0), valueArgument(1), valueArgument(2))
                   : nullptr;
    case BuiltinTypeId::String:
        return Types.GetString();
    case BuiltinTypeId::Object:
        return Types.GetClass(InstantiateStruct(symbol, arguments));
    case BuiltinTypeId::Array:
        return typeArgument(0) != nullptr ? Types.GetGcArray(typeArgument(0)) : nullptr;
    case BuiltinTypeId::Feature:
        return Types.GetFeature();
    case BuiltinTypeId::Texture1D:
        return texture(MirTextureDim::Texture1D, false, false);
    case BuiltinTypeId::Texture1DArray:
        return texture(MirTextureDim::Texture1D, true, false);
    case BuiltinTypeId::Texture2D:
        return texture(MirTextureDim::Texture2D, false, false);
    case BuiltinTypeId::Texture2DArray:
        return texture(MirTextureDim::Texture2D, true, false);
    case BuiltinTypeId::Texture2DMultisample:
        return texture(MirTextureDim::Texture2D, false, true);
    case BuiltinTypeId::Texture3D:
        return texture(MirTextureDim::Texture3D, false, false);
    case BuiltinTypeId::TextureCube:
        return texture(MirTextureDim::Cube, false, false);
    case BuiltinTypeId::TextureCubeArray:
        return texture(MirTextureDim::Cube, true, false);
    case BuiltinTypeId::Ptr:
    {
        // Access.Read is 0; Write and ReadWrite both make the pointer writable (§4.5).
        const SemaType *element = typeArgument(0);
        const bool writable = valueArgument(1) != 0;
        if (element->Is(SemaTypeKind::Struct) &&
            element->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Byte))
        {
            return Types.GetBuffer(nullptr, writable);
        }
        if (element->Is(SemaTypeKind::Struct) && !ResolveFields(element->GetStruct()))
        {
            return nullptr;
        }
        if (!element->IsStorable())
        {
            Diagnostics.Error(DiagnosticCode::InvalidGenericArguments, range, "`{0}` cannot be stored in GPU memory",
                              TypeNameOf(element).AsView())
                .WithNote("GPU memory holds numbers, vectors, matrices, enums, handles, pointers and structs of them (§4.5)");
            return nullptr;
        }
        return Types.GetBuffer(element, writable);
    }
    case BuiltinTypeId::Sampler:
        return Types.GetSampler(false);
    case BuiltinTypeId::ComparisonSampler:
        return Types.GetSampler(true);
    case BuiltinTypeId::AccelerationStructure:
        return Types.GetAccelerationStructure();
    case BuiltinTypeId::RayQuery:
        return Types.GetRayQuery(valueArgument(0));
    case BuiltinTypeId::HitObject:
        return Types.GetHitObject();
    case BuiltinTypeId::CoopVector:
        return Types.GetCoopVector(typeArgument(0), valueArgument(1));
    case BuiltinTypeId::CoopMatrix:
        return Types.GetCoopMatrix(typeArgument(0), valueArgument(1), valueArgument(2),
                                   static_cast<MirMatrixUse>(valueArgument(3)));
    case BuiltinTypeId::MatrixRef:
        return Types.GetMatrixRef(typeArgument(0), valueArgument(1), valueArgument(2), SemaMatrixLayout::RowMajor);
    case BuiltinTypeId::VectorRef:
        return Types.GetVectorRef(typeArgument(0), valueArgument(1));
    case BuiltinTypeId::Vertices:
    case BuiltinTypeId::Indices:
    case BuiltinTypeId::Primitives:
    {
        const auto id = static_cast<BuiltinTypeId>(symbol->BuiltinId);
        const SemaMeshOutput kind = id == BuiltinTypeId::Vertices  ? SemaMeshOutput::Vertices
                                    : id == BuiltinTypeId::Indices ? SemaMeshOutput::Indices
                                                                   : SemaMeshOutput::Primitives;
        return Types.GetMeshOutput(kind, typeArgument(0), valueArgument(1));
    }
    default:
        Diagnostics.Error(DiagnosticCode::InvalidAttribute, range, "`{0}` is not a magic type the compiler knows",
                          symbol->Name);
        return nullptr;
    }
}
} // namespace Marlin

namespace Marlin
{
uint32 Sema::GetMagicFamily(const SemaType *type, List<SemaGenericArgument> &arguments)
{
    auto addType = [&](const SemaType *argument) { arguments.Add({argument, nullptr}); };
    auto addValue = [&](uint64 value) {
        arguments.Add({nullptr, NewScalar(Types.GetInt(32, true), value)});
    };
    BuiltinTypeId id = BuiltinTypeId::None;
    switch (type->GetKind())
    {
    case SemaTypeKind::Vector:
        id = BuiltinTypeId::Vector;
        addType(type->GetElement());
        addValue(type->GetCount());
        break;
    case SemaTypeKind::Matrix:
        id = BuiltinTypeId::Matrix;
        addType(type->GetElement());
        addValue(type->GetRows());
        addValue(type->GetCount());
        break;
    case SemaTypeKind::Texture:
        switch (type->GetTextureDim())
        {
        case MirTextureDim::Texture1D:
            id = type->IsArrayed() ? BuiltinTypeId::Texture1DArray : BuiltinTypeId::Texture1D;
            break;
        case MirTextureDim::Texture2D:
            id = type->IsMultisampled() ? BuiltinTypeId::Texture2DMultisample
                 : type->IsArrayed()    ? BuiltinTypeId::Texture2DArray
                                        : BuiltinTypeId::Texture2D;
            break;
        case MirTextureDim::Texture3D:
            id = BuiltinTypeId::Texture3D;
            break;
        case MirTextureDim::Cube:
            id = type->IsArrayed() ? BuiltinTypeId::TextureCubeArray : BuiltinTypeId::TextureCube;
            break;
        }
        addType(type->GetElement());
        // Access.ReadWrite is 2; the format as ImageFormat numbers it.
        addValue(type->IsWritable() ? 2 : 0);
        addValue(static_cast<uint64>(type->GetImageFormat()));
        break;
    case SemaTypeKind::Buffer:
    {
        id = BuiltinTypeId::Ptr;
        const SemaType *element = type->GetElement();
        if (element == nullptr)
        {
            SemaSymbol *byte = GetMagicSymbol(static_cast<uint32>(BuiltinTypeId::Byte));
            element = byte != nullptr ? Types.GetStruct(InstantiateStruct(byte, {})) : Types.GetError();
        }
        addType(element);
        // Access.ReadWrite is 2.
        addValue(type->IsWritable() ? 2 : 0);
        break;
    }
    case SemaTypeKind::Sampler:
        id = type->IsComparison() ? BuiltinTypeId::ComparisonSampler : BuiltinTypeId::Sampler;
        break;
    case SemaTypeKind::AccelerationStructure:
        id = BuiltinTypeId::AccelerationStructure;
        break;
    case SemaTypeKind::RayQuery:
        id = BuiltinTypeId::RayQuery;
        addValue(0);
        break;
    case SemaTypeKind::HitObject:
        id = BuiltinTypeId::HitObject;
        break;
    case SemaTypeKind::CoopVector:
        id = BuiltinTypeId::CoopVector;
        addType(type->GetElement());
        addValue(type->GetCount());
        break;
    case SemaTypeKind::CoopMatrix:
        id = BuiltinTypeId::CoopMatrix;
        addType(type->GetElement());
        addValue(type->GetRows());
        addValue(type->GetCount());
        addValue(static_cast<uint64>(type->GetMatrixUse()));
        break;
    case SemaTypeKind::MatrixRef:
        id = BuiltinTypeId::MatrixRef;
        addType(type->GetElement());
        addValue(type->GetRows());
        addValue(type->GetCount());
        break;
    case SemaTypeKind::VectorRef:
        id = BuiltinTypeId::VectorRef;
        addType(type->GetElement());
        addValue(type->GetCount());
        break;
    case SemaTypeKind::Feature:
        id = BuiltinTypeId::Feature;
        break;
    case SemaTypeKind::String:
        id = BuiltinTypeId::String;
        break;
    case SemaTypeKind::GcArray:
        id = BuiltinTypeId::Array;
        addType(type->GetElement());
        break;

    case SemaTypeKind::MeshOutput:
        id = type->GetMeshOutput() == SemaMeshOutput::Vertices  ? BuiltinTypeId::Vertices
             : type->GetMeshOutput() == SemaMeshOutput::Indices ? BuiltinTypeId::Indices
                                                                : BuiltinTypeId::Primitives;
        addType(type->GetElement());
        addValue(type->GetCount());
        break;
    default:
        break;
    }
    return static_cast<uint32>(id);
}
} // namespace Marlin

namespace Marlin
{
String Sema::GetQualifiedName(const SemaSymbol *symbol) const
{
    List<StringView> parts;
    for (const SemaSymbol *s = symbol; s != nullptr && s != _global; s = s->Parent)
    {
        parts.Add(s->Name);
    }
    StringBuilder<256> text;
    for (usize i = parts.Count(); i-- > 0;)
    {
        text.Append(parts[i]);
        if (i != 0)
        {
            text.Append('.');
        }
    }
    return text.ToString();
}

} // namespace Marlin

namespace Marlin
{
bool Sema::InferGenerics(SemaSymbol *symbol, AstNode *parameterType, const SemaType *argumentType,
                         List<SemaGenericBinding> &bindings)
{
    if (parameterType == nullptr || argumentType->IsError())
    {
        return true;
    }
    auto bindName = [&](StringView name, const SemaGenericArgument &value) {
        for (SemaGenericBinding &binding : bindings)
        {
            if (binding.Name == name)
            {
                if (binding.Argument.Type == nullptr && binding.Argument.Value == nullptr)
                {
                    binding.Argument = value;
                    return true;
                }
                return binding.Argument == value;
            }
        }
        return true;
    };
    auto isParameter = [&](StringView name) {
        for (const SemaGenericBinding &binding : bindings)
        {
            if (binding.Name == name)
            {
                return true;
            }
        }
        return false;
    };
    auto inferValue = [&](AstNode *node, uint64 value) {
        StringView name;
        if (const auto *named = node->TryAs<AstNamedType>())
        {
            name = named->Qualifier == nullptr && !named->HasGenericArguments ? named->Name : StringView();
        }
        else if (const auto *nameExpr = node->TryAs<AstNameExpr>())
        {
            name = nameExpr->Name;
        }
        if (name.IsEmpty() || !isParameter(name))
        {
            return true;
        }
        return bindName(name, {nullptr, NewScalar(Types.GetInt(32, true), value)});
    };

    if (const auto *nullable = parameterType->TryAs<AstNullableType>())
    {
        const SemaType *inner = argumentType->Is(SemaTypeKind::Nullable) ? argumentType->GetElement() : argumentType;
        return inner->Is(SemaTypeKind::Null) || InferGenerics(symbol, nullable->Element, inner, bindings);
    }
    if (const auto *array = parameterType->TryAs<AstArrayType>())
    {
        if (array->Size == nullptr && argumentType->Is(SemaTypeKind::GcArray))
        {
            return InferGenerics(symbol, array->Element, argumentType->GetElement(), bindings);
        }
        if (!argumentType->Is(SemaTypeKind::Array))
        {
            return true;
        }
        return InferGenerics(symbol, array->Element, argumentType->GetElement(), bindings) &&
               (array->Size == nullptr || inferValue(array->Size, argumentType->GetCount()));
    }
    const auto *named = parameterType->TryAs<AstNamedType>();
    if (named == nullptr)
    {
        return true;
    }
    if (named->Qualifier == nullptr && !named->HasGenericArguments && isParameter(named->Name))
    {
        return bindName(named->Name, {argumentType, nullptr});
    }
    if (!named->HasGenericArguments)
    {
        return true;
    }
    SemaLookup lookup;
    Lookup(ContextOf(symbol), named->Name, lookup);
    SemaSymbol *generic = nullptr;
    for (SemaSymbol *candidate : lookup.Symbols)
    {
        if (generic == nullptr || GenericParametersOf(candidate->Decl).Count() == named->GenericArguments.Count())
        {
            generic = candidate;
        }
    }
    if (generic == nullptr)
    {
        return true;
    }
    // A class, interface or delegate argument may give the generic through a base class or an interface it
    // implements, as `List<int>` gives `IEnumerable<T>`.
    List<SemaGenericArgument> actual;
    if (argumentType->HasInstance())
    {
        SemaStructInstance *found = nullptr;
        SemaStructInstance *instance = argumentType->GetStruct();
        if (instance->Symbol != generic && generic->Kind != SemaSymbolKind::Struct && ResolveHierarchy(instance))
        {
            for (SemaStructInstance *base = instance->Base; base != nullptr && found == nullptr; base = base->Base)
            {
                found = base->Symbol == generic ? base : nullptr;
            }
            for (SemaStructInstance *implemented : instance->Interfaces)
            {
                found = found == nullptr && implemented->Symbol == generic ? implemented : found;
            }
        }
        found = instance->Symbol == generic ? instance : found;
        if (found == nullptr)
        {
            return true;
        }
        actual.AddRange(found->Arguments);
    }
    else if (GetMagicFamily(argumentType, actual) != generic->BuiltinId || generic->BuiltinId == 0)
    {
        return true;
    }
    for (usize i = 0; i < named->GenericArguments.Count() && i < actual.Count(); ++i)
    {
        AstNode *node = named->GenericArguments[i];
        if (actual[i].Type != nullptr)
        {
            if (!InferGenerics(symbol, node, actual[i].Type, bindings))
            {
                return false;
            }
        }
        else if (!inferValue(node, actual[i].Value->Bits))
        {
            return false;
        }
    }
    return true;
}
} // namespace Marlin
