#include "Reflection/ExtensionSet.h"

#include "Diagnostics/AssertFormat.h"
#include "Diagnostics/Log.h"
#include "Reflection/Reflection.h"
#include "String/Number.h"

namespace Marlin::Private
{
namespace
{
bool ShouldCreate(const TypeInfo &type, const ExtensionContext &context)
{
    const FunctionInfo *function = type.FindFunction("ShouldCreate");
    if (function == nullptr)
    {
        return true;
    }
    if (!function->IsStatic() || function->GetReturnType() != &TypeOf<bool>() ||
        function->GetParameters().Count() != 1 || &function->GetParameters()[0].Type() != &TypeOf<ExtensionContext>())
    {
        MARLIN_FATALF("{0}::ShouldCreate must be static bool ShouldCreate(const ExtensionContext &)",
                      type.GetFullName());
    }
    bool result = true;
    void *const arguments[] = {const_cast<ExtensionContext *>(&context)};
    function->Invoke(nullptr, arguments, &result);
    return result;
}

int64 PriorityOf(const TypeInfo &type)
{
    int64 priority = 0;
    const Optional<StringView> text = type.GetMetadata().Get("Priority");
    if (text.HasValue() && !TryParse(text.Value().Trim(), priority))
    {
        MARLIN_FATALF("{0} has Priority = {1}, which is not an integer", type.GetFullName(), text.Value());
    }
    return priority;
}
} // namespace

void BuildExtensions(const TypeInfo &point, const ExtensionContext &context, List<ExtensionEntry> &entries)
{
    List<const TypeInfo *> types;
    Reflection::GetDerivedTypes(point, types);
    List<const TypeInfo *> chosen;
    for (const TypeInfo *type : types)
    {
        if (type->GetKind() != TypeKind::Class || type->IsAbstract() || type->GetMetadata().Has("Abstract"))
        {
            continue;
        }
        bool disabled = false;
        for (const String &name : context.Disabled)
        {
            disabled = disabled || name == type->GetName() || name == type->GetFullName();
        }
        if (!disabled && ShouldCreate(*type, context))
        {
            chosen.Add(type);
        }
    }
    struct Ranked
    {
        const TypeInfo *Type;
        int64 Priority;
    };
    List<Ranked> ranked;
    for (const TypeInfo *type : chosen)
    {
        ranked.Add({type, PriorityOf(*type)});
    }
    ranked.Sort([](const Ranked &a, const Ranked &b) {
        return a.Priority != b.Priority ? a.Priority > b.Priority : a.Type->GetName().CompareTo(b.Type->GetName()) < 0;
    });
    for (const Ranked &rank : ranked)
    {
        const TypeInfo *type = rank.Type;
        void *raw = type->New();
        if (raw == nullptr)
        {
            MARLIN_FATALF("Extension {0} of {1} cannot be constructed", type->GetName(), point.GetName());
        }
        ExtensionEntry &entry = entries.AddDefaulted();
        entry.Type = type;
        entry.Raw = raw;
        entry.Object = type->CastTo(raw, point);
    }
    for (ExtensionEntry &entry : entries)
    {
        const Optional<StringView> replaces = entry.Type->GetMetadata().Get("Replaces");
        if (!replaces.HasValue())
        {
            continue;
        }
        for (usize i = 0; i < entries.Count(); ++i)
        {
            if (entries[i].Type->GetName() == replaces.Value() || entries[i].Type->GetFullName() == replaces.Value())
            {
                if (entries[i].Replaced)
                {
                    MARLIN_FATALF("{0} and another extension both replace {1}", entry.Type->GetName(),
                                  replaces.Value());
                }
                entries[i].Replaced = true;
                entry.Replaces = static_cast<int32>(i);
            }
        }
        if (entry.Replaces < 0)
        {
            MARLIN_LOG(LogCore, Warning, "{0} replaces {1}, which is not an enabled {2}", entry.Type->GetName(),
                       replaces.Value(), point.GetName());
        }
    }
}

void DestroyExtensions(List<ExtensionEntry> &entries)
{
    for (ExtensionEntry &entry : entries)
    {
        entry.Type->GetOperations().Delete(entry.Raw);
    }
    entries.Clear();
}
} // namespace Marlin::Private
