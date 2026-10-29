#include "Diagnostics/Assert.h"

#include "Diagnostics/Log.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformDebug.h"

namespace Marlin::Private
{
namespace
{
class FailureMessage
{
public:
    void Append(const char *text)
    {
        while (*text != '\0' && _length < Capacity - 1)
        {
            _buffer[_length++] = *text++;
        }
        _buffer[_length] = '\0';
    }

    void Append(int32 value)
    {
        char digits[12];
        int32 count = 0;
        uint32 magnitude = value < 0 ? 0u - static_cast<uint32>(value) : static_cast<uint32>(value);
        do
        {
            digits[count++] = static_cast<char>('0' + magnitude % 10);
            magnitude /= 10;
        } while (magnitude != 0);
        if (value < 0)
        {
            digits[count++] = '-';
        }
        char text[12];
        for (int32 i = 0; i < count; ++i)
        {
            text[i] = digits[count - 1 - i];
        }
        text[count] = '\0';
        Append(text);
    }

    const char *Text() const
    {
        return _buffer;
    }

private:
    static constexpr usize Capacity = 2048;
    char _buffer[Capacity] = {};
    usize _length = 0;
};

/// Set while a failure is reported, so a failure inside logging falls back to the debugger output.
thread_local bool _failing = false;

[[noreturn]] void Fail(const char *kind, const char *what, const char *file, int32 line)
{
    PlatformCrash::NoteFailure(kind, what, file, line);
    if (!_failing)
    {
        _failing = true;
        FailureMessage logged;
        logged.Append(kind);
        logged.Append(": ");
        logged.Append(what);
        WriteLog(LogCore, LogLevel::Fatal, file, line, logged.Text());
    }
    FailureMessage message;
    message.Append(file);
    message.Append("(");
    message.Append(line);
    message.Append("): ");
    message.Append(kind);
    message.Append(": ");
    message.Append(what);
    message.Append("\n");
    PlatformDebug::Output(message.Text());
    if (PlatformDebug::IsDebuggerAttached())
    {
        MARLIN_DEBUG_BREAK();
    }
    MARLIN_TRAP();
}
} // namespace

void AssertFailed(const char *expression, const char *file, int32 line)
{
    Fail("Assertion failed", expression, file, line);
}

void FatalError(const char *message, const char *file, int32 line)
{
    Fail("Fatal error", message, file, line);
}
} // namespace Marlin::Private
