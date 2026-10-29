#include "Containers/FrozenArray.h"

#include "String/StringBuilder.h"

namespace Marlin
{
namespace Private
{
const FrozenLayout FrozenArrayLayout = FrozenLayout::Array;
const FrozenLayout FrozenStringLayout = FrozenLayout::String;
} // namespace Private

FrozenString::FrozenString(StringView text)
{
    if (!text.IsEmpty())
    {
        MARLIN_ASSERT(text.Length() < 0xFFFFFFFFu);
        _data = static_cast<char *>(Memory::Allocate(text.Length() + 1, 1));
        MemCopy(_data, text.Data(), text.Length());
        _data[text.Length()] = '\0';
        _length = static_cast<uint32>(text.Length());
        _owned = 1;
    }
}

FrozenString::FrozenString(const FrozenString &other) : FrozenString(other.AsView())
{
}

FrozenString::FrozenString(FrozenString &&other) noexcept
    : _data(other._data), _length(other._length), _owned(other._owned)
{
    other._data = nullptr;
    other._length = 0;
    other._owned = 0;
}

FrozenString::~FrozenString()
{
    Reset();
}

FrozenString &FrozenString::operator=(const FrozenString &other)
{
    if (this != &other)
    {
        *this = FrozenString(other.AsView());
    }
    return *this;
}

FrozenString &FrozenString::operator=(FrozenString &&other) noexcept
{
    if (this != &other)
    {
        Reset();
        _data = other._data;
        _length = other._length;
        _owned = other._owned;
        other._data = nullptr;
        other._length = 0;
        other._owned = 0;
    }
    return *this;
}

void FrozenString::Reset()
{
    if (_owned != 0)
    {
        Memory::Free(_data);
    }
    _data = nullptr;
    _length = 0;
    _owned = 0;
}

void FrozenString::FormatTo(StringWriter &output, StringView /*spec*/) const
{
    output.Append(AsView());
}

bool FrozenString::TryParse(StringView text, FrozenString &value)
{
    value = FrozenString(text);
    return true;
}
} // namespace Marlin
