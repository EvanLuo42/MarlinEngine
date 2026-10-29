#include "LanguageIR/MirPrinter.h"
#include "LanguageIR/MirVerifier.h"
#include "Sema/Lowering.h"
#include "Sema/SemaHelpers.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
struct StageAttribute
{
    StringView Name;
    MirStage Stage;
};

constexpr StageAttribute StageAttributes[] = {
    {"Compute", MirStage::Compute},
    {"Amplification", MirStage::Amplification},
    {"Mesh", MirStage::Mesh},
    {"Vertex", MirStage::Vertex},
    {"Pixel", MirStage::Pixel},
    {"RayGeneration", MirStage::RayGeneration},
    {"ClosestHit", MirStage::ClosestHit},
    {"AnyHit", MirStage::AnyHit},
    {"Miss", MirStage::Miss},
    {"Intersection", MirStage::Intersection},
    {"Callable", MirStage::Callable},
};
} // namespace

uint32 Sema::SizeOf(const SemaType *type)
{
    if (!type->IsStorable())
    {
        return 0;
    }
    if (!_layoutModule)
    {
        _layoutModule = MakeUnique<MirModule>();
        _layoutLowering = MakeUnique<Lowering>(*this, *_layoutModule);
    }
    return _layoutLowering->LowerType(type)->GetSize();
}

bool Sema::CheckMeshOutputs(SemaFunction *entry, const AstAttribute &stageAttribute, MirEntryPoint &settings)
{
    const SemaLocal *vertices = nullptr;
    const SemaLocal *indices = nullptr;
    for (const SemaLocal *parameter : entry->Parameters)
    {
        if (!parameter->Type->Is(SemaTypeKind::MeshOutput))
        {
            continue;
        }
        if (parameter->Mode != SemaParameterMode::Out)
        {
            Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, parameter->Range, "mesh outputs are `out` parameters")
                .WithLabel("declared here");
            return false;
        }
        const SemaType *element = parameter->Type->GetElement();
        switch (parameter->Type->GetMeshOutput())
        {
        case SemaMeshOutput::Vertices:
            vertices = parameter;
            [[fallthrough]];
        case SemaMeshOutput::Primitives:
            if (!element->Is(SemaTypeKind::Struct))
            {
                Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, parameter->Range,
                                  "the elements of `{0}` are structs whose fields are the outputs",
                                  TypeNameOf(parameter->Type).AsView())
                    .WithLabel("declared here");
                return false;
            }
            break;
        case SemaMeshOutput::Indices:
        {
            indices = parameter;
            const SemaType *scalar = element->GetScalar();
            const uint32 count = element->Is(SemaTypeKind::Vector) ? element->GetCount() : 1;
            if (scalar == nullptr || !scalar->Is(SemaTypeKind::Int) || scalar->IsSigned() || scalar->GetWidth() != 32 ||
                count > 3)
            {
                Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, parameter->Range,
                                  "indices are `uint` for points, `uint2` for lines and `uint3` for triangles")
                    .WithLabel("declared here");
                return false;
            }
            settings.Topology = count == 1 ? MirMeshTopology::Point
                                : count == 2 ? MirMeshTopology::Line
                                             : MirMeshTopology::Triangle;
            settings.MaxPrimitives = parameter->Type->GetCount();
            break;
        }
        }
    }
    if (vertices == nullptr || indices == nullptr)
    {
        Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, entry->Symbol->Decl->NameRange,
                          "a mesh entry point writes `out Vertices<V, N>` and `out Indices<T, N>`")
            .WithHelp("add the outputs as parameters (§9.2)");
        return false;
    }
    settings.MaxVertices = vertices->Type->GetCount();
    const SemaContext context = ContextOf(entry->Symbol);
    struct Limit
    {
        StringView Name;
        uint32 Value;
    };
    const Limit limits[] = {{"MaxVertices", settings.MaxVertices},
                            {"MaxPrimitives", settings.MaxPrimitives},
                            {"Topology", static_cast<uint32>(settings.Topology)}};
    for (const Limit &limit : limits)
    {
        const SemaConstant *value = EvaluateAttributeArgument(stageAttribute, ~usize{0}, limit.Name, context);
        if (value != nullptr && value->Bits != limit.Value)
        {
            Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, stageAttribute.Range,
                              "`{0}` disagrees with the mesh outputs, which give {1}", limit.Name, limit.Value)
                .WithNote("the sizes come from `Vertices<V, N>` and `Indices<T, N>`, the topology from the index type");
            return false;
        }
    }
    if (settings.MaxVertices > 256 || settings.MaxPrimitives > 256)
    {
        Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, stageAttribute.Range,
                          "a mesh group writes at most 256 vertices and 256 primitives");
        return false;
    }
    return true;
}

