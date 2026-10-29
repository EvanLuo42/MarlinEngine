#include "LanguageIR/MirType.h"

#include "Hash/Hash.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
uint32 AlignTo(uint32 value, uint32 alignment)
{
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

StringView AddressSpaceName(MirAddressSpace space)
{
    switch (space)
    {
    case MirAddressSpace::Function:
        return "function";
    case MirAddressSpace::Private:
        return "private";
    case MirAddressSpace::GroupShared:
        return "groupshared";
    case MirAddressSpace::Buffer:
        return "buffer";
    case MirAddressSpace::Uniform:
        return "uniform";
    case MirAddressSpace::Input:
        return "input";
    case MirAddressSpace::Output:
        return "output";
    case MirAddressSpace::RayPayload:
        return "raypayload";
    case MirAddressSpace::IncomingRayPayload:
        return "incomingraypayload";
    case MirAddressSpace::HitAttribute:
        return "hitattribute";
    case MirAddressSpace::CallableData:
        return "callabledata";
    case MirAddressSpace::IncomingCallableData:
        return "incomingcallabledata";
    case MirAddressSpace::ShaderRecord:
        return "shaderrecord";
    case MirAddressSpace::TaskPayload:
        return "taskpayload";
    case MirAddressSpace::Specialization:
        return "specialization";
    case MirAddressSpace::Managed:
        return "managed";
    case MirAddressSpace::Static:
        return "static";
    }
    return "?";
}

StringView TextureDimName(MirTextureDim dim)
{
    switch (dim)
    {
    case MirTextureDim::Texture1D:
        return "1d";
    case MirTextureDim::Texture2D:
        return "2d";
    case MirTextureDim::Texture3D:
        return "3d";
    case MirTextureDim::Cube:
        return "cube";
    }
    return "?";
}

StringView MatrixUseName(MirMatrixUse use)
{
    switch (use)
    {
    case MirMatrixUse::A:
        return "a";
    case MirMatrixUse::B:
        return "b";
    case MirMatrixUse::Accumulator:
        return "acc";
    }
    return "?";
}
} // namespace

const MirType *MirType::GetScalar() const
{
    if (_kind == MirTypeKind::Vector)
    {
        return _element;
    }
    if (_kind == MirTypeKind::Matrix)
    {
        return _element->_element;
    }
    return IsScalar() ? this : nullptr;
}

void MirType::Print(StringWriter &output) const
{
    switch (_kind)
    {
    case MirTypeKind::Void:
        output.Append("void");
        break;
    case MirTypeKind::Bool:
        output.Append("bool");
        break;
    case MirTypeKind::Int:
        FormatTo(output, "{0}{1}", StringView(_signed ? "i" : "u"), _width);
        break;
    case MirTypeKind::Float:
        FormatTo(output, "f{0}", _width);
        break;
    case MirTypeKind::Vector:
        _element->Print(output);
        FormatTo(output, "x{0}", _count);
        break;
    case MirTypeKind::Matrix:
        _element->_element->Print(output);
        FormatTo(output, "x{0}x{1}", _rows, _count);
        break;
    case MirTypeKind::Array:
        output.Append('[');
        _element->Print(output);
        FormatTo(output, "; {0}]", _count);
        break;
    case MirTypeKind::RuntimeArray:
        output.Append('[');
        _element->Print(output);
        output.Append(']');
        break;
    case MirTypeKind::Struct:
        output.Append('%').Append(_name);
        break;
    case MirTypeKind::Pointer:
        output.Append("ptr<").Append(AddressSpaceName(_space)).Append(", ");
        _element->Print(output);
        output.Append('>');
        break;
    case MirTypeKind::Function:
        output.Append("fn(");
        for (usize i = 0; i < _parameters.Count(); ++i)
        {
            if (i != 0)
            {
                output.Append(", ");
            }
            _parameters[i]->Print(output);
        }
        output.Append(") -> ");
        _element->Print(output);
        break;
    case MirTypeKind::Texture:
        output.Append(_writable ? "rwtexture" : "texture").Append(TextureDimName(_dim));
        output.Append(_arrayed ? "array" : "").Append(_multisampled ? "ms" : "").Append('<');
        _element->Print(output);
        if (_format != MirImageFormat::Unknown)
        {
            FormatTo(output, ", format{0}", static_cast<uint32>(_format));
        }
        output.Append('>');
        break;
    case MirTypeKind::Buffer:
        output.Append(_writable ? "rwbuffer" : "buffer");
        if (_element != nullptr)
        {
            output.Append('<');
            _element->Print(output);
            output.Append('>');
        }
        break;
    case MirTypeKind::Sampler:
        output.Append(_signed ? "comparisonsampler" : "sampler");
        break;
    case MirTypeKind::AccelerationStructure:
        output.Append("accelerationstructure");
        break;
    case MirTypeKind::RayQuery:
        FormatTo(output, "rayquery<{0}>", _count);
        break;
    case MirTypeKind::HitObject:
        output.Append("hitobject");
        break;
    case MirTypeKind::CoopVector:
        output.Append("coopvector<");
        _element->Print(output);
        FormatTo(output, ", {0}>", _count);
        break;
    case MirTypeKind::CoopMatrix:
        output.Append("coopmatrix<");
        _element->Print(output);
        FormatTo(output, ", {0}, {1}, {2}>", _rows, _count, MatrixUseName(_matrixUse));
        break;
    case MirTypeKind::Reference:
        output.Append("ref<");
        _element != nullptr ? _element->Print(output) : void(output.Append("object"));
        output.Append('>');
        break;
    }
}

