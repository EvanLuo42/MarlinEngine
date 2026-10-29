#include "HAL/PlatformCrash.h"

namespace Marlin::PlatformCrash
{
namespace
{
struct NotedFailure
{
    char Kind[64] = {};
    char Message[2048] = {};
    char File[260] = {};
    CrashFailure Failure;
    bool Noted = false;
};

thread_local NotedFailure _noted;

void CopyText(StringView source, char *target, usize capacity)
{
    const usize length = source.Length() < capacity - 1 ? source.Length() : capacity - 1;
    for (usize i = 0; i < length; ++i)
    {
        target[i] = source[i];
    }
    target[length] = '\0';
}

StringView TextOf(const char *text)
{
    return text != nullptr ? StringView(text) : StringView();
}
} // namespace

void NoteFailure(const char *kind, StringView message, const char *file, int32 line)
{
    NotedFailure &noted = _noted;
    CopyText(TextOf(kind), noted.Kind, sizeof(noted.Kind));
    CopyText(message, noted.Message, sizeof(noted.Message));
    CopyText(TextOf(file), noted.File, sizeof(noted.File));
    noted.Failure = {
        .Kind = noted.Kind,
        .Message = noted.Message,
        .File = file != nullptr ? noted.File : nullptr,
        .Line = line,
    };
    noted.Noted = true;
}

const CrashFailure *GetNotedFailure()
{
    return _noted.Noted ? &_noted.Failure : nullptr;
}
} // namespace Marlin::PlatformCrash
