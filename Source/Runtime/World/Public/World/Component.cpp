#include "World/Component.h"

#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Diagnostics/AssertFormat.h"
#include "Memory/MemoryOps.h"
#include "World/Entity.h"

namespace Marlin
{
struct ComponentRegistryAccess
{
    static void CollectEntityOffsets(const TypeInfo &type, uint32 base, List<uint32> &offsets)
    {
        for (const FieldInfo *field : type.GetFields())
        {
            const TypeInfo &fieldType = field->GetType();
            const uint32 offset = base + field->GetOffset();
            if (&fieldType == &TypeOf<Entity>())
            {
                offsets.Add(offset);
            }
            else if (fieldType.GetKind() == TypeKind::Struct)
            {
                CollectEntityOffsets(fieldType, offset, offsets);
            }
            else if (fieldType.GetKind() == TypeKind::FixedArray)
            {
                const TypeInfo &element = *fieldType.GetElementType();
                for (uint32 i = 0; i < fieldType.GetFixedCount(); ++i)
                {
                    const uint32 elementOffset = offset + i * element.GetSize();
                    if (&element == &TypeOf<Entity>())
                    {
                        offsets.Add(elementOffset);
                    }
                    else if (element.GetKind() == TypeKind::Struct)
                    {
                        CollectEntityOffsets(element, elementOffset, offsets);
                    }
                }
            }
        }
    }