uint64 MirTypeContext::Key::GetHash() const
{
    uint64 hash = HashCombine(static_cast<uint64>(Kind), (uint64{Width} << 16) | (uint64{Flags} << 8) | Extra);
    hash = HashCombine(hash, (uint64{Count} << 32) | Rows);
    hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(Element)));
    for (uint32 i = 0; i < ParameterCount; ++i)
    {
        hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(Parameters[i])));
    }
    return hash;
}

bool MirTypeContext::Key::operator==(const Key &other) const
{
    if (Kind != other.Kind || Width != other.Width || Flags != other.Flags || Extra != other.Extra ||
        Count != other.Count || Rows != other.Rows || Element != other.Element ||
        ParameterCount != other.ParameterCount)
    {
        return false;
    }
    for (uint32 i = 0; i < ParameterCount; ++i)
    {
        if (Parameters[i] != other.Parameters[i])
        {
            return false;
        }
    }
    return true;
}

MirTypeContext::MirTypeContext(Arena &arena) : _arena(&arena)
{
}

const MirType *MirTypeContext::Intern(const Key &key, MirType &prototype)
{
    if (const MirType *const *found = _types.Find(key))
    {
        return *found;
    }
    Key stored = key;
    if (key.ParameterCount != 0)
    {
        const MirType **parameters = _arena->AllocateArray<const MirType *>(key.ParameterCount);
        MemCopy(parameters, key.Parameters, key.ParameterCount * sizeof(const MirType *));
        stored.Parameters = parameters;
        prototype._parameters = Span<const MirType *const>(parameters, key.ParameterCount);
    }
    MirType *type = _arena->New<MirType>(prototype);
    _types.Add(stored, type);
    return type;
}

StringView MirTypeContext::CopyString(StringView text)
{
    char *data = _arena->AllocateArray<char>(text.Length() + 1);
    MemCopy(data, text.Data(), text.Length());
    data[text.Length()] = '\0';
    return {data, text.Length()};
}

const MirType *MirTypeContext::GetVoid()
{
    MirType type;
    return Intern({.Kind = MirTypeKind::Void}, type);
}

const MirType *MirTypeContext::GetBool()
{
    MirType type;
    type._kind = MirTypeKind::Bool;
    type._size = 4;
    type._alignment = 4;
    return Intern({.Kind = MirTypeKind::Bool}, type);
}

const MirType *MirTypeContext::GetInt(uint32 width, bool isSigned)
{
    MARLIN_ASSERT(width == 8 || width == 16 || width == 32 || width == 64);
    MirType type;
    type._kind = MirTypeKind::Int;
    type._width = static_cast<uint8>(width);
    type._signed = isSigned;
    type._size = width / 8;
    type._alignment = width / 8;
    return Intern({.Kind = MirTypeKind::Int, .Width = type._width, .Flags = static_cast<uint8>(isSigned)}, type);
}

