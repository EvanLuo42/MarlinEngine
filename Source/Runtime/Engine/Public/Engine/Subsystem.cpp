#include "Engine/Subsystem.h"

#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "Engine/StartupProgress.h"
#include "Reflection/Reflection.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
enum class VisitState : uint8
{
    Unvisited,
    Visiting,
    Done,
};

struct Candidate
{
    const TypeInfo *Type = nullptr;
    List<const TypeInfo *> Dependencies;
    /// OptionalDependencies that are linked: ordered before it when created, not needed.
    List<const TypeInfo *> After;
    VisitState State = VisitState::Unvisited;
    bool Created = false;
};

bool ShouldCreate(const TypeInfo &type, const TypeInfo &contextType, SubsystemContext &context)
{
    const FunctionInfo *function = type.FindFunction("ShouldCreate");
    if (function == nullptr)
    {
        return true;
    }
    if (!function->IsStatic() || function->GetReturnType() != &TypeOf<bool>() ||
        function->GetParameters().Count() != 1 || &function->GetParameters()[0].Type() != &contextType)
    {
        MARLIN_FATALF("{0}::ShouldCreate must be static bool ShouldCreate(const {1} &)", type.GetFullName(),
                      contextType.GetFullName());
    }
    bool result = true;
    void *const arguments[] = {&context};
    function->Invoke(nullptr, arguments, &result);
    return result;
}

void ResolveDependencies(Candidate &candidate)
{
    if (const Optional<StringView> names = candidate.Type->GetMetadata().Get("Dependencies"); names.HasValue())
    {
        for (const StringView part : names.Value().Split(','))
        {
            const StringView name = part.Trim();
            if (name.IsEmpty())
            {
                continue;
            }
            const TypeInfo *dependency = Reflection::FindType(name);
            if (dependency == nullptr || !dependency->IsA(TypeOf<Subsystem>()))
            {
                MARLIN_FATALF("{0} depends on {1}, which is not a subsystem", candidate.Type->GetName(), name);
            }
            candidate.Dependencies.Add(dependency);
        }
    }
    if (const Optional<StringView> names = candidate.Type->GetMetadata().Get("OptionalDependencies");
        names.HasValue())
    {
        for (const StringView part : names.Value().Split(','))
        {
            const StringView name = part.Trim();
            // A type that is not linked is simply absent; one that is linked must still be a subsystem.
            const TypeInfo *dependency = name.IsEmpty() ? nullptr : Reflection::FindType(name);
            if (dependency == nullptr)
            {
                continue;
            }
            if (!dependency->IsA(TypeOf<Subsystem>()))
            {
                MARLIN_FATALF("{0} is ordered after {1}, which is not a subsystem", candidate.Type->GetName(), name);
            }
            candidate.After.Add(dependency);
        }
    }
}
} // namespace

SubsystemCollection::SubsystemCollection(const TypeInfo &scope, SubsystemCollection *outer, const TypeInfo &contextType)
    : _scope(scope), _outer(outer), _contextType(contextType)
{
}

SubsystemCollection::~SubsystemCollection()
{
    Deinitialize();
}