void Sema::GetEntryPoints(StringView folder, List<String> &names) const
{
    // Entry points are functions with a stage attribute, declared at namespace level in files under *folder*.
    auto visit = [&](auto &self, Span<AstNode *> members) -> void {
        for (AstNode *member : members)
        {
            if (const auto *ns = member->TryAs<AstNamespaceDecl>())
            {
                self(self, ns->Members);
                continue;
            }
            const auto *function = member->TryAs<AstFunctionDecl>();
            // A generic entry point is a material pass, compiled per material rather than as a global shader (§9.4).
            if (function == nullptr || !function->GenericParameters.IsEmpty())
            {
                continue;
            }
            for (const StageAttribute &stage : StageAttributes)
            {
                if (FindAttribute(function->Attributes, stage.Name) != nullptr)
                {
                    if (const SemaSymbol *symbol = GetSymbolOf(function))
                    {
                        names.Add(GetQualifiedName(symbol));
                    }
                    break;
                }
            }
        }
    };
    for (const AstFile *tree : _trees)
    {
        const StringView path = tree->File->GetPath();
        const usize at = path.IndexOf(folder);
        // The folder may be under any mount root: `/Engine/Language/…`, `/Project/Language/…`.
        if (at != StringView::NotFound && tree->File->GetDialect() == LanguageDialect::Shader)
        {
            visit(visit, tree->Members);
        }
    }
}

