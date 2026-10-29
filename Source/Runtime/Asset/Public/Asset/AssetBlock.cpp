#include "Asset/AssetBlock.h"

#include "Asset/Asset.h"
#include "Asset/AssetRef.h"
#include "Containers/HashMap.h"
#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
bool IsTransient(const FieldInfo &field)
{
    return field.GetMetadata().Has("Transient");
}

bool IsFrozen(const TypeInfo &type, const FrozenLayout &layout)
{
    return type.GetExtension() == &layout;
}

bool IsLink(const TypeInfo &type, const AssetLinkKind &kind)
{
    return type.GetKind() == TypeKind::ObjectLink && type.GetExtension() == &kind;
}

usize AlignUp(usize value, usize alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

void AppendNumber(StringWriter &output, uint64 value)
{
    char digits[24];
    output.Append(StringView(digits, FormatUnsigned(value, digits)));
}

void AppendSignature(StringWriter &output, const TypeInfo &type)
{
    output.Append(type.GetFullName()).Append('/');
    AppendNumber(output, static_cast<uint64>(type.GetKind()));
    output.Append('/');
    AppendNumber(output, type.GetSize());
    output.Append('/');
    AppendNumber(output, type.GetAlignment());
    switch (type.GetKind())
    {
    case TypeKind::Struct:
    case TypeKind::Class:
        output.Append('{');
        for (const FieldInfo *field : type.GetFields())
        {
            if (IsTransient(*field))
            {
                continue;
            }
            output.Append(field->GetName()).Append('@');
            AppendNumber(output, field->GetOffset());
            output.Append(':');
            AppendSignature(output, field->GetType());
            output.Append(';');
        }
        output.Append('}');
        break;
    case TypeKind::Enum:
        output.Append('{');
        for (const EnumeratorInfo &enumerator : type.GetEnumerators())
        {
            output.Append(enumerator.Name).Append('=');
            AppendNumber(output, static_cast<uint64>(enumerator.Value));
            output.Append(';');
        }
        output.Append('}');
        break;
    case TypeKind::FixedArray:
        output.Append('[');
        AppendNumber(output, type.GetFixedCount());
        output.Append(']');
        [[fallthrough]];
    case TypeKind::List:
        output.Append('<');
        AppendSignature(output, *type.GetElementType());
        output.Append('>');
        break;
    case TypeKind::ObjectLink:
        output.Append('<').Append(type.GetElementType()->GetFullName()).Append('>');
        break;
    default:
        break;
    }
}

bool CheckValue(const TypeInfo &type, StringView path, StringWriter *problems, HashMap<const TypeInfo *, bool> &seen);

bool Problem(StringWriter *problems, StringView path, const TypeInfo &type, StringView reason)
{
    if (problems != nullptr)
    {
        problems->Append(Format("{0}: {1} {2}\n", path, type.GetName(), reason));
    }
    return false;
}

bool CheckFields(const TypeInfo &type, StringView path, StringWriter *problems, HashMap<const TypeInfo *, bool> &seen)
{
    bool safe = true;
    for (const FieldInfo *field : type.GetFields())
    {
        if (type.GetKind() == TypeKind::Class && IsTransient(*field))
        {
            continue;
        }
        StringBuilder<256> fieldPath(path);
        fieldPath.Append('.').Append(field->GetName());
        if (type.GetKind() == TypeKind::Struct && IsTransient(*field))
        {
            safe = Problem(problems, fieldPath.AsView(), field->GetType(),
                           "is Transient inside a struct, where the image would still copy it") &&
                   safe;
            continue;
        }
        safe = CheckValue(field->GetType(), fieldPath.AsView(), problems, seen) && safe;
    }
    return safe;
}

bool CheckValue(const TypeInfo &type, StringView path, StringWriter *problems, HashMap<const TypeInfo *, bool> &seen)
{
    switch (type.GetKind())
    {
    case TypeKind::Bool:
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Enum:
        return true;
    case TypeKind::Struct: {
        if (const bool *known = seen.Find(&type))
        {
            return *known;
        }
        if (type.GetFields().IsEmpty())
        {
            return Problem(problems, path, type, "has no reflected fields, so its bytes cannot be checked");
        }
        seen.Add(&type, true);
        const bool safe = CheckFields(type, path, problems, seen);
        seen.FindOrAdd(&type) = safe;
        return safe;
    }
    case TypeKind::FixedArray:
        return CheckValue(*type.GetElementType(), path, problems, seen);
    case TypeKind::List:
        if (!IsFrozen(type, Private::FrozenArrayLayout))
        {
            return Problem(problems, path, type, "owns heap memory; use FrozenArray");
        }
        return CheckValue(*type.GetElementType(), path, problems, seen);
    case TypeKind::Opaque:
        if (IsFrozen(type, Private::FrozenStringLayout))
        {
            return true;
        }
        return Problem(problems, path, type, "is opaque");
    case TypeKind::ObjectLink:
        if (IsLink(type, Private::StrongAssetLink) || IsLink(type, Private::SoftAssetLink))
        {
            return true;
        }
        return Problem(problems, path, type, "is not an asset reference");
    case TypeKind::String:
    case TypeKind::StringView:
    case TypeKind::Name:
        return Problem(problems, path, type, "owns or points at memory; use FrozenString");
    default:
        return Problem(problems, path, type, "cannot live in a memory image");
    }
}

class BlockWriter
{
public:
    explicit BlockWriter(List<uint8> &block) : _block(&block)
    {
    }

    bool Write(const Asset &asset, const Guid &guid, uint64 bulkSize)
    {
        const TypeInfo &type = asset.GetType();
        StringBuilder<512> problems;
        if (!AssetBlock::CheckImageSafe(type, &problems))
        {
            MARLIN_LOG(LogAsset, Error, "{0} cannot be written as a block:\n{1}", type.GetName(), problems.AsView());
            return false;
        }
        CollectDependencies(type, &asset);
        _alignment = type.GetAlignment() > 16 ? type.GetAlignment() : 16;

        _block->Clear();
        Reserve(sizeof(AssetBlockHeader), 16);
        const usize dependencies = Reserve(_dependencies.Count() * sizeof(Guid), alignof(Guid));
        for (usize i = 0; i < _dependencies.Count(); ++i)
        {
            MemCopy(_block->Data() + dependencies + i * sizeof(Guid), &_dependencies[i], sizeof(Guid));
        }
        _image = Reserve(type.GetSize(), _alignment);
        _dataStart = _block->Count();
        WriteFields(type, &asset, _image);
        const usize dataEnd = _block->Count();
        const usize fixups = Reserve(_fixups.Count() * sizeof(uint32), alignof(uint32));
        MemCopy(_block->Data() + fixups, _fixups.Data(), _fixups.Count() * sizeof(uint32));
        if (_block->Count() > 0xFFFFFFFFu)
        {
            MARLIN_LOG(LogAsset, Error, "The block of {0} is over 4 GiB; move large data to bulk", guid);
            return false;
        }

        AssetBlockHeader header;
        header.Object = guid;
        header.TypeHash = AssetBlock::GetTypeHash(type);
        header.LayoutHash = AssetBlock::GetLayoutHash(type);
        header.BlockSize = static_cast<uint32>(_block->Count());
        header.Alignment = static_cast<uint32>(_alignment);
        header.DependencyOffset = static_cast<uint32>(dependencies);
        header.DependencyCount = static_cast<uint32>(_dependencies.Count());
        header.ImageOffset = static_cast<uint32>(_image);
        header.ImageSize = type.GetSize();
        header.DataOffset = static_cast<uint32>(_dataStart);
        header.DataSize = static_cast<uint32>(dataEnd - _dataStart);
        header.FixupOffset = static_cast<uint32>(fixups);
        header.FixupCount = static_cast<uint32>(_fixups.Count());
        header.BulkSize = bulkSize;
        MemCopy(_block->Data(), &header, sizeof(header));
        return true;
    }

private:
    usize Reserve(usize size, usize alignment)
    {
        const usize offset = AlignUp(_block->Count(), alignment);
        _block->Resize(offset + size);
        return offset;
    }

    void CollectDependencies(const TypeInfo &type, const void *value)
    {
        switch (type.GetKind())
        {
        case TypeKind::Struct:
        case TypeKind::Class:
            for (const FieldInfo *field : type.GetFields())
            {
                if (!IsTransient(*field))
                {
                    CollectDependencies(field->GetType(), field->GetValue(value));
                }
            }
            break;
        case TypeKind::FixedArray: {
            const TypeInfo &element = *type.GetElementType();
            for (uint32 i = 0; i < type.GetFixedCount(); ++i)
            {
                CollectDependencies(element,
                                    static_cast<const uint8 *>(value) + static_cast<usize>(i) * element.GetSize());
            }
            break;
        }
        case TypeKind::List: {
            const ListOperations &list = type.GetListOperations();
            for (usize i = 0; i < list.Count(value); ++i)
            {
                CollectDependencies(*type.GetElementType(), list.At(Private::RemoveConst(value), i));
            }
            break;
        }
        case TypeKind::ObjectLink:
            if (IsLink(type, Private::StrongAssetLink))
            {
                Guid guid;
                type.GetLinkOperations().GetGuid(value, guid);
                if (guid.IsValid() && !_dependencyIndex.Contains(guid))
                {
                    _dependencyIndex.Add(guid, static_cast<uint32>(_dependencies.Count()));
                    _dependencies.Add(guid);
                }
            }
            break;
        default:
            break;
        }
    }

    void AddFixup(usize offset, AssetFixupKind kind)
    {
        MARLIN_ASSERT((offset & AssetFixupKindMask) == 0);
        _fixups.Add(static_cast<uint32>(offset) | static_cast<uint32>(kind));
    }

    void WriteFields(const TypeInfo &type, const void *value, usize target)
    {
        for (const FieldInfo *field : type.GetFields())
        {
            if (!IsTransient(*field))
            {
                WriteValue(field->GetType(), field->GetValue(value), target + field->GetOffset());
            }
        }
    }

    void WriteStorage(usize target, usize data, usize count)
    {
        const Private::FrozenStorage storage{reinterpret_cast<void *>(data), static_cast<uint32>(count), 0};
        MemCopy(_block->Data() + target, &storage, sizeof(storage));
        if (count != 0)
        {
            AddFixup(target, AssetFixupKind::Pointer);
        }
    }

    void WriteValue(const TypeInfo &type, const void *value, usize target)
    {
        switch (type.GetKind())
        {
        case TypeKind::Struct:
            WriteFields(type, value, target);
            return;
        case TypeKind::FixedArray: {
            const TypeInfo &element = *type.GetElementType();
            for (uint32 i = 0; i < type.GetFixedCount(); ++i)
            {
                const usize offset = static_cast<usize>(i) * element.GetSize();
                WriteValue(element, static_cast<const uint8 *>(value) + offset, target + offset);
            }
            return;
        }
        case TypeKind::List: {
            const ListOperations &list = type.GetListOperations();
            const TypeInfo &element = *type.GetElementType();
            const usize count = list.Count(value);
            if (count == 0)
            {
                WriteStorage(target, 0, 0);
                return;
            }
            const usize alignment = element.GetAlignment() > 8 ? element.GetAlignment() : 8;
            const usize data = Reserve(count * element.GetSize(), alignment);
            for (usize i = 0; i < count; ++i)
            {
                WriteValue(element, list.At(Private::RemoveConst(value), i), data + i * element.GetSize());
            }
            WriteStorage(target, data, count);
            return;
        }
        case TypeKind::Opaque: {
            const StringView text = static_cast<const FrozenString *>(value)->AsView();
            if (text.IsEmpty())
            {
                WriteStorage(target, 0, 0);
                return;
            }
            const usize data = Reserve(text.Length() + 1, 8);
            MemCopy(_block->Data() + data, text.Data(), text.Length());
            WriteStorage(target, data, text.Length());
            return;
        }
        case TypeKind::ObjectLink: {
            Guid guid;
            type.GetLinkOperations().GetGuid(value, guid);
            if (IsLink(type, Private::SoftAssetLink))
            {
                MemCopy(_block->Data() + target, &guid, sizeof(guid));
                return;
            }
            if (guid.IsValid())
            {
                const uint64 index = *_dependencyIndex.Find(guid);
                MemCopy(_block->Data() + target, &index, sizeof(index));
                AddFixup(target, AssetFixupKind::AssetRef);
            }
            return;
        }
        default:
            MemCopy(_block->Data() + target, value, type.GetSize());
            return;
        }
    }

    List<uint8> *_block;
    List<Guid> _dependencies;
    HashMap<Guid, uint32> _dependencyIndex;
    List<uint32> _fixups;
    usize _alignment = 16;
    usize _image = 0;
    usize _dataStart = 0;
};
} // namespace