void SubsystemCollection::Plan(SubsystemContext &context, List<const TypeInfo *> &order) const
{
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(_scope, types);
    types.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName().CompareTo(b->GetName()) < 0; });

    List<Candidate> candidates;
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() != TypeKind::Class || type->IsAbstract() || type->GetMetadata().Has("Abstract") ||
            (_filter && !_filter(*type)) || !ShouldCreate(*type, _contextType, context))
        {
            continue;
        }
        Candidate &candidate = candidates.AddDefaulted();
        candidate.Type = type;
        ResolveDependencies(candidate);
    }

    const auto find = [&](const TypeInfo *type) -> Candidate * {
        return candidates.Find([type](const Candidate &candidate) { return candidate.Type == type; });
    };
    List<const TypeInfo *> stack;
    const auto visit = [&](auto &self, Candidate &candidate) -> bool {
        if (candidate.State == VisitState::Done)
        {
            return candidate.Created;
        }
        if (candidate.State == VisitState::Visiting)
        {
            StringBuilder<256> cycle;
            for (const TypeInfo *type : stack)
            {
                cycle.Append(type->GetName()).Append(" -> ");
            }
            MARLIN_FATALF("Subsystem dependency cycle: {0}{1}", cycle.AsView(), candidate.Type->GetName());
        }
        candidate.State = VisitState::Visiting;
        stack.Add(candidate.Type);
        for (const TypeInfo *after : candidate.After)
        {
            if (Candidate *inner = after->IsA(_scope) ? find(after) : nullptr)
            {
                (void)self(self, *inner);
            }
        }
        bool ready = true;
        for (const TypeInfo *dependency : candidate.Dependencies)
        {
            if (dependency->IsA(_scope))
            {
                Candidate *inner = find(dependency);
                if (inner == nullptr || !self(self, *inner))
                {
                    MARLIN_LOG(LogCore, Warning, "{0} is not created: it needs {1}, which is not",
                               candidate.Type->GetName(), dependency->GetName());
                    ready = false;
                }
            }
            else if (_outer == nullptr || _outer->Find(*dependency) == nullptr)
            {
                MARLIN_LOG(LogCore, Warning, "{0} is not created: it needs {1} from an outer scope, which is missing",
                           candidate.Type->GetName(), dependency->GetName());
                ready = false;
            }
        }
        stack.Pop();
        candidate.State = VisitState::Done;
        candidate.Created = ready;
        if (ready)
        {
            order.Add(candidate.Type);
        }
        return candidate.Created;
    };
    for (Candidate &candidate : candidates)
    {
        visit(visit, candidate);
    }
}

void SubsystemCollection::Create(const TypeInfo &type, SubsystemContext &context)
{
    void *object = type.New();
    if (object == nullptr)
    {
        MARLIN_FATALF("Subsystem {0} cannot be constructed", type.GetName());
    }
    auto *subsystem = static_cast<Subsystem *>(type.CastTo(object, TypeOf<Subsystem>()));
    _subsystems.Add(subsystem);
    _types.Add(&type);
    subsystem->Bind(context);
    subsystem->Initialize(context);
}

void SubsystemCollection::Initialize()
{
    MARLIN_ASSERT(&_contextType == &TypeOf<SubsystemContext>());
    SubsystemContext context;
    Initialize(context);
}

void SubsystemCollection::Initialize(SubsystemContext &context)
{
    MARLIN_ASSERT(!_initialized);
    context.Collection = this;
    List<const TypeInfo *> order;
    Plan(context, order);
    _initialized = true;
    StartupProgress::AddSteps(static_cast<uint32>(order.Count()));
    for (const TypeInfo *type : order)
    {
        StartupProgress::BeginStep("Initializing", type->GetName());
        Create(*type, context);
        StartupProgress::EndStep();
    }
}

void SubsystemCollection::Update(SubsystemContext &context)
{
    MARLIN_ASSERT(_initialized);
    context.Collection = this;
    List<const TypeInfo *> order;
    Plan(context, order);
    List<usize> removed;
    for (usize i = _subsystems.Count(); i-- > 0;)
    {
        if (!order.Contains(_types[i]))
        {
            _subsystems[i]->Deinitialize();
            removed.Add(i);
        }
    }
    for (const usize i : removed)
    {
        delete _subsystems[i];
        _subsystems.RemoveAt(i);
        _types.RemoveAt(i);
    }
    for (const TypeInfo *type : order)
    {
        if (!_types.Contains(type))
        {
            Create(*type, context);
        }
    }
}

void SubsystemCollection::Deinitialize()
{
    if (!_initialized)
    {
        return;
    }
    for (usize i = _subsystems.Count(); i-- > 0;)
    {
        _subsystems[i]->Deinitialize();
    }
    for (usize i = _subsystems.Count(); i-- > 0;)
    {
        delete _subsystems[i];
    }
    _subsystems.Clear();
    _types.Clear();
    _initialized = false;
}

Subsystem *SubsystemCollection::Find(const TypeInfo &type) const
{
    for (usize i = 0; i < _types.Count(); ++i)
    {
        if (_types[i]->IsA(type))
        {
            return _subsystems[i];
        }
    }
    return _outer != nullptr ? _outer->Find(type) : nullptr;
}
} // namespace Marlin