bool Sema::CompileEntryPoint(StringView qualifiedName, MirModule &module)
{
    const uint32 errors = Diagnostics.GetErrorCount();
    SemaSymbol *symbol = FindQualified(qualifiedName);
    if (symbol == nullptr || symbol->Kind != SemaSymbolKind::Function)
    {
        Diagnostics.Error(DiagnosticCode::UndefinedName, {}, "cannot find the entry point `{0}`", qualifiedName)
            .WithNote("an entry point is named by its fully qualified function name, such as `Marlin.Passes.Post.Tonemap`");
        return false;
    }
    if (GetMembers(symbol->Parent, symbol->Name).Count() > 1)
    {
        Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, symbol->Decl->NameRange,
                          "the entry point `{0}` is overloaded", qualifiedName)
            .WithHelp("give each entry point its own name");
        return false;
    }
    UseFile(symbol->File);
    const auto *decl = symbol->Decl->As<AstFunctionDecl>();
    if (symbol->File->File->GetDialect() != LanguageDialect::Shader)
    {
        Diagnostics.Error(DiagnosticCode::NotInDialect, decl->NameRange, "entry points are declared in .mshader files");
        return false;
    }
    // A generic entry point is a material pass: it is instantiated with the material the options name (§9.4).
    List<SemaGenericArgument> materialArguments;
    if (!decl->GenericParameters.IsEmpty())
    {
        if (decl->GenericParameters.Count() != 1 || decl->GenericParameters[0]->ValueType != nullptr ||
            decl->Constraints.IsEmpty())
        {
            Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, decl->NameRange,
                              "an entry point is generic only over its material")
                .WithNote("a material pass has one type parameter constrained by a material interface, such as "
                          "`Resolve<TMaterial>() where TMaterial : ISurfaceMaterial` (§9.4)");
            return false;
        }
        if (Options->Material.IsEmpty())
        {
            Diagnostics.Error(DiagnosticCode::InvalidMaterial, decl->NameRange,
                              "`{0}` is a material pass and needs a material", qualifiedName)
                .WithHelp("name the material in LanguageCompileOptions::Material (§9.4)");
            return false;
        }
        const SemaType *material = GetMaterialType(decl->NameRange);
        if (material == nullptr || material->IsError())
        {
            return false;
        }
        materialArguments.Add({material, nullptr});
        const SemaGenericBinding binding{decl->GenericParameters[0]->Name, materialArguments[0]};
        if (!CheckConstraints(symbol, Span<const SemaGenericBinding>(&binding, 1), decl->NameRange))
        {
            return false;
        }
    }

    const AstAttribute *stageAttribute = nullptr;
    MirStage stage = MirStage::Compute;
    for (const StageAttribute &candidate : StageAttributes)
    {
        if (const AstAttribute *attribute = FindAttribute(decl->Attributes, candidate.Name))
        {
            if (stageAttribute != nullptr)
            {
                Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, attribute->Range,
                                  "an entry point has exactly one stage")
                    .WithSecondaryLabel(stageAttribute->Range, "already marked here");
                return false;
            }
            stageAttribute = attribute;
            stage = candidate.Stage;
        }
    }
    if (stageAttribute == nullptr)
    {
        Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, decl->NameRange, "`{0}` is not an entry point", qualifiedName)
            .WithHelp("mark it with a stage, such as `[Compute(64, 1, 1)]` or `[Pixel]` (§9.1)");
        return false;
    }
    switch (stage)
    {
    case MirStage::Compute:
    case MirStage::Vertex:
    case MirStage::Pixel:
    case MirStage::Amplification:
    case MirStage::Mesh:
    case MirStage::RayGeneration:
    case MirStage::ClosestHit:
    case MirStage::AnyHit:
    case MirStage::Miss:
    case MirStage::Intersection:
    case MirStage::Callable:
        break;
    }

    const SemaContext context = ContextOf(symbol);
    MirEntryPoint settings(Memory);
    if (stage == MirStage::Compute || stage == MirStage::Amplification || stage == MirStage::Mesh)
    {
        for (uint32 i = 0; i < 3; ++i)
        {
            SemaConstant *size = i < stageAttribute->Arguments.Count()
                                     ? EvaluateAttributeArgument(*stageAttribute, i, {}, context)
                                     : nullptr;
            settings.GroupSize[i] = size != nullptr ? static_cast<uint32>(size->Bits) : 1;
            if (settings.GroupSize[i] == 0)
            {
                Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, stageAttribute->Range,
                                  "a thread group cannot be empty");
                return false;
            }
        }
        if (settings.GroupSize[0] * settings.GroupSize[1] * settings.GroupSize[2] > 1024)
        {
            Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, stageAttribute->Range,
                              "a thread group has at most 1024 threads");
            return false;
        }
    }

    SemaFunction *entry = InstantiateFunction(symbol, nullptr, materialArguments.AsSpan());

    bool ok = BindBody(entry);
    if (ok && stage == MirStage::Mesh && !CheckMeshOutputs(entry, *stageAttribute, settings))
    {
        return false;
    }

    List<SemaFunction *> reachable;
    HashMap<const SemaFunction *, uint8> colors;
    struct Frame
    {
        SemaFunction *Function;
        usize Next;
    };
    List<Frame> stack;
    stack.Add({entry, 0});
    colors.Add(entry, 1);
    // The custom derivatives of the functions reached are reached too (§9.12), as roots of their own: they call the
    // function they belong to, which is no recursion.
    usize derivativesFrom = 0;
    while (!stack.IsEmpty() || derivativesFrom < reachable.Count())
    {
        if (stack.IsEmpty())
        {
            const SemaFunction *function = reachable[derivativesFrom++];
            for (SemaFunction *derivative :
                 {function->ForwardDerivative, function->ForwardDerivative2, function->BackwardDerivative})
            {
                if (derivative != nullptr && colors.Find(derivative) == nullptr)
                {
                    colors.Add(derivative, 1);
                    ok = BindBody(derivative) && ok;
                    stack.Add({derivative, 0});
                }
            }
            continue;
        }
        Frame &frame = stack.Last();
        SemaFunction *function = frame.Function;
        if (frame.Next < function->Callees.Count())
        {
            const SemaCallSite site = function->Callees[frame.Next++];
            SemaFunction *callee = site.Callee;
            uint8 &color = colors.FindOrAdd(callee);
            if (color == 1)
            {
                Diagnostics.Error(DiagnosticCode::RecursionNotAllowed, site.Range, "`{0}` calls itself through `{1}`",
                                  callee->Symbol->Name, function->Symbol->Name)
                    .WithLabel("recursive call")
                    .WithNote("shaders and the common subset cannot recurse (§3.2); `const` functions may, when "
                              "evaluated at compile time");
                ok = false;
                continue;
            }
            if (color == 0)
            {
                color = 1;
                ok = BindBody(callee) && ok;
                stack.Add({callee, 0});
            }
            continue;
        }
        colors.Set(function, 2);
        reachable.Add(function);
        stack.Pop();
    }
    if (!ok)
    {
        return false;
    }

    const uint32 stageBit = 1u << static_cast<uint32>(stage);
    for (SemaFunction *function : reachable)
    {
        if ((function->AllowedStages & stageBit) != 0)
        {
            continue;
        }
        SourceRange use = function->StageUse;
        for (SemaFunction *caller : reachable)
        {
            for (const SemaCallSite &site : caller->Callees)
            {
                if (site.Callee == function && !use.IsValid())
                {
                    use = site.Range;
                }
            }
        }
        Diagnostics
            .Error(DiagnosticCode::NotInStage, use, "`{0}` cannot be used in a {1} shader", function->Symbol->Name,
                   GetMirStageName(stage))
            .WithLabel("used here")
            .WithSecondaryLabel(decl->NameRange, "in this entry point");
        ok = false;
    }

    // Features (§9.7), from the callees up: a non-public function needs what it uses outside a `static if
    // (Device.Supports(…))` guard, inferred; a public function and the entry point are boundaries whose `[Requires]` must
    // cover it.
    HashMap<const SemaFunction *, List<StringView>> needs;
    for (SemaFunction *function : reachable)
    {
        const bool infers = function != entry && !HasAnyFlags(function->Symbol->Decl->Modifiers, AstModifiers::Public);
        List<StringView> own;
        own.AddRange(function->Requires.AsSpan());
        for (const SemaCallSite &site : function->Callees)
        {
            const List<StringView> *callee = needs.Find(site.Callee);
            for (const StringView feature : callee != nullptr ? callee->AsSpan() : site.Callee->Requires.AsSpan())
            {
                bool guarded = own.Contains(feature);
                for (const StringView guard : site.Guards)
                {
                    guarded = guarded || guard == feature;
                }
                if (guarded)
                {
                    continue;
                }
                if (infers)
                {
                    own.Add(feature);
                    continue;
                }
                Diagnostics
                    .Error(DiagnosticCode::FeatureRequired, site.Range, "`{0}` needs the feature `{1}`",
                           site.Callee->Symbol->Name, feature)
                    .WithLabel("used without checking for the feature")
                    .WithHelp(Format("guard it with `static if (Device.Supports(Features.{0}))` and a fallback, or "
                                     "declare `[Requires(Features.{0})]` on `{1}`: public functions and entry points "
                                     "state the features they need",
                                     feature, function->Symbol->Name)
                                  .AsView());
                ok = false;
            }
        }
        needs.Add(function, Move(own));
    }
    if (!ok || Diagnostics.GetErrorCount() != errors)
    {
        return false;
    }

    Lowering lowering(*this, module);
    if (!lowering.LowerEntryPoint(entry, stage, qualifiedName, settings) || Diagnostics.GetErrorCount() != errors)
    {
        return false;
    }
    List<String> problems;
    if (!VerifyMirModule(module, problems))
    {
        LanguageDiagnostic &error = Diagnostics.Error(DiagnosticCode::InvalidEntryPoint, decl->NameRange,
                                                      "internal compiler error: the lowered MIR is invalid");
        for (const String &problem : problems)
        {
            error.WithNote(problem.AsView());
        }
        return false;
    }
    return true;
}

