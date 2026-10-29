#include "Math/Float16.h"
#include "Sema/Sema.h"

namespace Marlin
{
namespace
{
SemaConversion Worse(SemaConversion a, SemaConversion b)
{
    return a > b ? a : b;
}

/// Rounds *value* to the precision of a float of *width* bits.
double RoundToWidth(double value, uint32 width)
{
    switch (width)
    {
    case 16:
        return Float16(static_cast<float32>(value)).ToFloat();
    case 32:
        return static_cast<float32>(value);
    default:
        return value;
    }
}

/// Truncates *bits* to *width* and extends it back to 64 by the signedness.
uint64 NormalizeInt(uint64 bits, uint32 width, bool isSigned)
{
    if (width >= 64)
    {
        return bits;
    }
    const uint64 mask = (uint64{1} << width) - 1;
    bits &= mask;
    if (isSigned && ((bits >> (width - 1)) & 1) != 0)
    {
        bits |= ~mask;
    }
    return bits;
}
} // namespace

SemaConstant *Sema::NewScalar(const SemaType *type, uint64 bits)
{
    SemaConstant *constant = New<SemaConstant>();
    constant->Type = type;
    const SemaType *integer = type->Is(SemaTypeKind::Enum) ? type->GetElement() : type;
    constant->Bits = integer->Is(SemaTypeKind::Int) ? NormalizeInt(bits, integer->GetWidth(), integer->IsSigned())
                                                    : bits;
    return constant;
}

SemaConstant *Sema::NewFloat(const SemaType *type, double value)
{
    SemaConstant *constant = New<SemaConstant>();
    constant->Type = type;
    constant->Float = RoundToWidth(value, type->GetWidth());
    return constant;
}

SemaConstant *Sema::NewBool(bool value)
{
    return NewScalar(Types.GetBool(), value ? 1 : 0);
}

SemaConstant *Sema::NewString(StringView text, const SemaType *type)
{
    SemaConstant *constant = New<SemaConstant>();
    constant->Type = type != nullptr ? type : Types.GetString();
    char *copy = Memory.AllocateArray<char>(text.Length() + 1);
    MemCopy(copy, text.Data(), text.Length());
    copy[text.Length()] = '\0';
    constant->Text = StringView(copy, text.Length());
    return constant;
}

SemaConstant *Sema::NewZero(const SemaType *type)
{
    SemaConstant *constant = New<SemaConstant>();
    constant->Type = type;
    constant->IsZero = true;
    return constant;
}

SemaConstant *Sema::NewComposite(const SemaType *type, Span<SemaConstant *const> elements)
{
    SemaConstant *constant = New<SemaConstant>();
    constant->Type = type;
    List<SemaConstant *> copy;
    copy.AddRange(elements);
    constant->Elements = Freeze(copy);
    return constant;
}

SemaConversion Sema::ClassifyConversion(const SemaType *from, const SemaType *to, const SemaConstant *constant)
{
    if (from == to || from->IsError() || to->IsError())
    {
        return SemaConversion::Identity;
    }
    // GPU memory (§4.5): writable to read-only Ptr or Span, and a Span to its Ptr; Binder::ConvertPointer makes them.
    const auto narrows = [](const SemaType *source, const SemaType *target) {
        return source->Is(SemaTypeKind::Buffer) && target->Is(SemaTypeKind::Buffer) &&
               source->GetElement() == target->GetElement() && (source == target || !target->IsWritable());
    };
    const auto pointerOf = [&](const SemaType *type) -> const SemaType * {
        return type->Is(SemaTypeKind::Struct) &&
                       type->GetStruct()->Symbol->BuiltinId == static_cast<uint32>(BuiltinTypeId::Span) &&
                       ResolveFields(type->GetStruct())
                   ? type->GetStruct()->Fields[0].Type
                   : nullptr;
    };
    if (narrows(from, to) || (pointerOf(from) != nullptr &&
                              (narrows(pointerOf(from), to) ||
                               (pointerOf(to) != nullptr && narrows(pointerOf(from), pointerOf(to))))))
    {
        return SemaConversion::Widening;
    }
    if (from->Is(SemaTypeKind::Null))
    {
        // `null` converts to the nullable types; to other references only `null!` does (MarlinScript §3.6). In
        // shaders it is the type of `default` before the context gives it one.
        if (to->Is(SemaTypeKind::Nullable))
        {
            return SemaConversion::Widening;
        }
        return to->IsReference() ? SemaConversion::None : SemaConversion::Identity;
    }
    if (to->Is(SemaTypeKind::FunctionGroup) || from->Is(SemaTypeKind::FunctionGroup))
    {
        // Whether a lambda or method group fits a delegate type is the binder's question (FitsDelegate).
        return to->Is(SemaTypeKind::Delegate) ? SemaConversion::Widening : SemaConversion::None;
    }
    if (from->Is(SemaTypeKind::Nullable) || to->Is(SemaTypeKind::Nullable))
    {
        const SemaType *fromInner = from->Is(SemaTypeKind::Nullable) ? from->GetElement() : from;
        const SemaType *toInner = to->Is(SemaTypeKind::Nullable) ? to->GetElement() : to;
        const SemaConversion inner = ClassifyConversion(fromInner, toInner, nullptr);
        if (inner == SemaConversion::None || inner == SemaConversion::Explicit)
        {
            return inner;
        }
        // `T` to `T?` wraps; `T?` to `T` is allowed where the flow analysis proves the value is not null.
        return Worse(inner, SemaConversion::Widening);
    }
    if (from->IsReference() || to->IsReference())
    {
        if (from->IsReference() && to->IsReference())
        {
            if (IsImplicitReference(from, to))
            {
                return SemaConversion::Widening;
            }
            const bool interfaces = from->Is(SemaTypeKind::Interface) || to->Is(SemaTypeKind::Interface);
            return IsImplicitReference(to, from) || interfaces ? SemaConversion::Explicit : SemaConversion::None;
        }
        const bool fromValue = !from->IsReference() && !from->Is(SemaTypeKind::Void);
        const bool toValue = !to->IsReference() && !to->Is(SemaTypeKind::Void);
        const SemaStructInstance *object = GetObjectClass();
        if (fromValue && ((to->Is(SemaTypeKind::Class) && to->GetStruct() == object && object != nullptr) ||
                          (to->Is(SemaTypeKind::Interface) && to->GetStruct() != nullptr &&
                           ImplementsInterface(from, to->GetStruct()))))
        {
            return SemaConversion::Widening;
        }
        if (toValue && ((from->Is(SemaTypeKind::Class) && from->GetStruct() == object && object != nullptr) ||
                        from->Is(SemaTypeKind::Interface)))
        {
            return SemaConversion::Explicit;
        }
        return SemaConversion::None;
    }
    if (from->IsScalar() && to->IsScalar())

    {
        if (from->Is(SemaTypeKind::Bool) || to->Is(SemaTypeKind::Bool))
        {
            return SemaConversion::Explicit;
        }
        if (constant != nullptr && FitsExactly(constant, to))
        {
            const bool widening =
                (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Float)) ||
                (from->GetKind() == to->GetKind() && from->GetWidth() < to->GetWidth() &&
                 (from->IsSigned() == to->IsSigned() || !from->IsSigned()));
            return widening ? SemaConversion::Widening : SemaConversion::ConstantNarrowing;
        }
        if (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Int))
        {
            if (to->GetWidth() > from->GetWidth() && (from->IsSigned() == to->IsSigned() || !from->IsSigned()))
            {
                return SemaConversion::Widening;
            }
            if (to->GetWidth() == from->GetWidth() && from->IsSigned() == to->IsSigned())
            {
                return SemaConversion::Identity;
            }
            return SemaConversion::Explicit;
        }
        if (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Float))
        {
            return SemaConversion::Widening;
        }
        if (from->Is(SemaTypeKind::Float) && to->Is(SemaTypeKind::Float))
        {
            return to->GetWidth() > from->GetWidth() ? SemaConversion::Widening : SemaConversion::Explicit;
        }
        return SemaConversion::Explicit;
    }
    if (from->IsScalar() && (to->Is(SemaTypeKind::Vector) || to->Is(SemaTypeKind::Matrix)))
    {
        const SemaConversion element = ClassifyConversion(from, to->GetScalar(), constant);
        return element >= SemaConversion::Explicit ? element : Worse(element, SemaConversion::Splat);
    }
    if (from->Is(SemaTypeKind::Vector) && to->Is(SemaTypeKind::Vector))
    {
        if (from->GetCount() != to->GetCount())
        {
            return SemaConversion::None;
        }
        return ClassifyConversion(from->GetElement(), to->GetElement(), nullptr);
    }
    if (from->Is(SemaTypeKind::Matrix) && to->Is(SemaTypeKind::Matrix))
    {
        if (from->GetRows() != to->GetRows() || from->GetCount() != to->GetCount())
        {
            return SemaConversion::None;
        }
        return ClassifyConversion(from->GetElement(), to->GetElement(), nullptr);
    }
    if ((from->Is(SemaTypeKind::Enum) && (to->Is(SemaTypeKind::Int) || to->Is(SemaTypeKind::Enum))) ||
        (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Enum)))
    {
        return SemaConversion::Explicit;
    }
    if (from->Is(SemaTypeKind::Texture) && to->Is(SemaTypeKind::Texture) && from->GetElement() == to->GetElement() &&
        from->GetTextureDim() == to->GetTextureDim() && from->IsArrayed() == to->IsArrayed() &&
        from->IsMultisampled() == to->IsMultisampled() && from->IsWritable() == to->IsWritable())
    {
        return SemaConversion::Identity;
    }
    if (from->Is(SemaTypeKind::Array) && to->Is(SemaTypeKind::Array) && from->GetCount() == to->GetCount() &&
        ClassifyConversion(from->GetElement(), to->GetElement(), nullptr) == SemaConversion::Identity)
    {
        return SemaConversion::Identity;
    }
    return SemaConversion::None;
}

