#include "Hash/Xxh3.h"
#include "Memory/MemoryOps.h"
#include "ScriptInterpreter.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "String/Utf.h"

namespace Marlin
{
namespace
{
uint8 *BytesOf(ScriptArray *array)
{
    return reinterpret_cast<uint8 *>(array) + array->Type->DataOffset;
}

bool IsString(const ScriptObject *object)
{
    return object != nullptr && object->Type->Kind == ScriptTypeKind::String;
}

/// The value of a box of a scalar, or null when the box holds something else.
const ScriptShape *BoxedScalarShape(const ScriptObject *object)
{
    const ScriptType &type = *object->Type;
    if (type.Kind != ScriptTypeKind::Value || type.Module == nullptr || type.Layout == ~0u)
    {
        return nullptr;
    }
    const ScriptShape &shape = type.Module->GetShape(type.Layout);
    return shape.Kind == ScriptShapeKind::Bool || shape.Kind == ScriptShapeKind::Int ||
                   shape.Kind == ScriptShapeKind::Float
               ? &shape
               : nullptr;
}

/// Writes a boxed scalar as ToString writes it; other boxes write their type's name.
void AppendBoxed(StringWriter &output, const ScriptObject *object)
{
    const ScriptShape *shape = BoxedScalarShape(object);
    const uint8 *data = object->GetData();
    if (shape == nullptr)
    {
        output.Append(object->Type->Name.AsView());
        return;
    }
    char digits[MaxIntegerLength];
    switch (shape->Kind)
    {
    case ScriptShapeKind::Bool: {
        uint32 value;
        MemCopy(&value, data, sizeof(value));
        output.Append(value != 0 ? StringView("True") : StringView("False"));
        return;
    }
    case ScriptShapeKind::Int: {
        uint64 bits = 0;
        MemCopy(&bits, data, shape->Width / 8);
        if (shape->Signed)
        {
            const uint32 unused = 64 - shape->Width;
            const int64 value = static_cast<int64>(bits << unused) >> unused;
            output.Append(StringView(digits, FormatSigned(value, digits)));
        }
        else
        {
            output.Append(StringView(digits, FormatUnsigned(bits, digits)));
        }
        return;
    }
    default:
        if (shape->Width == 64)
        {
            float64 value;
            MemCopy(&value, data, sizeof(value));
            AppendFloat(output, value);
        }
        else if (shape->Width == 32)
        {
            float32 value;
            MemCopy(&value, data, sizeof(value));
            AppendFloat(output, value);
        }
        else
        {
            uint16 bits;
            MemCopy(&bits, data, sizeof(bits));
            AppendFloat(output, static_cast<float32>(BitCast<_Float16>(bits)));
        }
        return;
    }
}

/// The size of the value a box holds.
uint32 BoxedSize(const ScriptObject *object)
{
    const ScriptType &type = *object->Type;
    if (type.Module != nullptr && type.Layout != ~0u)
    {
        return type.Module->GetShape(type.Layout).Size;
    }
    return type.InstanceSize - ScriptObjectHeaderBytes;
}

ScriptArray *ObjectToString(ScriptCallContext &context, ScriptObject *self)
{
    if (IsString(self))
    {
        return static_cast<ScriptArray *>(self);
    }
    if (self->Type->Kind == ScriptTypeKind::Value)
    {
        StringBuilder<64> text;
        AppendBoxed(text, self);
        return context.GetDomain().NewString(text.AsView());
    }
    return context.GetDomain().NewString(self->Type->Name.AsView());
}

bool ObjectEquals(ScriptObject *self, ScriptObject *other)
{
    if (self == other)
    {
        return true;
    }
    if (self == nullptr || other == nullptr || self->Type != other->Type)
    {
        return false;
    }
    if (IsString(self))
    {
        return GetScriptString(static_cast<ScriptArray *>(self)) == GetScriptString(static_cast<ScriptArray *>(other));
    }
    if (self->Type->Kind != ScriptTypeKind::Value)
    {
        return false;
    }
    // RECOVERY-DECISION SB1: boxed floats compare like Equals(this float): NaN equals NaN and +0 equals -0; other
    // boxed values compare their bytes.
    const ScriptShape *shape = BoxedScalarShape(self);
    if (shape != nullptr && shape->Kind == ScriptShapeKind::Float && shape->Width >= 32)
    {
        if (shape->Width == 32)
        {
            float32 a;
            float32 b;
            MemCopy(&a, self->GetData(), 4);
            MemCopy(&b, other->GetData(), 4);
            return a == b || (a != a && b != b);
        }
        float64 a;
        float64 b;
        MemCopy(&a, self->GetData(), 8);
        MemCopy(&b, other->GetData(), 8);
        return a == b || (a != a && b != b);
    }
    return MemCompare(self->GetData(), other->GetData(), BoxedSize(self)) == 0;
}

int32 ObjectGetHashCode(ScriptObject *self)
{
    if (IsString(self))
    {
        const StringView text = GetScriptString(static_cast<ScriptArray *>(self));
        return static_cast<int32>(Xxh3Hash64(text.Data(), text.Length()));
    }
    if (self->Type->Kind == ScriptTypeKind::Value)
    {
        const ScriptShape *shape = BoxedScalarShape(self);
        if (shape != nullptr && shape->Kind == ScriptShapeKind::Float)
        {
            uint8 zero[8] = {};
            const bool isZero = (shape->Width == 32 && *reinterpret_cast<const float32 *>(self->GetData()) == 0.0f) ||
                                (shape->Width == 64 && *reinterpret_cast<const float64 *>(self->GetData()) == 0.0);
            if (isZero)
            {
                return static_cast<int32>(Xxh3Hash64(zero, shape->Width / 8));
            }
        }
        return static_cast<int32>(Xxh3Hash64(self->GetData(), BoxedSize(self)));
    }
    return static_cast<int32>(self->GetHashOrdinal());
}

ScriptObject *ObjectGetType(ScriptCallContext &context, ScriptObject *self)
{
    return context.GetDomain().GetTypeInfo(*self->Type);
}

int32 StringLength(ScriptArray *self)
{
    return static_cast<int32>(self->Length);
}

bool StringEquals(ScriptArray *self, ScriptArray *other)
{
    return other != nullptr && GetScriptString(self) == GetScriptString(other);
}

/// A new string of *length* bytes for the caller to fill from *data*; null when the heap is exhausted.
ScriptArray *NewStringOfLength(ScriptDomain &domain, usize length, uint8 *&data)
{
    ScriptArray *string = domain.NewArray(domain.GetStringType(), static_cast<uint32>(length));
    data = string != nullptr ? reinterpret_cast<uint8 *>(string) + domain.GetStringType().DataOffset : nullptr;
    return string;
}

ScriptArray *StringConcat(ScriptCallContext &context, ScriptArray *left, ScriptArray *right)
{
    // RECOVERY-DECISION SB2: Concat treats null as the empty string, as `+` on strings does in C#.
    if (left == nullptr || left->Length == 0)
    {
        return right != nullptr ? right : context.GetDomain().NewString({});
    }
    if (right == nullptr || right->Length == 0)
    {
        return left;
    }
    // Allocating does not collect, so the operands stay where they are while the result is filled.
    const StringView first = GetScriptString(left);
    const StringView second = GetScriptString(right);
    uint8 *data = nullptr;
    ScriptArray *result = NewStringOfLength(context.GetDomain(), first.Length() + second.Length(), data);
    if (result != nullptr)
    {
        MemCopy(data, first.Data(), first.Length());
        MemCopy(data + first.Length(), second.Data(), second.Length());
    }
    return result;
}

bool IsCharacterStart(StringView text, usize index)
{
    return index >= text.Length() || (static_cast<uint8>(text[index]) & 0xC0) != 0x80;
}

bool IsAsciiSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

ScriptArray *StringSubstring(ScriptCallContext &context, ScriptArray *self, int32 start, int32 length)
{
    const StringView text = GetScriptString(self);
    if (start < 0 || static_cast<usize>(start) > text.Length())
    {
        context.Fault(ScriptFault::IndexOutOfRange, "the start is outside the string");
        return nullptr;
    }
    if (length < 0 || static_cast<usize>(start) + static_cast<usize>(length) > text.Length())
    {
        context.Fault(ScriptFault::IndexOutOfRange, "the substring is outside the string");
        return nullptr;
    }
    // RECOVERY-DECISION SB3: a substring that splits a UTF-8 character is a Panic fault.
    if (!IsCharacterStart(text, static_cast<usize>(start)) ||
        !IsCharacterStart(text, static_cast<usize>(start) + static_cast<usize>(length)))
    {
        context.Fault(ScriptFault::Panic, "the substring splits a UTF-8 character");
        return nullptr;
    }
    if (start == 0 && static_cast<usize>(length) == text.Length())
    {
        return self;
    }
    return context.GetDomain().NewString(text.Substring(static_cast<usize>(start), static_cast<usize>(length)));
}

int32 StringIndexOf(ScriptCallContext &context, ScriptArray *self, StringView value, int32 start)
{
    const StringView text = GetScriptString(self);
    if (start < 0 || static_cast<usize>(start) > text.Length())
    {
        context.Fault(ScriptFault::IndexOutOfRange, "the start is outside the string");
        return 0;
    }
    const usize found = text.IndexOf(value, static_cast<usize>(start));
    return found == StringView::NotFound ? -1 : static_cast<int32>(found);
}

int32 StringLastIndexOf(ScriptArray *self, StringView value)
{
    const StringView text = GetScriptString(self);
    if (value.IsEmpty())
    {
        return static_cast<int32>(text.Length());
    }
    const usize found = text.LastIndexOf(value);
    return found == StringView::NotFound ? -1 : static_cast<int32>(found);
}

int32 StringGetByte(ScriptCallContext &context, ScriptArray *self, int32 index)
{
    if (index < 0 || static_cast<uint32>(index) >= self->Length)
    {
        context.Fault(ScriptFault::IndexOutOfRange, "the index is outside the string");
        return 0;
    }
    return BytesOf(self)[index];
}

ScriptArray *StringReplace(ScriptCallContext &context, ScriptArray *self, StringView old, StringView replacement)
{
    if (old.IsEmpty())
    {
        context.Fault(ScriptFault::Panic, "the text to replace is empty");
        return nullptr;
    }
    const StringView text = GetScriptString(self);
    usize found = text.IndexOf(old);
    if (found == StringView::NotFound)
    {
        return self;
    }
    // Counts the matches, then fills a string of the final length; allocating does not collect, so the views stay
    // valid.
    usize matches = 0;
    for (usize at = found; at != StringView::NotFound; at = text.IndexOf(old, at + old.Length()))
    {
        ++matches;
    }
    uint8 *data = nullptr;
    ScriptArray *result = NewStringOfLength(
        context.GetDomain(), text.Length() - matches * old.Length() + matches * replacement.Length(), data);
    if (result == nullptr)
    {
        return nullptr;
    }
    usize at = 0;
    while (found != StringView::NotFound)
    {
        MemCopy(data, text.Data() + at, found - at);
        data += found - at;
        MemCopy(data, replacement.Data(), replacement.Length());
        data += replacement.Length();
        at = found + old.Length();
        found = text.IndexOf(old, at);
    }
    MemCopy(data, text.Data() + at, text.Length() - at);
    return result;
}

template <bool TStart, bool TEnd> ScriptArray *StringTrim(ScriptCallContext &context, ScriptArray *self)
{
    const StringView text = GetScriptString(self);
    usize begin = 0;
    usize end = text.Length();
    if constexpr (TStart)
    {
        while (begin < end && IsAsciiSpace(text[begin]))
        {
            ++begin;
        }
    }
    if constexpr (TEnd)
    {
        while (end > begin && IsAsciiSpace(text[end - 1]))
        {
            --end;
        }
    }
    if (begin == 0 && end == text.Length())
    {
        return self;
    }
    return context.GetDomain().NewString(text.Substring(begin, end - begin));
}

template <bool TUpper> ScriptArray *StringChangeAsciiCase(ScriptCallContext &context, ScriptArray *self)
{
    const StringView text = GetScriptString(self);
    bool changes = false;
    for (const char c : text)
    {
        changes = changes || (TUpper ? (c >= 'a' && c <= 'z') : (c >= 'A' && c <= 'Z'));
    }
    if (!changes)
    {
        return self;
    }
    String result(text);
    for (usize i = 0; i < result.Length(); ++i)
    {
        char &c = result.Data()[i];
        if (TUpper && c >= 'a' && c <= 'z')
        {
            c = static_cast<char>(c - 'a' + 'A');
        }
        else if (!TUpper && c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return context.GetDomain().NewString(result.AsView());
}

void WeakRefNew(ScriptCallContext &context, ScriptObject *self, ScriptObject *target)
{
    context.GetDomain().RegisterWeakRef(self, target);
}

void WeakRefSetTarget(ScriptCallContext &context, ScriptObject *self, ScriptObject *target)
{
    context.GetDomain().SetWeakTarget(self, target);
}

ScriptObject *WeakRefGetTarget(ScriptCallContext &context, ScriptObject *self)
{
    return context.GetDomain().GetWeakTarget(self);
}

uint64 RandomEntropy(ScriptCallContext &context)
{
    return context.GetDomain().TakeEntropy();
}

template <typename T> bool NumberTryParse(StringView text, T *value)
{
    T parsed{};
    bool ok = false;
    if constexpr (IsFloatingPoint<T>)
    {
        // RECOVERY-DECISION SB4: "inf", "infinity" and "nan" in any case, with an optional sign before the first
        // two, are handled here; everything else is Marlin's TryParse, which rounds correctly.
        StringView body = text;
        const bool negative = body.StartsWith('-');
        if (negative || body.StartsWith('+'))
        {
            body = body.RemoveStart(1);
        }
        if (body.EqualsIgnoreCase("inf") || body.EqualsIgnoreCase("infinity"))
        {
            parsed = negative ? -__builtin_inf() : __builtin_inf();
            ok = true;
        }
        else if (text.EqualsIgnoreCase("nan"))
        {
            parsed = static_cast<T>(__builtin_nan(""));
            ok = true;
        }
        else
        {
            ok = TryParse(text, parsed);
        }
    }
    else
    {
        ok = TryParse(text, parsed);
    }
    *value = ok ? parsed : T{};
    return ok;
}

bool StringContains(ScriptArray *self, StringView value)
{
    return GetScriptString(self).Contains(value);
}

bool StringStartsWith(ScriptArray *self, StringView value)
{
    return GetScriptString(self).StartsWith(value);
}

bool StringEndsWith(ScriptArray *self, StringView value)
{
    return GetScriptString(self).EndsWith(value);
}

int32 StringCompareTo(ScriptCallContext &context, ScriptArray *self, ScriptArray *other)
{
    if (other == nullptr)
    {
        context.Fault(ScriptFault::NullReference, "the other string is null");
        return 0;
    }
    const StringView a = GetScriptString(self);
    const StringView b = GetScriptString(other);
    const usize common = a.Length() < b.Length() ? a.Length() : b.Length();
    const int32 compared = MemCompare(a.Data(), b.Data(), common);
    if (compared != 0)
    {
        return compared < 0 ? -1 : 1;
    }
    return a.Length() < b.Length() ? -1 : a.Length() > b.Length() ? 1 : 0;
}

void BuilderAppend(ScriptCallContext &context, ScriptObject *self, StringView text)
{
    if (text.IsEmpty())
    {
        return;
    }
    ScriptDomain &domain = context.GetDomain();
    ScriptStringBuilderState &state = StateOf<ScriptStringBuilderState>(self);
    const usize needed = usize{state.Length} + text.Length();
    const uint32 capacity = state.Buffer != nullptr ? state.Buffer->Length : 0;
    if (needed > capacity)
    {
        if (needed > 0x7FFFFFFF)
        {
            context.Fault(ScriptFault::Overflow, "arithmetic overflow");
            return;
        }
        usize grown = capacity < 16 ? 16 : usize{capacity} * 2;
        grown = grown < needed ? needed : grown;
        grown = grown > 0x7FFFFFFF ? 0x7FFFFFFF : grown;
        ScriptArray *buffer = domain.NewArray(domain.GetByteArrayType(), static_cast<uint32>(grown));
        if (buffer == nullptr)
        {
            context.Fault(ScriptFault::Native, "the script heap is exhausted");
            return;
        }
        // Allocation does not collect, so self and the old buffer stay where they are.
        ScriptStringBuilderState &current = StateOf<ScriptStringBuilderState>(self);
        if (current.Buffer != nullptr)
        {
            MemCopy(BytesOf(buffer), BytesOf(current.Buffer), current.Length);
        }
        domain.WriteReference(reinterpret_cast<ScriptObject **>(&current.Buffer), buffer);
    }
    ScriptStringBuilderState &current = StateOf<ScriptStringBuilderState>(self);
    MemCopy(BytesOf(current.Buffer) + current.Length, text.Data(), text.Length());
    current.Length += static_cast<uint32>(text.Length());
}

void BuilderNew(ScriptCallContext &context, ScriptObject *self)
{
    ScriptStringBuilderState &state = StateOf<ScriptStringBuilderState>(self);
    context.GetDomain().WriteReference(reinterpret_cast<ScriptObject **>(&state.Buffer), nullptr);
    state.Length = 0;
}

void BuilderAppendString(ScriptCallContext &context, ScriptObject *self, StringView value)
{
    BuilderAppend(context, self, value);
}

template <typename T> void BuilderAppendInteger(ScriptCallContext &context, ScriptObject *self, T value)
{
    char digits[MaxIntegerLength];
    const usize length = IsSigned<T> ? FormatSigned(static_cast<int64>(value), digits)
                                     : FormatUnsigned(static_cast<uint64>(value), digits);
    BuilderAppend(context, self, StringView(digits, length));
}

template <typename T> void BuilderAppendFloat(ScriptCallContext &context, ScriptObject *self, T value)
{
    StringBuilder<64> text;
    AppendFloat(text, value);
    BuilderAppend(context, self, text.AsView());
}

void BuilderAppendBool(ScriptCallContext &context, ScriptObject *self, bool value)
{
    BuilderAppend(context, self, value ? StringView("True") : StringView("False"));
}

void BuilderAppendRune(ScriptCallContext &context, ScriptObject *self, uint32 rune)
{
    if (rune > Utf8::MaxCodePoint || (rune >= 0xD800 && rune <= 0xDFFF))
    {
        context.Fault(ScriptFault::Panic, "the value is not a Unicode scalar value");
        return;
    }
    char bytes[4];
    const usize length = Utf8::Encode(static_cast<char32_t>(rune), bytes);
    BuilderAppend(context, self, StringView(bytes, length));
}

/// Splits a format specifier (MarlinScript §10.2) into its letter, its precision (-1 when it has none) and
/// whether the letter is upper case.
bool ParseFormat(StringView format, char &kind, int32 &precision, bool &upper)
{
    if (format.IsEmpty())
    {
        kind = 'G';
        precision = -1;
        upper = true;
        return true;
    }
    const char letter = format[0];
    upper = letter >= 'A' && letter <= 'Z';
    kind = upper ? letter : static_cast<char>(letter - 'a' + 'A');
    if (kind != 'D' && kind != 'X' && kind != 'F' && kind != 'E' && kind != 'G')
    {
        return false;
    }
    precision = -1;
    if (format.Length() > 1)
    {
        uint32 digits = 0;
        if (!TryParse(format.RemoveStart(1), digits) || digits > 99)
        {
            return false;
        }
        precision = static_cast<int32>(digits);
    }
    return true;
}

template <typename T> void AppendFloatFormatted(StringWriter &text, T value, char kind, int32 precision, bool upper)
{
    switch (kind)
    {
    case 'F':
        AppendFloat(text, value, FloatStyle::Fixed, precision < 0 ? 2 : precision, upper);
        return;
    case 'E':
        AppendFloat(text, value, FloatStyle::Exponent, precision < 0 ? 6 : precision, upper);
        return;
    default:
        if (precision <= 0)
        {
            AppendFloat(text, value, FloatStyle::Shortest, 0, upper);
        }
        else
        {
            AppendFloat(text, value, FloatStyle::General, precision, upper);
        }
        return;
    }
}

template <typename T>
void BuilderAppendIntegerFormat(ScriptCallContext &context, ScriptObject *self, T value, StringView format)
{
    char kind = 'G';
    int32 precision = -1;
    bool upper = true;
    if (!ParseFormat(format, kind, precision, upper))
    {
        context.Fault(ScriptFault::Panic, "the format specifier is not D, X, F, E or G");
        return;
    }
    StringBuilder<128> text;
    char digits[MaxIntegerLength];
    if (kind == 'D' || kind == 'X' || (kind == 'G' && precision < 0))
    {
        usize length = 0;
        bool negative = false;
        if (kind == 'X')
        {
            // Hexadecimal shows the bits of the value's own width.
            using TUnsigned = MakeUnsigned<T>;
            length = FormatUnsigned(static_cast<uint64>(static_cast<TUnsigned>(value)), digits, 16, upper);
        }
        else if constexpr (IsSigned<T>)
        {
            negative = value < 0;
            length = FormatUnsigned(negative ? 0 - static_cast<uint64>(static_cast<int64>(value))
                                             : static_cast<uint64>(value),
                                    digits);
        }
        else
        {
            length = FormatUnsigned(static_cast<uint64>(value), digits);
        }
        if (negative)
        {
            text.Append('-');
        }
        if (kind != 'G' && precision > 0 && static_cast<usize>(precision) > length)
        {
            text.Append(static_cast<usize>(precision) - length, '0');
        }
        text.Append(StringView(digits, length));
    }
    else
    {
        // RECOVERY-DECISION SB5: F, E and G with a precision format an integer as the double it converts to.
        AppendFloatFormatted(text, static_cast<float64>(value), kind, precision, upper);
    }
    BuilderAppend(context, self, text.AsView());
}

template <typename T>
void BuilderAppendFloatFormat(ScriptCallContext &context, ScriptObject *self, T value, StringView format)
{
    char kind = 'G';
    int32 precision = -1;
    bool upper = true;
    // RECOVERY-DECISION SB6: D and X apply to integers only; on floats they fault like an unknown specifier.
    if (!ParseFormat(format, kind, precision, upper) || kind == 'D' || kind == 'X')
    {
        context.Fault(ScriptFault::Panic, "the format specifier is not D, X, F, E or G");
        return;
    }
    StringBuilder<128> text;
    AppendFloatFormatted(text, value, kind, precision, upper);
    BuilderAppend(context, self, text.AsView());
}

ScriptArray *BuilderToString(ScriptCallContext &context, ScriptObject *self)
{
    ScriptStringBuilderState &state = StateOf<ScriptStringBuilderState>(self);
    if (state.Buffer == nullptr)
    {
        return context.GetDomain().NewString({});
    }
    return context.GetDomain().NewString(
        StringView(reinterpret_cast<const char *>(BytesOf(state.Buffer)), state.Length));
}

int32 ArrayLength(ScriptArray *self)
{
    return static_cast<int32>(self->Length);
}

bool CheckRange(ScriptCallContext &context, const ScriptArray &array, int32 start, int32 count)
{
    if (start < 0 || count < 0 || static_cast<int64>(start) + count > static_cast<int64>(array.Length))
    {
        context.Fault(ScriptFault::IndexOutOfRange,
                      Format("elements {0}..{1} are outside an array of {2}", start, static_cast<int64>(start) + count,
                             array.Length)
                          .AsView());
        return false;
    }
    return true;
}

void ArrayCopyTo(ScriptCallContext &context, ScriptArray *self, int32 sourceIndex, ScriptArray *destination,
                 int32 destinationIndex, int32 count)
{
    if (destination == nullptr)
    {
        context.Fault(ScriptFault::NullReference, "the destination array is null");
        return;
    }
    if (!CheckRange(context, *self, sourceIndex, count) || !CheckRange(context, *destination, destinationIndex, count))
    {
        return;
    }
    context.GetDomain().CopyElements(*self, static_cast<uint32>(sourceIndex), *destination,
                                     static_cast<uint32>(destinationIndex), static_cast<uint32>(count));
}

void ArrayClear(ScriptCallContext &context, ScriptArray *self, int32 start, int32 count)
{
    if (CheckRange(context, *self, start, count))
    {
        context.GetDomain().ClearElements(*self, static_cast<uint32>(start), static_cast<uint32>(count));
    }
}

ScriptArray *TypeInfoName(ScriptCallContext &context, ScriptObject *self)
{
    return context.GetDomain().NewString(StateOf<ScriptTypeInfoState>(self).Described->Name.AsView());
}

bool TypeInfoIsAssignableFrom(ScriptObject *self, ScriptObject *other)
{
    return other != nullptr && ScriptDomain::IsAssignable(*StateOf<ScriptTypeInfoState>(other).Described,
                                                          *StateOf<ScriptTypeInfoState>(self).Described);
}

void ScriptPanic(ScriptCallContext &context, StringView message)
{
    context.Fault(ScriptFault::Panic, message);
}

void DebugPrint(StringView message)
{
    MARLIN_LOG(LogScript, Info, "{0}", message);
}

void AssertFail(ScriptCallContext &context, StringView message)
{
    context.Fault(ScriptFault::AssertFailed, message);
}

template <typename T> ScriptArray *NumberToString(ScriptCallContext &context, T value)
{
    StringBuilder<64> text;
    if constexpr (IsFloatingPoint<T>)
    {
        AppendFloat(text, value);
    }
    else
    {
        char digits[MaxIntegerLength];
        text.Append(StringView(digits, IsSigned<T> ? FormatSigned(static_cast<int64>(value), digits)
                                                   : FormatUnsigned(static_cast<uint64>(value), digits)));
    }
    return context.GetDomain().NewString(text.AsView());
}

ScriptArray *BoolToString(ScriptCallContext &context, bool value)
{
    return context.GetDomain().NewString(value ? StringView("True") : StringView("False"));
}
} // namespace

void RegisterScriptCoreNatives(ScriptNativeRegistry &registry)
{
    registry.Register("Object.Equals", &ObjectEquals);
    registry.Register("Object.GetHashCode", &ObjectGetHashCode);
    registry.Register("Object.ToString", &ObjectToString);
    registry.Register("Object.GetType", &ObjectGetType);

    registry.Register("String.Length", &StringLength);
    registry.Register("String.Equals", &StringEquals);
    registry.Register("String.Concat", &StringConcat);
    registry.Register("String.Substring", &StringSubstring);
    registry.Register("String.Contains", &StringContains);
    registry.Register("String.StartsWith", &StringStartsWith);
    registry.Register("String.EndsWith", &StringEndsWith);
    registry.Register("String.CompareTo", &StringCompareTo);
    registry.Register("String.GetHashCode", &ObjectGetHashCode);
    registry.Register("String.IndexOf", &StringIndexOf);
    registry.Register("String.LastIndexOf", &StringLastIndexOf);
    registry.Register("String.GetByte", &StringGetByte);
    registry.Register("String.Replace", &StringReplace);
    registry.Register("String.Trim", &StringTrim<true, true>);
    registry.Register("String.TrimStart", &StringTrim<true, false>);
    registry.Register("String.TrimEnd", &StringTrim<false, true>);
    registry.Register("String.ToUpperAscii", &StringChangeAsciiCase<true>);
    registry.Register("String.ToLowerAscii", &StringChangeAsciiCase<false>);

    registry.Register("Number.TryParseInt32", &NumberTryParse<int32>);
    registry.Register("Number.TryParseUInt32", &NumberTryParse<uint32>);
    registry.Register("Number.TryParseInt64", &NumberTryParse<int64>);
    registry.Register("Number.TryParseUInt64", &NumberTryParse<uint64>);
    registry.Register("Number.TryParseFloat", &NumberTryParse<float32>);
    registry.Register("Number.TryParseDouble", &NumberTryParse<float64>);

    registry.Register("StringBuilder.New", &BuilderNew);
    registry.Register("StringBuilder.AppendString", &BuilderAppendString);
    registry.Register("StringBuilder.AppendInt32", &BuilderAppendInteger<int32>);
    registry.Register("StringBuilder.AppendUInt32", &BuilderAppendInteger<uint32>);
    registry.Register("StringBuilder.AppendInt64", &BuilderAppendInteger<int64>);
    registry.Register("StringBuilder.AppendUInt64", &BuilderAppendInteger<uint64>);
    registry.Register("StringBuilder.AppendFloat", &BuilderAppendFloat<float32>);
    registry.Register("StringBuilder.AppendDouble", &BuilderAppendFloat<float64>);
    registry.Register("StringBuilder.AppendBool", &BuilderAppendBool);
    registry.Register("StringBuilder.AppendRune", &BuilderAppendRune);
    registry.Register("StringBuilder.AppendInt32Format", &BuilderAppendIntegerFormat<int32>);
    registry.Register("StringBuilder.AppendUInt32Format", &BuilderAppendIntegerFormat<uint32>);
    registry.Register("StringBuilder.AppendInt64Format", &BuilderAppendIntegerFormat<int64>);
    registry.Register("StringBuilder.AppendUInt64Format", &BuilderAppendIntegerFormat<uint64>);
    registry.Register("StringBuilder.AppendFloatFormat", &BuilderAppendFloatFormat<float32>);
    registry.Register("StringBuilder.AppendDoubleFormat", &BuilderAppendFloatFormat<float64>);
    registry.Register("StringBuilder.ToString", &BuilderToString);

    registry.Register("Array.Length", &ArrayLength);
    registry.Register("Array.CopyTo", &ArrayCopyTo);
    registry.Register("Array.Clear", &ArrayClear);

    registry.Register("TypeInfo.Name", &TypeInfoName);
    registry.Register("TypeInfo.IsAssignableFrom", &TypeInfoIsAssignableFrom);

    registry.Register("WeakRef.New", &WeakRefNew);
    registry.Register("WeakRef.GetTarget", &WeakRefGetTarget);
    registry.Register("WeakRef.SetTarget", &WeakRefSetTarget);

    registry.Register("Random.Entropy", &RandomEntropy);
    registry.Register("Script.Panic", &ScriptPanic);
    registry.Register("Script.AssertFailed", &AssertFail);
    registry.Register("Debug.Print", &DebugPrint);

    registry.Register("Int32.ToString", &NumberToString<int32>);
    registry.Register("UInt32.ToString", &NumberToString<uint32>);
    registry.Register("Int64.ToString", &NumberToString<int64>);
    registry.Register("UInt64.ToString", &NumberToString<uint64>);
    registry.Register("Float.ToString", &NumberToString<float32>);
    registry.Register("Double.ToString", &NumberToString<float64>);
    registry.Register("Bool.ToString", &BoolToString);
}
} // namespace Marlin
