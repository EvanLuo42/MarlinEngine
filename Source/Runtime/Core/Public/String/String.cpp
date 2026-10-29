#include "String/String.h"

#include "Memory/Memory.h"

namespace Marlin
{
String::String(usize count, char c)
{
    InitInline();
    Append(count, c);
}

void String::InitFrom(const char *data, usize length)
{
    if (length <= InlineCapacity)
    {
        InitInline();
        MemCopy(_bytes, data, length);
        SetLength(length);
        return;
    }
    char *heap = static_cast<char *>(Memory::Allocate(length + 1));
    MemCopy(heap, data, length);
    SetHeap(heap, length, Memory::GetAllocationSize(heap) - 1);
    heap[length] = '\0';
}

void String::SetHeap(char *data, usize length, usize capacity)
{
    const usize flagged = (capacity & CapacityMask) | (usize{HeapFlag} << (8 * (sizeof(usize) - 1)));
    MemCopy(_bytes, static_cast<const void *>(&data), sizeof(data));
    MemCopy(_bytes + sizeof(void *), &length, sizeof(length));
    MemCopy(_bytes + 2 * sizeof(void *), &flagged, sizeof(flagged));
}

void String::SetLength(usize length)
{
    if (IsHeap())
    {
        MemCopy(_bytes + sizeof(void *), &length, sizeof(length));
        HeapData()[length] = '\0';
    }
    else
    {
        MARLIN_CHECK_SLOW(length <= InlineCapacity);
        _bytes[length] = '\0';
        _bytes[InlineCapacity] = static_cast<char>(InlineCapacity - length);
    }
}

void String::Assign(StringView text)
{
    const usize length = text.Length();
    if (length > Capacity())
    {
        // The text may live in this string, so it is copied before the old buffer goes.
        String copy(text);
        *this = Move(copy);
        return;
    }
    MemMove(Data(), text.Data(), length);
    SetLength(length);
}

void String::Grow(usize required)
{
    const usize capacity = Capacity();
    usize target = capacity + capacity / 2;
    target = target < required ? required : target;
    target = target < 2 * InlineCapacity ? 2 * InlineCapacity : target;

    const usize length = Length();
    char *heap = nullptr;
    if (IsHeap())
    {
        heap = static_cast<char *>(Memory::Reallocate(HeapData(), target + 1));
    }
    else
    {
        heap = static_cast<char *>(Memory::Allocate(target + 1));
        MemCopy(heap, _bytes, length + 1);
    }
    SetHeap(heap, length, Memory::GetAllocationSize(heap) - 1);
}

void String::FreeHeap()
{
    Memory::Free(HeapData());
}

String &String::Append(const char *data, usize length)
{
    const usize oldLength = Length();
    if (oldLength + length > Capacity())
    {
        const char *begin = Data();
        if (data >= begin && data < begin + oldLength)
        {
            const auto offset = static_cast<usize>(data - begin);
            Grow(oldLength + length);
            data = Data() + offset;
        }
        else
        {
            Grow(oldLength + length);
        }
    }
    MemMove(Data() + oldLength, data, length);
    SetLength(oldLength + length);
    return *this;
}

String &String::Append(usize count, char c)
{
    const usize length = Length();
    Reserve(length + count);
    MemSet(Data() + length, static_cast<uint8>(c), count);
    SetLength(length + count);
    return *this;
}

void String::Insert(usize index, StringView text)
{
    const usize length = Length();
    MARLIN_ASSERT(index <= length);
    if (text.IsEmpty())
    {
        return;
    }
    const String copy = text.Data() >= Data() && text.Data() < Data() + length ? String(text) : String();
    const StringView source = copy.IsEmpty() ? text : copy.AsView();
    Reserve(length + source.Length());
    char *data = Data();
    MemMove(data + index + source.Length(), data + index, length - index);
    MemCopy(data + index, source.Data(), source.Length());
    SetLength(length + source.Length());
}

void String::Remove(usize index, usize count)
{
    const usize length = Length();
    MARLIN_ASSERT(index <= length);
    count = count < length - index ? count : length - index;
    char *data = Data();
    MemMove(data + index, data + index + count, length - index - count);
    SetLength(length - count);
}

usize String::Replace(StringView from, StringView to)
{
    MARLIN_ASSERT(!from.IsEmpty());
    const StringView text = AsView();
    usize found = text.IndexOf(from);
    if (found == NotFound)
    {
        return 0;
    }
    String result;
    result.Reserve(text.Length());
    usize start = 0;
    usize count = 0;
    while (found != NotFound)
    {
        result.Append(text.Data() + start, found - start);
        result.Append(to);
        start = found + from.Length();
        found = text.IndexOf(from, start);
        ++count;
    }
    result.Append(text.Data() + start, text.Length() - start);
    *this = Move(result);
    return count;
}

void String::Replace(char from, char to)
{
    for (char &c : *this)
    {
        c = c == from ? to : c;
    }
}

void String::Resize(usize length, char fill)
{
    const usize oldLength = Length();
    if (length > oldLength)
    {
        Append(length - oldLength, fill);
    }
    else
    {
        SetLength(length);
    }
}

char *String::ResizeUninitialized(usize length)
{
    Reserve(length);
    SetLength(length);
    return Data();
}

void String::ShrinkToFit()
{
    if (IsHeap())
    {
        String copy(AsView());
        if (copy.Capacity() < HeapCapacity())
        {
            *this = Move(copy);
        }
    }
}

void String::ToUpper()
{
    for (char &c : *this)
    {
        c = Ascii::ToUpper(c);
    }
}

void String::ToLower()
{
    for (char &c : *this)
    {
        c = Ascii::ToLower(c);
    }
}

void String::TrimStart()
{
    const StringView text = AsView();
    Remove(0, text.Length() - text.TrimStart().Length());
}

void String::TrimEnd()
{
    SetLength(AsView().TrimEnd().Length());
}

void String::Trim()
{
    TrimEnd();
    TrimStart();
}
} // namespace Marlin