bool Sema::FitsExactly(const SemaConstant *value, const SemaType *to)
{
    const SemaType *from = value->Type;
    if (value->IsZero)
    {
        return true;
    }
    if (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Int))
    {
        if (to->GetWidth() >= 64)
        {
            return to->IsSigned() == from->IsSigned() || (from->IsSigned() ? value->GetInt() >= 0
                                                                           : value->Bits <= 0x7FFFFFFFFFFFFFFFull);
        }
        if (to->IsSigned())
        {
            const int64 minimum = -(int64{1} << (to->GetWidth() - 1));
            const int64 maximum = (int64{1} << (to->GetWidth() - 1)) - 1;
            if (!from->IsSigned() && value->Bits > static_cast<uint64>(maximum))
            {
                return false;
            }
            return value->GetInt() >= minimum && value->GetInt() <= maximum;
        }
        if (from->IsSigned() && value->GetInt() < 0)
        {
            return false;
        }
        return value->Bits <= (uint64{1} << to->GetWidth()) - 1;
    }
    if (from->Is(SemaTypeKind::Int) && to->Is(SemaTypeKind::Float))
    {
        const double asFloat =
            from->IsSigned() ? static_cast<double>(value->GetInt()) : static_cast<double>(value->Bits);
        return RoundToWidth(asFloat, to->GetWidth()) == asFloat;
    }
    if (from->Is(SemaTypeKind::Float) && to->Is(SemaTypeKind::Float))
    {
        const double maximum = to->GetWidth() == 16 ? 65504.0 : to->GetWidth() == 32 ? 3.4028234663852886e38 : 1.7976931348623157e308;
        const double magnitude = value->Float < 0 ? -value->Float : value->Float;
        return value->Float != value->Float || magnitude <= maximum || magnitude == __builtin_inf();
    }
    return false;
}

