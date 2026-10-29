#include "String/StringBuilder.h"

#include "Memory/Memory.h"

namespace Marlin
{
StringWriter::~StringWriter()
{
    if (_data != _inlineBuffer)
    {
        Memory::Free(_data);
    }
}

void StringWriter::Grow(usize extra)
{
    usize capacity = _capacity + _capacity / 2;
    capacity = capacity < _length + extra ? _length + extra : capacity;
    capacity = capacity < 64 ? 64 : capacity;
    char *data = nullptr;
    if (_data == _inlineBuffer)
    {
        data = static_cast<char *>(Memory::Allocate(capacity + 1));
        MemCopy(data, _data, _length);
    }
    else
    {
        data = static_cast<char *>(Memory::Reallocate(_data, capacity + 1));
    }
    _data = data;
    _capacity = Memory::GetAllocationSize(data) - 1;
}
} // namespace Marlin
