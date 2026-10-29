#include "MarlinText/MarlinTextSchema.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/List.h"
#include "HAL/PlatformSync.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
struct UpgradeStep
{
    uint64 Type;
    uint32 From;
    uint32 To;
    MarlinTextUpgradeFunction Function;
};

struct UpgradeRegistry
{
    Mutex Lock;
    List<UpgradeStep> Steps;

    /// Built on first use and never destroyed.
    static UpgradeRegistry &Get()
    {
        alignas(UpgradeRegistry) static uint8 storage[sizeof(UpgradeRegistry)];
        static UpgradeRegistry *registry = ConstructAt(reinterpret_cast<UpgradeRegistry *>(storage));
        return *registry;
    }
};
} // namespace

namespace MarlinTextSchema
{
void RegisterUpgrade(StringView typeName, uint32 from, uint32 to, MarlinTextUpgradeFunction upgrade)
{
    UpgradeRegistry &registry = UpgradeRegistry::Get();
    const ScopeLock lock(registry.Lock);
    registry.Steps.Add({typeName.GetHash(), from, to, upgrade});
}

bool Upgrade(StringView typeName, MarlinTextObject &object, uint32 from, uint32 to, MarlinTextDiagnostics &diagnostics)
{
    UpgradeRegistry &registry = UpgradeRegistry::Get();
    const uint64 type = typeName.GetHash();
    uint32 version = from;
    while (version < to)
    {
        UpgradeStep best{};
        {
            const ScopeLock lock(registry.Lock);
            for (const UpgradeStep &step : registry.Steps)
            {
                if (step.Type == type && step.From == version && step.To <= to && step.To > best.To)
                {
                    best = step;
                }
            }
        }
        if (best.Function == nullptr)
        {
            diagnostics.Error(object.GetLine(),
                              Format("{0} has no upgrade from schema {1} toward {2}", typeName, version, to));
            return false;
        }
        best.Function(object);
        version = best.To;
    }
    return true;
}
} // namespace MarlinTextSchema
} // namespace Marlin
