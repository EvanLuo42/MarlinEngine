#include "World/Resources.h"

namespace Marlin
{
void *ResourceStore::Find(const TypeInfo &type) const
{
    const UniquePtr<ReflectedValue> *value = _resources.Find(&type);
    return value != nullptr ? (*value)->Get() : nullptr;
}

void *ResourceStore::GetOrAdd(const TypeInfo &type)
{
    if (void *value = Find(type))
    {
        return value;
    }
    UniquePtr<ReflectedValue> &added = _resources.Add(&type, MakeUnique<ReflectedValue>(type));
    MARLIN_ASSERT(added->IsValid());
    return added->Get();
}

void ResourceStore::Remove(const TypeInfo &type)
{
    (void)_resources.Remove(&type);
}

void EventStore::Update()
{
    const ScopeLock lock(_lock);
    for (auto &[type, stream] : _streams)
    {
        stream->Update();
    }
}
} // namespace Marlin
