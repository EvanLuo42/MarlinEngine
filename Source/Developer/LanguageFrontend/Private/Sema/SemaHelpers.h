#pragma once

#include "Sema/BoundTree.h"
#include "Sema/Sema.h"
#include "String/StringBuilder.h"

namespace Marlin
{
/// The language name of *type*, for diagnostics.
inline String TypeNameOf(const SemaType *type)
{
    StringBuilder<64> text;
    type->Print(text);
    return text.ToString();
}

template <typename T> T *MakeExpr(Sema &sema, const SemaType *type, SourceRange range)
{
    T *node = sema.New<T>();
    node->Kind = T::StaticKind;
    node->Type = type;
    node->Range = range;
    return node;
}

template <typename T> T *MakeStmt(Sema &sema, SourceRange range)
{
    T *node = sema.New<T>();
    node->Kind = T::StaticKind;
    node->Range = range;
    return node;
}

inline BoundExpr *MakeConstant(Sema &sema, SemaConstant *value, SourceRange range)
{
    auto *constant = MakeExpr<BoundConstant>(sema, value->Type, range);
    constant->Value = value;
    return constant;
}

inline Span<AstGenericParameterDecl *> GenericParametersOf(const AstDecl *decl)
{
    if (decl == nullptr)
    {
        return {};
    }
    if (const auto *structDecl = decl->TryAs<AstStructDecl>())
    {
        return structDecl->GenericParameters;
    }
    if (const auto *interfaceDecl = decl->TryAs<AstInterfaceDecl>())
    {
        return interfaceDecl->GenericParameters;
    }
    if (const auto *function = decl->TryAs<AstFunctionDecl>())
    {
        return function->GenericParameters;
    }
    if (const auto *delegate = decl->TryAs<AstDelegateDecl>())
    {
        return delegate->GenericParameters;
    }
    if (const auto *extension = decl->TryAs<AstExtensionDecl>())
    {
        return extension->GenericParameters;
    }
    return {};
}

inline Span<AstParameterDecl *> ParametersOf(const AstDecl *decl)
{
    if (const auto *function = decl->TryAs<AstFunctionDecl>())
    {
        return function->Parameters;
    }
    if (const auto *constructor = decl->TryAs<AstConstructorDecl>())
    {
        return constructor->Parameters;
    }
    if (const auto *op = decl->TryAs<AstOperatorDecl>())
    {
        return op->Parameters;
    }
    if (const auto *delegate = decl->TryAs<AstDelegateDecl>())
    {
        return delegate->Parameters;
    }
    if (const auto *property = decl->TryAs<AstPropertyDecl>())
    {
        return property->Parameters;
    }
    return {};
}

/// The qualified name of *instance* with its generic arguments, `Marlin.Collections.List<int>`, as script modules
/// name types and functions.
inline void AppendQualifiedName(const Sema &sema, const SemaStructInstance &instance, StringWriter &output)
{
    output.Append(sema.GetQualifiedName(instance.Symbol).AsView());
    if (instance.Arguments.IsEmpty())
    {
        return;
    }
    output.Append('<');
    for (usize i = 0; i < instance.Arguments.Count(); ++i)
    {
        output.Append(i == 0 ? "" : ", ");
        if (instance.Arguments[i].Type != nullptr)
        {
            instance.Arguments[i].Type->Print(output);
        }
        else
        {
            FormatTo(output, "{0}", instance.Arguments[i].Value->GetInt());
        }
    }
    output.Append('>');
}

/// The underlying integer of an enum, or *type* itself.
inline const SemaType *UnderlyingOf(const SemaType *type)
{
    return type->Is(SemaTypeKind::Enum) ? type->GetElement() : type;
}
} // namespace Marlin
