#include "ScriptRegisters.h"

namespace Marlin
{
namespace
{
void SetBit(List<uint8> &bits, uint32 offset)
{
    const uint32 unit = offset / 8;
    if (unit / 8 >= bits.Count())
    {
        bits.Resize(unit / 8 + 1, 0);
    }
    bits[unit / 8] |= static_cast<uint8>(1u << (unit % 8));
}
} // namespace

uint32 ScriptSlotsOf(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Void:
        return 0;
    case MirTypeKind::Bool:
    case MirTypeKind::Int:
    case MirTypeKind::Float:
    case MirTypeKind::Pointer:
    case MirTypeKind::Reference:
        return 1;
    default:
        return type->GetSize() == 0 ? 1 : (type->GetSize() + ScriptSlotBytes - 1) / ScriptSlotBytes;
    }
}

uint32 ScriptReadAccessOf(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Bool:
        return MakeScriptAccess(ScriptAccess::B32);
    case MirTypeKind::Int:
        switch (type->GetWidth())
        {
        case 8:
            return MakeScriptAccess(type->IsSigned() ? ScriptAccess::I8 : ScriptAccess::U8);
        case 16:
            return MakeScriptAccess(type->IsSigned() ? ScriptAccess::I16 : ScriptAccess::U16);
        case 32:
            return MakeScriptAccess(ScriptAccess::B32);
        default:
            return MakeScriptAccess(ScriptAccess::B64);
        }
    case MirTypeKind::Float:
        return MakeScriptAccess(type->GetWidth() == 16   ? ScriptAccess::U16
                                : type->GetWidth() == 32 ? ScriptAccess::B32
                                                         : ScriptAccess::B64);
    case MirTypeKind::Pointer:
    case MirTypeKind::Reference:
        return MakeScriptAccess(ScriptAccess::B64);
    default:
        return MakeScriptAccess(ScriptAccess::Bytes, type->GetSize());
    }
}

uint32 ScriptWriteAccessOf(const MirType *type, bool barrier, uint32 shape)
{
    if (barrier && type->Is(MirTypeKind::Reference))
    {
        return MakeScriptAccess(ScriptAccess::Reference);
    }
    if (barrier && ScriptHasReferences(type))
    {
        return MakeScriptAccess(ScriptAccess::Typed, shape);
    }
    return ScriptReadAccessOf(type);
}

bool ScriptHasReferences(const MirType *type)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Reference:
        return true;
    case MirTypeKind::Array:
        return ScriptHasReferences(type->GetElement());
    case MirTypeKind::Struct:
        for (const MirStructField &field : type->GetFields())
        {
            if (ScriptHasReferences(field.Type))
            {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

ScriptNumber ScriptNumberOf(const MirType *scalar)
{
    if (scalar->Is(MirTypeKind::Bool))
    {
        return ScriptNumber::Bool;
    }
    if (scalar->Is(MirTypeKind::Float))
    {
        return scalar->GetWidth() == 16   ? ScriptNumber::F16
               : scalar->GetWidth() == 32 ? ScriptNumber::F32
                                          : ScriptNumber::F64;
    }
    const bool isSigned = scalar->IsSigned();
    switch (scalar->GetWidth())
    {
    case 8:
        return isSigned ? ScriptNumber::I8 : ScriptNumber::U8;
    case 16:
        return isSigned ? ScriptNumber::I16 : ScriptNumber::U16;
    case 32:
        return isSigned ? ScriptNumber::I32 : ScriptNumber::U32;
    default:
        return isSigned ? ScriptNumber::I64 : ScriptNumber::U64;
    }
}

void MarkScriptReferences(const MirType *type, uint32 offset, List<uint8> &objects, List<uint8> &managed)
{
    switch (type->GetKind())
    {
    case MirTypeKind::Reference:
        SetBit(objects, offset);
        break;
    case MirTypeKind::Pointer:
        if (type->GetAddressSpace() != MirAddressSpace::Static)
        {
            SetBit(managed, offset);
        }
        break;
    case MirTypeKind::Array:
        for (uint32 i = 0; i < type->GetCount(); ++i)
        {
            MarkScriptReferences(type->GetElement(), offset + i * type->GetStride(), objects, managed);
        }
        break;
    case MirTypeKind::Struct:
        for (const MirStructField &field : type->GetFields())
        {
            MarkScriptReferences(field.Type, offset + field.Offset, objects, managed);
        }
        break;
    default:
        break;
    }
}
} // namespace Marlin