const MirType *MirTypeContext::GetFloat(uint32 width)
{
    MARLIN_ASSERT(width == 16 || width == 32 || width == 64);
    MirType type;
    type._kind = MirTypeKind::Float;
    type._width = static_cast<uint8>(width);
    type._size = width / 8;
    type._alignment = width / 8;
    return Intern({.Kind = MirTypeKind::Float, .Width = type._width}, type);
}

const MirType *MirTypeContext::GetVector(const MirType *scalar, uint32 count)
{
    MARLIN_ASSERT(scalar->IsScalar() && count >= 2 && count <= 4);
    MirType type;
    type._kind = MirTypeKind::Vector;
    type._width = scalar->_width;
    type._element = scalar;
    type._count = count;
    type._size = scalar->_size * count;
    type._alignment = scalar->_size * (count == 3 ? 4 : count);
    return Intern({.Kind = MirTypeKind::Vector, .Count = count, .Element = scalar}, type);
}

const MirType *MirTypeContext::GetMatrix(const MirType *scalar, uint32 rows, uint32 columns)
{
    MARLIN_ASSERT(scalar->IsNumeric() && rows >= 2 && rows <= 4);
    const MirType *row = GetVector(scalar, columns);
    MirType type;
    type._kind = MirTypeKind::Matrix;
    type._width = scalar->_width;
    type._element = row;
    type._count = columns;
    type._rows = rows;
    type._stride = AlignTo(row->_size, row->_alignment);
    type._size = type._stride * rows;
    type._alignment = row->_alignment;
    return Intern({.Kind = MirTypeKind::Matrix, .Count = columns, .Rows = rows, .Element = row}, type);
}

const MirType *MirTypeContext::GetArray(const MirType *element, uint32 count)
{
    MirType type;
    type._kind = MirTypeKind::Array;
    type._element = element;
    type._count = count;
    type._stride = AlignTo(element->_size, element->_alignment);
    type._size = type._stride * count;
    type._alignment = element->_alignment;
    return Intern({.Kind = MirTypeKind::Array, .Count = count, .Element = element}, type);
}

const MirType *MirTypeContext::GetRuntimeArray(const MirType *element)
{
    MirType type;
    type._kind = MirTypeKind::RuntimeArray;
    type._element = element;
    type._stride = AlignTo(element->_size, element->_alignment);
    type._alignment = element->_alignment;
    return Intern({.Kind = MirTypeKind::RuntimeArray, .Element = element}, type);
}

const MirType *MirTypeContext::GetPointer(const MirType *pointee, MirAddressSpace space)
{
    MirType type;
    type._kind = MirTypeKind::Pointer;
    type._element = pointee;
    type._space = space;
    return Intern({.Kind = MirTypeKind::Pointer, .Flags = static_cast<uint8>(space), .Element = pointee}, type);
}

const MirType *MirTypeContext::GetReference(const MirType *target)
{
    MirType type;
    type._kind = MirTypeKind::Reference;
    type._element = target;
    // RECOVERY-DECISION lang-ir-D7: a reference is stored as a pointer, 8 bytes aligned to 8.
    type._size = 8;
    type._alignment = 8;
    return Intern({.Kind = MirTypeKind::Reference, .Element = target}, type);
}

const MirType *MirTypeContext::GetFunction(const MirType *result, Span<const MirType *const> parameters)
{
    MirType type;
    type._kind = MirTypeKind::Function;
    type._element = result;
    return Intern({.Kind = MirTypeKind::Function,
                   .Element = result,
                   .Parameters = parameters.Data(),
                   .ParameterCount = static_cast<uint32>(parameters.Count())},
                  type);
}

const MirType *MirTypeContext::GetTexture(const MirType *sampled, MirTextureDim dim, bool arrayed, bool multisampled,
                                          bool writable, MirImageFormat format)
{
    MirType type;
    type._kind = MirTypeKind::Texture;
    type._element = sampled;
    type._dim = dim;
    type._arrayed = arrayed;
    type._multisampled = multisampled;
    type._writable = writable;
    type._format = format;
    type._size = 4;
    type._alignment = 4;
    const auto flags = static_cast<uint8>((arrayed ? 1 : 0) | (multisampled ? 2 : 0) | (writable ? 4 : 0) |
                                          (static_cast<uint32>(dim) << 3));
    return Intern(
        {.Kind = MirTypeKind::Texture, .Flags = flags, .Extra = static_cast<uint8>(format), .Element = sampled}, type);
}

