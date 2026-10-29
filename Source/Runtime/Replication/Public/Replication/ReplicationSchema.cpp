#include "Replication/ReplicationSchema.h"

#include "Concurrency/Atomic.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Math/MathUtility.h"
#include "Memory/Memory.h"
#include "Memory/MemoryOps.h"
#include "Reflection/Reflection.h"
#include "String/Number.h"
#include "Templates/Bit.h"
#include "World/Entity.h"

MARLIN_DEFINE_LOG_CATEGORY(LogReplication);

namespace Marlin
{
namespace
{
struct Schema
{
    List<ReplicatedComponent *> Components;
    HashMap<ComponentId, ReplicatedComponent *> ById;
};

Atomic<Schema *> GSchema{nullptr};
SpinLock GSchemaLock;

struct FieldOptions
{
    float64 Quantize = 0;
    bool OwnerOnly = false;
    bool InitialOnly = false;
};

/// Appends the leaves of a value of *type* at *offset*; false if part of it cannot be replicated.
bool Flatten(const TypeInfo &type, uint32 offset, const FieldOptions &options, List<NetLeafField> &fields)
{
    NetLeafField leaf;
    leaf.Offset = offset;
    leaf.Size = type.GetSize();
    leaf.OwnerOnly = options.OwnerOnly;
    leaf.InitialOnly = options.InitialOnly;
    switch (type.GetKind())
    {
    case TypeKind::Bool:
        leaf.Encoding = NetFieldEncoding::Bool;
        break;
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
        leaf.Encoding = NetFieldEncoding::Signed;
        break;
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
        leaf.Encoding = NetFieldEncoding::Unsigned;
        break;
    case TypeKind::Enum:
        leaf.Encoding = type.IsSigned() ? NetFieldEncoding::Signed : NetFieldEncoding::Unsigned;
        break;
    case TypeKind::Float32:
    case TypeKind::Float64:
        leaf.Encoding = options.Quantize > 0                  ? NetFieldEncoding::Quantized
                        : type.GetKind() == TypeKind::Float32 ? NetFieldEncoding::Float32
                                                              : NetFieldEncoding::Float64;
        leaf.Quantize = options.Quantize > 0 ? options.Quantize : 0;
        break;
    case TypeKind::Struct:
        if (&type == &TypeOf<Entity>())
        {
            leaf.Encoding = NetFieldEncoding::Entity;
            break;
        }
        for (const FieldInfo *field : type.GetFields())
        {
            if (!Flatten(field->GetType(), offset + field->GetOffset(), options, fields))
            {
                return false;
            }
        }
        return true;
    case TypeKind::FixedArray:
        for (uint32 i = 0; i < type.GetFixedCount(); ++i)
        {
            const TypeInfo &element = *type.GetElementType();
            if (!Flatten(element, offset + i * element.GetSize(), options, fields))
            {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
    fields.Add(leaf);
    return true;
}

Schema &GetSchema()
{
    if (Schema *schema = GSchema.Load(MemoryOrder::Acquire))
    {
        return *schema;
    }
    const ScopeLock lock(GSchemaLock);
    if (Schema *schema = GSchema.Load(MemoryOrder::Acquire))
    {
        return *schema;
    }
    List<const TypeInfo *> types;
    Reflection::ForEachType([&](const TypeInfo &type) {
        if (type.GetKind() == TypeKind::Struct && type.GetMetadata().Has("Component"))
        {
            types.Add(&type);
        }
    });
    // RECOVERY-DECISION D2: NetIndex is the position in the type-name order, so both ends agree without a handshake
    // as long as they link the same replicated components.
    types.Sort([](const TypeInfo *a, const TypeInfo *b) { return a->GetName() < b->GetName(); });

    auto *schema = new Schema();
    for (const TypeInfo *type : types)
    {
        List<NetLeafField> fields;
        for (const FieldInfo *field : type->GetFields())
        {
            const Metadata &metadata = field->GetMetadata();
            if (!metadata.Has("Replicated"))
            {
                continue;
            }
            FieldOptions options;
            if (const Optional<StringView> quantize = metadata.Get("Quantize"); quantize.HasValue())
            {
                (void)TryParse(quantize.Value(), options.Quantize);
            }
            options.OwnerOnly = metadata.HasFlag("Condition", "OwnerOnly");
            options.InitialOnly = metadata.HasFlag("Condition", "InitialOnly");
            const usize before = fields.Count();
            if (!Flatten(field->GetType(), field->GetOffset(), options, fields))
            {
                MARLIN_LOG(LogReplication, Error,
                           "{0}.{1} cannot be replicated: only numbers, enums, entities and structs and fixed arrays "
                           "of them can",
                           type->GetName(), field->GetName());
                fields.RemoveAt(before, fields.Count() - before);
            }
        }
        const ComponentType *component = fields.IsEmpty() ? nullptr : Components::Find(*type);
        if (component == nullptr)
        {
            continue;
        }
        auto *replicated = new ReplicatedComponent();
        replicated->Component = component->GetId();
        replicated->NetIndex = static_cast<uint32>(schema->Components.Count());
        replicated->Fields = Move(fields);
        const ReflectedValue initial(*type);
        for (const NetLeafField &field : replicated->Fields)
        {
            replicated->Defaults.Add(ReplicationSchema::ReadLeaf(field, initial.Get()));
        }
        schema->Components.Add(replicated);
        schema->ById.Add(replicated->Component, replicated);
    }
    GSchema.Store(schema, MemoryOrder::Release);
    return *schema;
}

/// The little-endian bytes of a *size*-byte value, zero-extended.
uint64 ReadRaw(const uint8 *source, uint32 size)
{
    uint64 value = 0;
    MemCopy(&value, source, size < 8 ? size : 8);
    return value;
}

int64 SignExtend(uint64 value, uint32 size)
{
    const uint32 shift = 64 - size * 8;
    return shift >= 64 ? 0 : static_cast<int64>(value << shift) >> shift;
}
} // namespace

namespace ReplicationSchema
{
Span<const ReplicatedComponent *const> GetAll()
{
    const Schema &schema = GetSchema();
    return {const_cast<const ReplicatedComponent *const *>(schema.Components.Data()), schema.Components.Count()};
}

const ReplicatedComponent *Find(ComponentId component)
{
    ReplicatedComponent *const *found = GetSchema().ById.Find(component);
    return found != nullptr ? *found : nullptr;
}

const ReplicatedComponent *FromNetIndex(uint32 index)
{
    const Schema &schema = GetSchema();
    return index < schema.Components.Count() ? schema.Components[index] : nullptr;
}

uint64 ReadLeaf(const NetLeafField &field, const void *component)
{
    const uint8 *source = static_cast<const uint8 *>(component) + field.Offset;
    const uint64 raw = ReadRaw(source, field.Size);
    switch (field.Encoding)
    {
    case NetFieldEncoding::Bool:
        return raw != 0 ? 1 : 0;
    case NetFieldEncoding::Signed:
        return static_cast<uint64>(SignExtend(raw, field.Size));
    case NetFieldEncoding::Quantized: {
        const float64 value =
            field.Size == 4 ? static_cast<float64>(BitCast<float32>(static_cast<uint32>(raw))) : BitCast<float64>(raw);
        return static_cast<uint64>(static_cast<int64>(Math::Round(value / field.Quantize)));
    }
    case NetFieldEncoding::Unsigned:
    case NetFieldEncoding::Float32:
    case NetFieldEncoding::Float64:
    case NetFieldEncoding::Entity:
        return raw;
    }
    return raw;
}

void WriteLeaf(const NetLeafField &field, void *component, uint64 value)
{
    uint8 *target = static_cast<uint8 *>(component) + field.Offset;
    if (field.Encoding == NetFieldEncoding::Quantized)
    {
        const float64 number = static_cast<float64>(static_cast<int64>(value)) * field.Quantize;
        if (field.Size == 4)
        {
            const float32 narrow = static_cast<float32>(number);
            MemCopy(target, &narrow, sizeof(narrow));
        }
        else
        {
            MemCopy(target, &number, sizeof(number));
        }
        return;
    }
    if (field.Encoding == NetFieldEncoding::Bool)
    {
        value = value != 0 ? 1 : 0;
    }
    MemCopy(target, &value, field.Size < 8 ? field.Size : 8);
}
} // namespace ReplicationSchema
} // namespace Marlin
