#include "HAL/PlatformDirectoryWatcher.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin
{
namespace
{
constexpr DWORD BufferSize = 64 * 1024;
constexpr DWORD NotifyFilter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                               FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_CREATION;

struct WatcherState
{
    HANDLE Directory = nullptr;
    OVERLAPPED Overlapped = {};
    alignas(DWORD) uint8 Buffer[BufferSize] = {};
    DWORD Bytes = 0;
    DWORD Offset = 0;
    bool Recursive = false;
    bool Pending = false;
    bool Ready = false;
    bool Overflowed = false;
};

WatcherState *Watcher(void *state)
{
    return static_cast<WatcherState *>(state);
}

bool Issue(WatcherState &state)
{
    state.Pending = ReadDirectoryChangesW(state.Directory, state.Buffer, BufferSize, state.Recursive ? TRUE : FALSE,
                                          NotifyFilter, nullptr, &state.Overlapped, nullptr) != 0;
    return state.Pending;
}

FileChangeKind ToKind(DWORD action)
{
    switch (action)
    {
    case FILE_ACTION_ADDED:
        return FileChangeKind::Added;
    case FILE_ACTION_REMOVED:
        return FileChangeKind::Removed;
    case FILE_ACTION_RENAMED_OLD_NAME:
        return FileChangeKind::RenamedFrom;
    case FILE_ACTION_RENAMED_NEW_NAME:
        return FileChangeKind::RenamedTo;
    default:
        return FileChangeKind::Modified;
    }
}
} // namespace

DirectoryWatcher::~DirectoryWatcher()
{
    Close();
}

bool DirectoryWatcher::Open(const char *path, bool recursive)
{
    Close();
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    auto *state = static_cast<WatcherState *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WatcherState)));
    if (state == nullptr)
    {
        return false;
    }
    state->Recursive = recursive;
    state->Directory =
        CreateFileW(widePath.Get(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    state->Overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    _state = state;
    if (state->Directory == INVALID_HANDLE_VALUE || state->Overlapped.hEvent == nullptr || !Issue(*state))
    {
        if (state->Directory == INVALID_HANDLE_VALUE)
        {
            state->Directory = nullptr;
        }
        Close();
        return false;
    }
    return true;
}

void DirectoryWatcher::Close()
{
    WatcherState *state = Watcher(_state);
    if (state == nullptr)
    {
        return;
    }
    if (state->Pending)
    {
        CancelIoEx(state->Directory, &state->Overlapped);
        DWORD bytes = 0;
        GetOverlappedResult(state->Directory, &state->Overlapped, &bytes, TRUE);
    }
    if (state->Directory != nullptr)
    {
        CloseHandle(state->Directory);
    }
    if (state->Overlapped.hEvent != nullptr)
    {
        CloseHandle(state->Overlapped.hEvent);
    }
    HeapFree(GetProcessHeap(), 0, state);
    _state = nullptr;
}

bool DirectoryWatcher::Wait(uint32 timeout)
{
    WatcherState *state = Watcher(_state);
    if (state == nullptr)
    {
        return false;
    }
    if (state->Ready)
    {
        return true;
    }
    if (!state->Pending || WaitForSingleObject(state->Overlapped.hEvent, timeout) != WAIT_OBJECT_0)
    {
        return false;
    }
    DWORD bytes = 0;
    const bool succeeded = GetOverlappedResult(state->Directory, &state->Overlapped, &bytes, FALSE) != 0;
    state->Pending = false;
    ResetEvent(state->Overlapped.hEvent);
    state->Bytes = succeeded ? bytes : 0;
    state->Offset = 0;
    state->Overflowed = !succeeded || bytes == 0;
    state->Ready = true;
    return true;
}

bool DirectoryWatcher::Next(FileChange &change)
{
    WatcherState *state = Watcher(_state);
    if (state == nullptr || !state->Ready)
    {
        return false;
    }
    if (state->Overflowed)
    {
        change.Kind = FileChangeKind::Overflow;
        change.Path[0] = '\0';
        state->Overflowed = false;
        state->Ready = false;
        Issue(*state);
        return true;
    }

    const auto *info = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(state->Buffer + state->Offset);
    change.Kind = ToKind(info->Action);
    const auto nameLength = static_cast<int32>(info->FileNameLength / sizeof(wchar_t));
    const int32 written = WideCharToMultiByte(CP_UTF8, 0, info->FileName, nameLength, change.Path,
                                              static_cast<int32>(FileChange::MaxPathLength - 1), nullptr, nullptr);
    change.Path[written > 0 ? written : 0] = '\0';
    for (char *c = change.Path; *c != '\0'; ++c)
    {
        if (*c == '\\')
        {
            *c = '/';
        }
    }

    if (info->NextEntryOffset == 0)
    {
        state->Ready = false;
        Issue(*state);
    }
    else
    {
        state->Offset += info->NextEntryOffset;
    }
    return true;
}
} // namespace Marlin