namespace AssetBlock
{
uint64 GetTypeHash(const TypeInfo &type)
{
    const StringView name = type.GetFullName();
    return Xxh3Hash64(name.Data(), name.Length());
}

uint64 GetLayoutHash(const TypeInfo &type)
{
    StringBuilder<4096> signature;
    AppendSignature(signature, type);
    return Xxh3Hash64(signature.Data(), signature.Length());
}

bool CheckImageSafe(const TypeInfo &type, StringWriter *problems)
{
    HashMap<const TypeInfo *, bool> seen;
    return CheckFields(type, type.GetName(), problems, seen);
}

bool Write(const Asset &asset, const Guid &guid, List<uint8> &block, uint64 bulkSize)
{
    BlockWriter writer(block);
    return writer.Write(asset, guid, bulkSize);
}

bool Validate(Span<const uint8> block, StringWriter &error)
{
    if (block.Count() < sizeof(AssetBlockHeader))
    {
        error.Append("it is smaller than a block header");
        return false;
    }
    AssetBlockHeader header;
    MemCopy(&header, block.Data(), sizeof(header));
    if (header.Magic != AssetBlockMagic)
    {
        error.Append("it is not an asset block");
        return false;
    }
    if (header.Version != AssetBlockVersion)
    {
        error.Append(Format("it is block version {0}, this build reads {1}", header.Version, AssetBlockVersion));
        return false;
    }
    if (header.BlockSize != block.Count())
    {
        error.Append(Format("it is {0} bytes, the header says {1}", block.Count(), header.BlockSize));
        return false;
    }
    const auto inside = [&](uint64 offset, uint64 size) {
        return offset <= block.Count() && size <= block.Count() - offset;
    };
    if (!inside(header.DependencyOffset, uint64{header.DependencyCount} * sizeof(Guid)) ||
        !inside(header.ImageOffset, header.ImageSize) || !inside(header.DataOffset, header.DataSize) ||
        !inside(header.FixupOffset, uint64{header.FixupCount} * sizeof(uint32)))
    {
        error.Append("a part of it lies outside the block");
        return false;
    }
    const uint8 *fixups = block.Data() + header.FixupOffset;
    for (uint32 i = 0; i < header.FixupCount; ++i)
    {
        uint32 fixup = 0;
        MemCopy(&fixup, fixups + i * sizeof(uint32), sizeof(fixup));
        const uint32 offset = fixup & ~AssetFixupKindMask;
        const uint32 kind = fixup & AssetFixupKindMask;
        if (kind > static_cast<uint32>(AssetFixupKind::AssetRef) || !inside(offset, sizeof(void *)) ||
            offset < header.ImageOffset)
        {
            error.Append(Format("fixup {0} is invalid", i));
            return false;
        }
    }
    return true;
}
} // namespace AssetBlock
} // namespace Marlin