bool Sema::CheckDeclaredDifferentiable(const SemaType *self, SourceRange range)
{
    const SemaField *blocking = nullptr;
    if (IsDifferentiableStruct(self, blocking))
    {
        return true;
    }
    LanguageDiagnostic &error =
        Diagnostics.Error(DiagnosticCode::NotDifferentiable, range, "`{0}` cannot implement `IDifferentiable`",
                          TypeNameOf(self).AsView());
    if (blocking != nullptr)
    {
        error
            .WithSecondaryLabel(blocking->Symbol->Decl->NameRange,
                                Format("`{0}` has no derivative", TypeNameOf(blocking->Type).AsView()).AsView())
            .WithHelp("mark the field `[NoDiff]` to leave it out of the derivative (§9.12)");
    }
    else
    {
        error.WithNote("a differentiable struct needs a field with a derivative that is not `[NoDiff]`");
    }
    return false;
}

bool Sema::ResolveCustomDerivatives(SemaFunction *function)
{
    const AstDecl *decl = function->Symbol->Decl;
    // Forward, backward, then forward along two directions (DifferentialPair2).
    const AstAttribute *attributes[] = {FindAttribute(decl->Attributes, "ForwardDerivative"),
                                        FindAttribute(decl->Attributes, "BackwardDerivative"),
                                        FindAttribute(decl->Attributes, "ForwardDerivative2")};
    const AstAttribute *firstFound = nullptr;
    for (const AstAttribute *attribute : attributes)
    {
        firstFound = firstFound != nullptr ? firstFound : attribute;
    }
    if (firstFound == nullptr)
    {
        return true;
    }
    const AstAttribute &first = *firstFound;
    if (function->This != nullptr || !function->GenericArguments.IsEmpty() ||
        (function->Owner != nullptr && !function->Owner->Arguments.IsEmpty()) || function->Parameters.Count() > 32)
    {
        Diagnostics.Error(DiagnosticCode::InvalidCustomDerivative, first.Range,
                          "only non-generic static or top-level functions have custom derivatives")
            .WithLabel("on an instance, generic or 32-parameter function");
        return false;
    }
    if (!IsDifferentiable(function->ReturnType))
    {
        Diagnostics
            .Error(DiagnosticCode::InvalidCustomDerivative, first.Range, "`{0}` returns `{1}`, which has no derivative",
                   function->Symbol->Name, TypeNameOf(function->ReturnType).AsView())
            .WithLabel("custom derivative here");
        return false;
    }
    // A DifferentialPair stands for each parameter with a derivative that is passed by value and not [NoDiff].
    uint32 pairs = 0;
    for (usize i = 0; i < function->Parameters.Count(); ++i)
    {
        const SemaLocal *parameter = function->Parameters[i];
        if ((parameter->Mode == SemaParameterMode::Value || parameter->Mode == SemaParameterMode::In) &&
            !IsNoDiff(parameter->Attributes) && IsDifferentiable(parameter->Type))
        {
            pairs |= 1u << i;
        }
    }
    function->DerivativeParameters = pairs;
    List<SemaGenericBinding> bindings;
    GetBindings(function, bindings);
    const SemaContext context = ContextOf(function->Symbol, Freeze(bindings), function->Owner);
    bool ok = true;
    for (uint32 side = 0; side < 3; ++side)
    {
        if (attributes[side] == nullptr)
        {
            continue;
        }
        const AstAttribute &attribute = *attributes[side];
        const bool forward = side != 1;
        const StringView kind = side == 0   ? StringView("ForwardDerivative")
                                : side == 1 ? StringView("BackwardDerivative")
                                            : StringView("ForwardDerivative2");
        const StringView pairName = side == 2 ? StringView("DifferentialPair2") : StringView("DifferentialPair");
        auto isPairOf = [pairName](const SemaType *type, const SemaType *value) {
            if (!type->Is(SemaTypeKind::Struct))
            {
                return false;
            }
            const SemaStructInstance *instance = type->GetStruct();
            return instance->Symbol->IsCore && instance->Symbol->Name == pairName &&
                   instance->Arguments.Count() == 1 && instance->Arguments[0].Type == value;
        };
        const SemaConstant *name =
            attribute.Arguments.Count() == 1 ? EvaluateAttributeArgument(attribute, 0, {}, context) : nullptr;
        if (name == nullptr || !name->Type->Is(SemaTypeKind::String))
        {
            Diagnostics
                .Error(DiagnosticCode::InvalidCustomDerivative, attribute.Range, "`[{0}]` names the function", kind)
                .WithHelp(Format("write `[{0}(nameof(Function))]`", kind).AsView());
            ok = false;
            continue;
        }
        SemaLookup lookup;
        Lookup(context, name->Text, lookup);
        List<SemaSymbol *> candidates;
        for (SemaSymbol *symbol : lookup.Symbols)
        {
            if (symbol->Kind == SemaSymbolKind::Function)
            {
                candidates.Add(symbol);
            }
        }
        if (candidates.Count() != 1)
        {
            Diagnostics
                .Error(DiagnosticCode::InvalidCustomDerivative, attribute.Range, "`{0}` is {1}", name->Text,
                       StringView(candidates.IsEmpty() ? "not a function in scope" : "overloaded"))
                .WithLabel(Format("the {0} of `{1}`", kind, function->Symbol->Name).AsView())
                .WithNote("a custom derivative is one function, found by its name from where the attribute is");
            ok = false;
            continue;
        }
        SemaSymbol *symbol = candidates[0];
        SemaStructInstance *owner = nullptr;
        if (symbol->Parent != nullptr && symbol->Parent->IsAggregate())
        {
            owner = symbol->Parent == function->Symbol->Parent ? function->Owner
                                                                : InstantiateStruct(symbol->Parent, {});
        }
        SemaFunction *derivative = GenericParametersOf(symbol->Decl).IsEmpty()
                                       ? InstantiateFunction(symbol, owner, {})
                                       : nullptr;
        if (derivative == nullptr || !ResolveSignature(derivative))
        {
            ok = false;
            continue;
        }
        // The signature it must have: a pair (by `ref` backward) for each parameter with a derivative, the others
        // as they are, and then the pair of the result forward, or dResult backward.
        StringBuilder<512> expected;
        const String result = Format("{0}<{1}>", pairName, TypeNameOf(function->ReturnType).AsView());
        expected.Append(forward ? result.AsView() : StringView("void")).Append(' ').Append(symbol->Name).Append('(');
        bool matches = derivative->This == nullptr &&
                       derivative->Parameters.Count() == function->Parameters.Count() + (forward ? 0 : 1) &&
                       (forward ? isPairOf(derivative->ReturnType, function->ReturnType)
                                : derivative->ReturnType->Is(SemaTypeKind::Void));
        for (usize i = 0; i < function->Parameters.Count(); ++i)
        {
            const SemaLocal *parameter = function->Parameters[i];
            const bool pair = (pairs & (1u << i)) != 0;
            const SemaParameterMode mode = pair ? (forward ? SemaParameterMode::Value : SemaParameterMode::Ref)
                                                : parameter->Mode;
            const String type = pair ? Format("{0}<{1}>", pairName, TypeNameOf(parameter->Type).AsView())
                                     : TypeNameOf(parameter->Type);
            expected.Append(i == 0 ? StringView() : StringView(", "))
                .Append(mode == SemaParameterMode::Ref   ? StringView("ref ")
                        : mode == SemaParameterMode::Out ? StringView("out ")
                        : mode == SemaParameterMode::In  ? StringView("in ")
                                                         : StringView())
                .Append(type.AsView())
                .Append(' ')
                .Append(parameter->Name);
            if (!matches)
            {
                continue;
            }
            const SemaLocal *given = derivative->Parameters[i];
            const bool sameMode =
                given->Mode == mode || (mode == SemaParameterMode::Value && given->Mode == SemaParameterMode::In);
            matches = sameMode && (pair ? isPairOf(given->Type, parameter->Type) : given->Type == parameter->Type);
        }
        if (!forward)
        {
            expected.Append(function->Parameters.IsEmpty() ? StringView() : StringView(", "))
                .Append(TypeNameOf(function->ReturnType).AsView())
                .Append(" dResult");
            const SemaLocal *dResult = matches ? derivative->Parameters.Last() : nullptr;
            matches = matches && dResult->Type == function->ReturnType &&
                      (dResult->Mode == SemaParameterMode::Value || dResult->Mode == SemaParameterMode::In);
        }
        expected.Append(')');
        if (!matches)
        {
            Diagnostics
                .Error(DiagnosticCode::InvalidCustomDerivative, attribute.Range,
                       "`{0}` does not have the signature of a {1} derivative of `{2}`", symbol->Name,
                       StringView(forward ? "forward" : "backward"), function->Symbol->Name)
                .WithSecondaryLabel(symbol->Decl->NameRange, "declared here")
                .WithNote(Format("expected `{0}`", expected.AsView()).AsView());
            ok = false;
            continue;
        }
        (side == 0 ? function->ForwardDerivative
                   : side == 1 ? function->BackwardDerivative
                               : function->ForwardDerivative2) = derivative;
    }
    return ok;
}
} // namespace Marlin
