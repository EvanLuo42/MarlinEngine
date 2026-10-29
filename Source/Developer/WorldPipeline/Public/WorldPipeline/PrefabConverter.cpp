#include "WorldPipeline/PrefabConverter.h"

#include "MarlinText/MarlinTextDocument.h"
#include "String/StringBuilder.h"
#include "World/EntityManager.h"
#include "World/EntityTemplate.h"
#include "WorldPipeline/PrefabSource.h"

namespace Marlin
{
const TypeInfo &PrefabConverter::GetRuntimeType() const
{
    return TypeOf<EntityTemplate>();
}

bool PrefabConverter::Convert(ConversionContext &context)
{
    // The prefab is built in a scratch manager and captured, so the template holds exactly what instantiating the
    // flattened entities gives.
    PrefabFlattener flattener(context);
    List<PrefabFlatEntity> entities;
    EntityManager manager;
    List<Entity> created;
    if (!flattener.Flatten(context.GetSource(), entities) || !flattener.Instantiate(entities, manager, created))
    {
        return false;
    }
    EntityTemplate result;
    StringBuilder<512> problems;
    if (!result.Capture(manager, created.AsSpan(), &problems))
    {
        context.Error(problems.AsView());
        return false;
    }
    result.Name = FrozenString(context.GetSource().GetName());
    context.SetResult(result);
    return !context.HasFailed();
}
} // namespace Marlin