const MirType *MirTypeContext::GetBuffer(const MirType *element, bool writable)
{
    MirType type;
    type._kind = MirTypeKind::Buffer;
    type._element = element;
    type._writable = writable;
    type._bufferKind = element != nullptr ? MirBufferKind::Structured : MirBufferKind::ByteAddress;
    // A buffer value is the 64-bit device address of its first element (MarlinLanguage §4.5, §10).
    type._size = 8;
    type._alignment = 8;
    return Intern({.Kind = MirTypeKind::Buffer, .Flags = static_cast<uint8>(writable), .Element = element}, type);
}

const MirType *MirTypeContext::GetSampler(bool comparison)
{
    MirType type;
    type._kind = MirTypeKind::Sampler;
    type._signed = comparison;
    type._size = 4;
    type._alignment = 4;
    return Intern({.Kind = MirTypeKind::Sampler, .Flags = static_cast<uint8>(comparison)}, type);
}

const MirType *MirTypeContext::GetAccelerationStructure()
{
    MirType type;
    type._kind = MirTypeKind::AccelerationStructure;
    type._size = 4;
    type._alignment = 4;
    return Intern({.Kind = MirTypeKind::AccelerationStructure}, type);
}

const MirType *MirTypeContext::GetRayQuery(uint32 flags)
{
    MirType type;
    type._kind = MirTypeKind::RayQuery;
    type._count = flags;
    return Intern({.Kind = MirTypeKind::RayQuery, .Count = flags}, type);
}

const MirType *MirTypeContext::GetHitObject()
{
    MirType type;
    type._kind = MirTypeKind::HitObject;
    return Intern({.Kind = MirTypeKind::HitObject}, type);
}

const MirType *MirTypeContext::GetCoopVector(const MirType *scalar, uint32 count)
{
    MirType type;
    type._kind = MirTypeKind::CoopVector;
    type._width = scalar->_width;
    type._element = scalar;
    type._count = count;
    return Intern({.Kind = MirTypeKind::CoopVector, .Count = count, .Element = scalar}, type);
}

const MirType *MirTypeContext::GetCoopMatrix(const MirType *scalar, uint32 rows, uint32 columns, MirMatrixUse use)
{
    MirType type;
    type._kind = MirTypeKind::CoopMatrix;
    type._width = scalar->_width;
    type._element = scalar;
    type._rows = rows;
    type._count = columns;
    type._matrixUse = use;
    return Intern({.Kind = MirTypeKind::CoopMatrix,
                   .Flags = static_cast<uint8>(use),
                   .Count = columns,
                   .Rows = rows,
                   .Element = scalar},
                  type);
}

const MirType *MirTypeContext::CreateStruct(StringView name, Span<const MirStructField> fields, uint32 size)
{
    MirStructField *copies = _arena->AllocateArray<MirStructField>(fields.Count());
    uint32 offset = 0;
    uint32 alignment = 1;
    for (usize i = 0; i < fields.Count(); ++i)
    {
        const MirType *fieldType = fields[i].Type;
        const uint32 fieldAlignment = fieldType->_alignment != 0 ? fieldType->_alignment : 1;
        uint32 fieldOffset = AlignTo(offset, fieldAlignment);
        if (fields[i].Offset != AutoOffset)
        {
            if (fields[i].Offset < offset || fields[i].Offset % fieldAlignment != 0)
            {
                return nullptr;
            }
            fieldOffset = fields[i].Offset;
        }
        copies[i] = {CopyString(fields[i].Name), fieldType, fieldOffset, fields[i].NoDiff};
        offset = fieldOffset + fieldType->_size;
        alignment = fieldAlignment > alignment ? fieldAlignment : alignment;
    }
    const uint32 end = AlignTo(offset, alignment);
    if (size != 0 && (size < offset || size % alignment != 0))
    {
        return nullptr;
    }
    MirType *type = _arena->New<MirType>();
    type->_kind = MirTypeKind::Struct;
    type->_name = CopyString(name);
    type->_fields = Span<const MirStructField>(copies, fields.Count());
    type->_size = size != 0 ? size : end;
    type->_alignment = alignment;
    return type;
}
} // namespace Marlin