SemaConstant *Sema::ConvertConstant(SemaConstant *value, const SemaType *to)
{
    if (value == nullptr)
    {
        return nullptr;
    }
    const SemaType *from = value->Type;
    if (from == to)
    {
        return value;
    }
    if (value->IsZero)
    {
        return NewZero(to);
    }
    if (from->Is(SemaTypeKind::Enum))
    {
        SemaConstant *underlying = NewScalar(from->GetElement(), value->Bits);
        return ConvertConstant(underlying, to);
    }
    if (to->Is(SemaTypeKind::Enum))
    {
        SemaConstant *underlying = ConvertConstant(value, to->GetElement());
        return underlying != nullptr ? NewScalar(to, underlying->Bits) : nullptr;
    }
    if (from->IsScalar() && to->IsScalar())
    {
        if (to->Is(SemaTypeKind::Bool))
        {
            return NewBool(from->Is(SemaTypeKind::Float) ? value->Float != 0 : value->Bits != 0);
        }
        if (to->Is(SemaTypeKind::Float))
        {
            if (from->Is(SemaTypeKind::Float))
            {
                return NewFloat(to, value->Float);
            }
            const double asFloat = from->Is(SemaTypeKind::Int) && from->IsSigned() ? static_cast<double>(value->GetInt())
                                                                                   : static_cast<double>(value->Bits);
            return NewFloat(to, asFloat);
        }
        if (from->Is(SemaTypeKind::Float))
        {
            const double truncated = value->Float < 0 ? -__builtin_floor(-value->Float) : __builtin_floor(value->Float);
            const uint64 bits = to->IsSigned() ? static_cast<uint64>(static_cast<int64>(truncated))
                                               : static_cast<uint64>(truncated);
            return NewScalar(to, bits);
        }
        return NewScalar(to, value->Bits);
    }
    if (from->IsScalar() && (to->Is(SemaTypeKind::Vector) || to->Is(SemaTypeKind::Matrix)))
    {
        SemaConstant *element = ConvertConstant(value, to->GetScalar());
        if (element == nullptr)
        {
            return nullptr;
        }
        if (to->Is(SemaTypeKind::Vector))
        {
            List<SemaConstant *> elements;
            elements.Resize(to->GetCount(), element);
            return NewComposite(to, elements.AsSpan());
        }
        const SemaType *row = Types.GetVector(to->GetScalar(), to->GetCount());
        List<SemaConstant *> rowElements;
        rowElements.Resize(to->GetCount(), element);
        SemaConstant *rowValue = NewComposite(row, rowElements.AsSpan());
        List<SemaConstant *> rows;
        rows.Resize(to->GetRows(), rowValue);
        return NewComposite(to, rows.AsSpan());
    }
    if ((from->Is(SemaTypeKind::Vector) && to->Is(SemaTypeKind::Vector) && from->GetCount() == to->GetCount()) ||
        (from->Is(SemaTypeKind::Matrix) && to->Is(SemaTypeKind::Matrix) && from->GetRows() == to->GetRows() &&
         from->GetCount() == to->GetCount()))
    {
        const SemaType *element = to->Is(SemaTypeKind::Vector) ? to->GetElement()
                                                               : Types.GetVector(to->GetScalar(), to->GetCount());
        List<SemaConstant *> elements;
        for (SemaConstant *part : value->Elements)
        {
            SemaConstant *converted = ConvertConstant(part, element);
            if (converted == nullptr)
            {
                return nullptr;
            }
            elements.Add(converted);
        }
        return NewComposite(to, elements.AsSpan());
    }
    return nullptr;
}
} // namespace Marlin