    static ComponentType *Create(const TypeInfo &type, ComponentId id)
    {
        auto *component = new ComponentType();
        component->_type = &type;
        component->_id = id;
        const Metadata &metadata = type.GetMetadata();
        ComponentFlags flags = ComponentFlags::None;
        flags |= metadata.Has("Tag") ? ComponentFlags::Tag : ComponentFlags::None;
        flags |= metadata.Has("Shared") ? ComponentFlags::Shared : ComponentFlags::None;
        flags |= metadata.Has("Chunk") ? ComponentFlags::Chunk : ComponentFlags::None;
        flags |= metadata.Has("Enableable") ? ComponentFlags::Enableable : ComponentFlags::None;
        component->_flags = flags;
        component->_size = component->IsTag() ? 0 : type.GetSize();
        component->_alignment = type.GetAlignment() > 0 ? type.GetAlignment() : 1;

        // Plain data copies with memcpy: numbers, enums, entities and structs and fixed arrays of them.
        bool plain = type.GetKind() == TypeKind::Struct;
        List<const TypeInfo *> pending;
        pending.Add(&type);
        while (plain && !pending.IsEmpty())
        {
            const TypeInfo *current = pending.Pop();
            for (const FieldInfo *field : current->GetFields())
            {
                const TypeInfo *fieldType = &field->GetType();
                while (fieldType->GetKind() == TypeKind::FixedArray)
                {
                    fieldType = fieldType->GetElementType();
                }
                if (fieldType->GetKind() == TypeKind::Struct)
                {
                    pending.Add(fieldType);
                }
                else if (!fieldType->IsPrimitive() && fieldType->GetKind() != TypeKind::Enum)
                {
                    plain = false;
                }
            }
        }
        component->_triviallyCopyable = plain;
        const TypeOperations &operations = type.GetOperations();
        component->_triviallyDestructible = plain || operations.Destruct == nullptr;
        CollectEntityOffsets(type, 0, component->_entityOffsets);

        MARLIN_ASSERTF(operations.Construct != nullptr && operations.Destruct != nullptr &&
                           (component->_triviallyCopyable ||
                            (operations.MoveConstruct != nullptr && operations.CopyConstruct != nullptr)),
                       "Component {0} must be default-constructible, copyable and movable", type.GetName());
        return component;
    }
};

namespace
{
struct ComponentRegistry
{
    SpinLock Lock;
    HashMap<const TypeInfo *, ComponentId> ByType;
};

ComponentType *GComponents[MaxComponentTypes] = {};
Atomic<uint32> GComponentCount{0};
Atomic<ComponentRegistry *> GRegistry{nullptr};
SpinLock GRegistryCreateLock;

ComponentRegistry &GetRegistry()
{
    ComponentRegistry *registry = GRegistry.Load(MemoryOrder::Acquire);
    if (registry != nullptr)
    {
        return *registry;
    }
    const ScopeLock lock(GRegistryCreateLock);
    registry = GRegistry.Load(MemoryOrder::Acquire);
    if (registry == nullptr)
    {
        registry = new ComponentRegistry();
        GRegistry.Store(registry, MemoryOrder::Release);
    }
    return *registry;
}

const ComponentType &Register(ComponentRegistry &registry, const TypeInfo &type)
{
    const ComponentId id = GComponentCount.Load(MemoryOrder::Relaxed);
    MARLIN_ASSERTF(id < MaxComponentTypes, "More than {0} component types", MaxComponentTypes);
    GComponents[id] = ComponentRegistryAccess::Create(type, id);
    registry.ByType.Add(&type, id);
    GComponentCount.Store(id + 1, MemoryOrder::Release);
    return *GComponents[id];
}
} // namespace

void ComponentType::Construct(void *target, uint32 count) const
{
    if (_size == 0)
    {
        return;
    }
    auto *bytes = static_cast<uint8 *>(target);
    for (uint32 i = 0; i < count; ++i)
    {
        _type->GetOperations().Construct(bytes + static_cast<usize>(i) * _size);
    }
}

void ComponentType::Destruct(void *target, uint32 count) const
{
    if (_size == 0 || _triviallyDestructible)
    {
        return;
    }
    auto *bytes = static_cast<uint8 *>(target);
    for (uint32 i = 0; i < count; ++i)
    {
        _type->GetOperations().Destruct(bytes + static_cast<usize>(i) * _size);
    }
}

void ComponentType::CopyConstruct(void *target, const void *source, uint32 count) const
{
    if (_size == 0)
    {
        return;
    }
    if (_triviallyCopyable)
    {
        MemCopy(target, source, static_cast<usize>(count) * _size);
        return;
    }
    auto *to = static_cast<uint8 *>(target);
    const auto *from = static_cast<const uint8 *>(source);
    for (uint32 i = 0; i < count; ++i)
    {
        const usize offset = static_cast<usize>(i) * _size;
        _type->GetOperations().CopyConstruct(to + offset, from + offset);
    }
}

void ComponentType::Relocate(void *target, void *source, uint32 count) const
{
    if (_size == 0 || target == source)
    {
        return;
    }
    if (_triviallyCopyable)
    {
        MemMove(target, source, static_cast<usize>(count) * _size);
        return;
    }
    auto *to = static_cast<uint8 *>(target);
    auto *from = static_cast<uint8 *>(source);
    const TypeOperations &operations = _type->GetOperations();
    for (uint32 i = 0; i < count; ++i)
    {
        const usize offset = static_cast<usize>(i) * _size;
        operations.MoveConstruct(to + offset, from + offset);
        operations.Destruct(from + offset);
    }
}

void ComponentType::CopyAssign(void *target, const void *source) const
{
    if (_size == 0 || target == source)
    {
        return;
    }
    if (_triviallyCopyable)
    {
        MemCopy(target, source, _size);
        return;
    }
    const TypeOperations &operations = _type->GetOperations();
    if (operations.CopyAssign != nullptr)
    {
        operations.CopyAssign(target, source);
        return;
    }
    operations.Destruct(target);
    operations.CopyConstruct(target, source);
}

namespace Components
{
const ComponentType *Find(const TypeInfo &type)
{
    ComponentRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    if (const ComponentId *id = registry.ByType.Find(&type))
    {
        return GComponents[*id];
    }
    if (!type.GetMetadata().Has("Component"))
    {
        return nullptr;
    }
    return &Register(registry, type);
}

const ComponentType &Get(ComponentId id)
{
    MARLIN_ASSERT(id < GComponentCount.Load(MemoryOrder::Acquire));
    return *GComponents[id];
}

const ComponentType &RegisterDynamic(const TypeInfo &type)
{
    ComponentRegistry &registry = GetRegistry();
    const ScopeLock lock(registry.Lock);
    if (const ComponentId *id = registry.ByType.Find(&type))
    {
        return *GComponents[*id];
    }
    return Register(registry, type);
}

uint32 GetCount()
{
    return GComponentCount.Load(MemoryOrder::Acquire);
}
} // namespace Components
} // namespace Marlin
