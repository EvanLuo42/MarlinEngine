#include "Sema/SemaType.h"

#include "Hash/Hash.h"
#include "Sema/SemaSymbols.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
const SemaType *SemaType::GetScalar() const
{
    if (_kind == SemaTypeKind::Vector || _kind == SemaTypeKind::Matrix)
    {
        return _element;
    }
    return IsScalar() ? this : nullptr;
}

bool SemaType::IsStorable() const
{
    switch (_kind)
    {
    case SemaTypeKind::Bool:
    case SemaTypeKind::Int:
    case SemaTypeKind::Float:
    case SemaTypeKind::Vector:
    case SemaTypeKind::Matrix:
    case SemaTypeKind::Enum:
    case SemaTypeKind::Texture:
    case SemaTypeKind::Buffer:
    case SemaTypeKind::Sampler:
    case SemaTypeKind::AccelerationStructure:
        return true;
    case SemaTypeKind::Array:
        return _element->IsStorable();
    case SemaTypeKind::Struct:
        for (const SemaField &field : _struct->Fields)
        {
            if (!field.Type->IsStorable())
            {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

bool SemaType::IsReference() const
{
    switch (_kind)
    {
    case SemaTypeKind::Class:
    case SemaTypeKind::Interface:
    case SemaTypeKind::Delegate:
    case SemaTypeKind::GcArray:
    case SemaTypeKind::String:
        return true;
    default:
        return false;
    }
}

namespace
{
/// `Name` or `Name<Arguments>`.
void PrintInstance(const SemaStructInstance &instance, StringWriter &output)
{
    output.Append(instance.Symbol->Name);
    if (instance.Arguments.IsEmpty())
    {
        return;
    }
    output.Append('<');
    for (usize i = 0; i < instance.Arguments.Count(); ++i)
    {
        output.Append(i == 0 ? "" : ", ");
        const SemaGenericArgument &argument = instance.Arguments[i];
        if (argument.Type != nullptr)
        {
            argument.Type->Print(output);
        }
        else
        {
            FormatTo(output, "{0}", argument.Value->GetInt());
        }
    }
    output.Append('>');
}

StringView ScalarName(const SemaType &type)
{
    switch (type.GetKind())
    {
    case SemaTypeKind::Bool:
        return "bool";
    case SemaTypeKind::Float:
        return type.GetWidth() == 16 ? "half" : type.GetWidth() == 32 ? "float" : "double";
    case SemaTypeKind::Int:
        switch (type.GetWidth())
        {
        case 16:
            return type.IsSigned() ? "short" : "ushort";
        case 64:
            return type.IsSigned() ? "long" : "ulong";
        default:
            return type.IsSigned() ? "int" : "uint";
        }
    default:
        return "?";
    }
}

StringView TextureName(const SemaType &type)
{
    switch (type.GetTextureDim())
    {
    case MirTextureDim::Texture1D:
        return type.IsArrayed() ? "Texture1DArray" : "Texture1D";
    case MirTextureDim::Texture2D:
        if (type.IsMultisampled())
        {
            return "Texture2DMultisample";
        }
        return type.IsArrayed() ? "Texture2DArray" : "Texture2D";
    case MirTextureDim::Texture3D:
        return "Texture3D";
    case MirTextureDim::Cube:
        return type.IsArrayed() ? "TextureCubeArray" : "TextureCube";
    }
    return "Texture";
}
} // namespace

void SemaType::Print(StringWriter &output) const
{
    switch (_kind)
    {
    case SemaTypeKind::Error:
        output.Append("<error>");
        break;
    case SemaTypeKind::Void:
        output.Append("void");
        break;
    case SemaTypeKind::Null:
        output.Append("null");
        break;
    case SemaTypeKind::Feature:
        output.Append("Feature");
        break;
    case SemaTypeKind::String:
        output.Append("string");
        break;
    case SemaTypeKind::Bool:
    case SemaTypeKind::Int:
    case SemaTypeKind::Float:
        output.Append(ScalarName(*this));
        break;
    case SemaTypeKind::Vector:
        FormatTo(output, "{0}{1}", ScalarName(*_element), _count);
        break;
    case SemaTypeKind::Matrix:
        FormatTo(output, "{0}{1}x{2}", ScalarName(*_element), _rows, _count);
        break;
    case SemaTypeKind::Array:
        _element->Print(output);
        FormatTo(output, "[{0}]", _count);
        break;
    case SemaTypeKind::Struct:
    case SemaTypeKind::Class:
    case SemaTypeKind::Delegate:
        PrintInstance(*_struct, output);
        break;
    case SemaTypeKind::Interface:
        if (_struct != nullptr)
        {
            PrintInstance(*_struct, output);
        }
        else
        {
            output.Append(_symbol->Name);
        }
        break;
    case SemaTypeKind::Enum:
        output.Append(_symbol->Name);
        break;
    case SemaTypeKind::GcArray:
        _element->Print(output);
        output.Append("[]");
        break;
    case SemaTypeKind::Nullable:
        _element->Print(output);
        output.Append('?');
        break;
    case SemaTypeKind::FunctionGroup:
        // RECOVERY-DECISION lang-sema-D1: how a lambda or method group without a delegate type prints.
        output.Append("method group");
        break;
    case SemaTypeKind::Texture:
        output.Append(TextureName(*this)).Append('<');
        _element->Print(output);
        output.Append(_writable ? ", Access.ReadWrite" : "");
        if (_format != MirImageFormat::Unknown)
        {
            FormatTo(output, ", ImageFormat({0})", static_cast<uint32>(_format));
        }
        output.Append('>');
        break;
    case SemaTypeKind::Buffer:
        output.Append("Ptr<");
        if (_element == nullptr)
        {
            output.Append("byte");
        }
        else
        {
            _element->Print(output);
        }
        output.Append(_writable ? ", Access.ReadWrite>" : ">");
        break;
    case SemaTypeKind::Sampler:
        output.Append(_signed ? "ComparisonSampler" : "Sampler");
        break;
    case SemaTypeKind::AccelerationStructure:
        output.Append("AccelerationStructure");
        break;
    case SemaTypeKind::RayQuery:
        output.Append("RayQuery");
        break;
    case SemaTypeKind::HitObject:
        output.Append("HitObject");
        break;
    case SemaTypeKind::CoopVector:
        output.Append("CoopVector<");
        _element->Print(output);
        FormatTo(output, ", {0}>", _count);
        break;
    case SemaTypeKind::CoopMatrix:
        output.Append("CoopMatrix<");
        _element->Print(output);
        FormatTo(output, ", {0}, {1}>", _rows, _count);
        break;
    case SemaTypeKind::MatrixRef:
        output.Append("MatrixRef<");
        _element->Print(output);
        FormatTo(output, ", {0}, {1}>", _rows, _count);
        break;
    case SemaTypeKind::MeshOutput:
        output.Append(_meshOutput == SemaMeshOutput::Vertices  ? "Vertices<"
                      : _meshOutput == SemaMeshOutput::Indices ? "Indices<"
                                                               : "Primitives<");
        _element->Print(output);
        FormatTo(output, ", {0}>", _count);
        break;
    case SemaTypeKind::VectorRef:
        output.Append("VectorRef<");
        _element->Print(output);
        FormatTo(output, ", {0}>", _count);
        break;
    }
}

uint64 SemaTypeTable::Key::GetHash() const
{
    uint64 hash = HashCombine(static_cast<uint64>(Kind), (uint64{Width} << 16) | (uint64{Flags} << 8) | Extra);
    hash = HashCombine(hash, (uint64{Count} << 32) | Rows);
    hash = HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(Element)));
    return HashCombine(hash, HashInteger(reinterpret_cast<uintptr>(Owner)));
}

SemaTypeTable::SemaTypeTable(Arena &arena) : _arena(&arena)
{
    _error = MakeFixed(SemaTypeKind::Error);
    _void = MakeFixed(SemaTypeKind::Void);
    _bool = MakeFixed(SemaTypeKind::Bool);
    _null = MakeFixed(SemaTypeKind::Null);
    _feature = MakeFixed(SemaTypeKind::Feature);
    _string = MakeFixed(SemaTypeKind::String);
    _functionGroup = MakeFixed(SemaTypeKind::FunctionGroup);
}

const SemaType *SemaTypeTable::MakeFixed(SemaTypeKind kind)
{
    SemaType *type = _arena->New<SemaType>();
    type->_kind = kind;
    return type;
}

const SemaType *SemaTypeTable::Intern(const Key &key, const SemaType &prototype)
{
    if (const SemaType *const *found = _types.Find(key))
    {
        return *found;
    }
    const SemaType *type = _arena->New<SemaType>(prototype);
    _types.Add(key, type);
    return type;
}

const SemaType *SemaTypeTable::GetInt(uint32 width, bool isSigned)
{
    SemaType type;
    type._kind = SemaTypeKind::Int;
    type._width = static_cast<uint8>(width);
    type._signed = isSigned;
    return Intern({.Kind = SemaTypeKind::Int, .Width = type._width, .Flags = static_cast<uint8>(isSigned)}, type);
}

const SemaType *SemaTypeTable::GetFloat(uint32 width)
{
    SemaType type;
    type._kind = SemaTypeKind::Float;
    type._width = static_cast<uint8>(width);
    return Intern({.Kind = SemaTypeKind::Float, .Width = type._width}, type);
}

const SemaType *SemaTypeTable::GetVector(const SemaType *scalar, uint32 count)
{
    SemaType type;
    type._kind = SemaTypeKind::Vector;
    type._element = scalar;
    type._count = count;
    return Intern({.Kind = SemaTypeKind::Vector, .Count = count, .Element = scalar}, type);
}

const SemaType *SemaTypeTable::GetMatrix(const SemaType *scalar, uint32 rows, uint32 columns)
{
    SemaType type;
    type._kind = SemaTypeKind::Matrix;
    type._element = scalar;
    type._rows = rows;
    type._count = columns;
    return Intern({.Kind = SemaTypeKind::Matrix, .Count = columns, .Rows = rows, .Element = scalar}, type);
}

const SemaType *SemaTypeTable::GetArray(const SemaType *element, uint32 count)
{
    SemaType type;
    type._kind = SemaTypeKind::Array;
    type._element = element;
    type._count = count;
    return Intern({.Kind = SemaTypeKind::Array, .Count = count, .Element = element}, type);
}

const SemaType *SemaTypeTable::GetStruct(SemaStructInstance *instance)
{
    SemaType type;
    type._kind = SemaTypeKind::Struct;
    type._struct = instance;
    return Intern({.Kind = SemaTypeKind::Struct, .Owner = instance}, type);
}

const SemaType *SemaTypeTable::GetEnum(SemaSymbol *symbol, const SemaType *underlying)
{
    SemaType type;
    type._kind = SemaTypeKind::Enum;
    type._symbol = symbol;
    type._element = underlying;
    return Intern({.Kind = SemaTypeKind::Enum, .Element = underlying, .Owner = symbol}, type);
}

const SemaType *SemaTypeTable::GetInterface(SemaSymbol *symbol)
{
    SemaType type;
    type._kind = SemaTypeKind::Interface;
    type._symbol = symbol;
    return Intern({.Kind = SemaTypeKind::Interface, .Owner = symbol}, type);
}

const SemaType *SemaTypeTable::GetInterface(SemaStructInstance *instance)
{
    SemaType type;
    type._kind = SemaTypeKind::Interface;
    type._struct = instance;
    type._symbol = instance->Symbol;
    return Intern({.Kind = SemaTypeKind::Interface, .Flags = 1, .Owner = instance}, type);
}

const SemaType *SemaTypeTable::GetClass(SemaStructInstance *instance)
{
    SemaType type;
    type._kind = SemaTypeKind::Class;
    type._struct = instance;
    type._symbol = instance->Symbol;
    return Intern({.Kind = SemaTypeKind::Class, .Owner = instance}, type);
}

const SemaType *SemaTypeTable::GetDelegate(SemaStructInstance *instance)
{
    SemaType type;
    type._kind = SemaTypeKind::Delegate;
    type._struct = instance;
    type._symbol = instance->Symbol;
    return Intern({.Kind = SemaTypeKind::Delegate, .Owner = instance}, type);
}

const SemaType *SemaTypeTable::GetGcArray(const SemaType *element)
{
    SemaType type;
    type._kind = SemaTypeKind::GcArray;
    type._element = element;
    return Intern({.Kind = SemaTypeKind::GcArray, .Element = element}, type);
}

const SemaType *SemaTypeTable::GetNullable(const SemaType *element)
{
    if (element->Is(SemaTypeKind::Nullable) || element->Is(SemaTypeKind::Null) || element->IsError())
    {
        return element;
    }
    SemaType type;
    type._kind = SemaTypeKind::Nullable;
    type._element = element;
    return Intern({.Kind = SemaTypeKind::Nullable, .Element = element}, type);
}

const SemaType *SemaTypeTable::GetTexture(const SemaType *texel, MirTextureDim dim, bool arrayed, bool multisampled,

                                          bool writable, MirImageFormat format)
{
    SemaType type;
    type._kind = SemaTypeKind::Texture;
    type._element = texel;
    type._dim = dim;
    type._arrayed = arrayed;
    type._multisampled = multisampled;
    type._writable = writable;
    type._format = format;
    const auto flags = static_cast<uint8>((arrayed ? 1 : 0) | (multisampled ? 2 : 0) | (writable ? 4 : 0) |
                                          (static_cast<uint32>(dim) << 3));
    return Intern(
        {.Kind = SemaTypeKind::Texture, .Flags = flags, .Extra = static_cast<uint8>(format), .Element = texel}, type);
}

const SemaType *SemaTypeTable::GetBuffer(const SemaType *element, bool writable)
{
    SemaType type;
    type._kind = SemaTypeKind::Buffer;
    type._element = element;
    type._writable = writable;
    return Intern({.Kind = SemaTypeKind::Buffer, .Flags = static_cast<uint8>(writable), .Element = element}, type);
}

const SemaType *SemaTypeTable::GetSampler(bool comparison)
{
    SemaType type;
    type._kind = SemaTypeKind::Sampler;
    type._signed = comparison;
    return Intern({.Kind = SemaTypeKind::Sampler, .Flags = static_cast<uint8>(comparison)}, type);
}

const SemaType *SemaTypeTable::GetAccelerationStructure()
{
    SemaType type;
    type._kind = SemaTypeKind::AccelerationStructure;
    return Intern({.Kind = SemaTypeKind::AccelerationStructure}, type);
}

const SemaType *SemaTypeTable::GetRayQuery(uint32 flags)
{
    SemaType type;
    type._kind = SemaTypeKind::RayQuery;
    type._count = flags;
    return Intern({.Kind = SemaTypeKind::RayQuery, .Count = flags}, type);
}

const SemaType *SemaTypeTable::GetHitObject()
{
    SemaType type;
    type._kind = SemaTypeKind::HitObject;
    return Intern({.Kind = SemaTypeKind::HitObject}, type);
}

const SemaType *SemaTypeTable::GetCoopVector(const SemaType *scalar, uint32 count)
{
    SemaType type;
    type._kind = SemaTypeKind::CoopVector;
    type._element = scalar;
    type._count = count;
    return Intern({.Kind = SemaTypeKind::CoopVector, .Count = count, .Element = scalar}, type);
}

const SemaType *SemaTypeTable::GetCoopMatrix(const SemaType *scalar, uint32 rows, uint32 columns, MirMatrixUse use)
{
    SemaType type;
    type._kind = SemaTypeKind::CoopMatrix;
    type._element = scalar;
    type._rows = rows;
    type._count = columns;
    type._matrixUse = use;
    return Intern({.Kind = SemaTypeKind::CoopMatrix,
                   .Flags = static_cast<uint8>(use),
                   .Count = columns,
                   .Rows = rows,
                   .Element = scalar},
                  type);
}

const SemaType *SemaTypeTable::GetMatrixRef(const SemaType *scalar, uint32 rows, uint32 columns,
                                            SemaMatrixLayout layout)
{
    SemaType type;
    type._kind = SemaTypeKind::MatrixRef;
    type._element = scalar;
    type._rows = rows;
    type._count = columns;
    type._layout = layout;
    return Intern({.Kind = SemaTypeKind::MatrixRef,
                   .Flags = static_cast<uint8>(layout),
                   .Count = columns,
                   .Rows = rows,
                   .Element = scalar},
                  type);
}

const SemaType *SemaTypeTable::GetVectorRef(const SemaType *scalar, uint32 count)
{
    SemaType type;
    type._kind = SemaTypeKind::VectorRef;
    type._element = scalar;
    type._count = count;
    return Intern({.Kind = SemaTypeKind::VectorRef, .Count = count, .Element = scalar}, type);
}

const SemaType *SemaTypeTable::GetMeshOutput(SemaMeshOutput kind, const SemaType *element, uint32 count)
{
    SemaType type;
    type._kind = SemaTypeKind::MeshOutput;
    type._meshOutput = kind;
    type._element = element;
    type._count = count;
    return Intern({.Kind = SemaTypeKind::MeshOutput, .Flags = static_cast<uint8>(kind), .Count = count, .Element = element},
                  type);
}

const SemaType *SemaTypeTable::WithScalar(const SemaType *shape, const SemaType *scalar)
{
    switch (shape->GetKind())
    {
    case SemaTypeKind::Vector:
        return GetVector(scalar, shape->GetCount());
    case SemaTypeKind::Matrix:
        return GetMatrix(scalar, shape->GetRows(), shape->GetCount());
    default:
        return scalar;
    }
}
} // namespace Marlin
