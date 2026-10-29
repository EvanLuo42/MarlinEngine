#include "HAL/PlatformFile.h"

#include "Diagnostics/Assert.h"
#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformSystem.h"

namespace Marlin
{
namespace
{
HANDLE ToHandle(FileHandle file)
{
    return reinterpret_cast<HANDLE>(file.Value);
}

FileInfo ToFileInfo(DWORD attributes, DWORD sizeHigh, DWORD sizeLow, FILETIME modified)
{
    return {
        .Size = (static_cast<uint64>(sizeHigh) << 32) | sizeLow,
        .ModifiedTime = Microsoft::FileTimeToTicks(modified),
        .IsDirectory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
        .IsReadOnly = (attributes & FILE_ATTRIBUTE_READONLY) != 0,
    };
}

DWORD ChunkSize(usize size)
{
    constexpr usize MaxChunk = 1u << 30;
    return static_cast<DWORD>(size < MaxChunk ? size : MaxChunk);
}

bool IsDotEntry(const wchar_t *name)
{
    return name[0] == L'.' && (name[1] == L'\0' || (name[1] == L'.' && name[2] == L'\0'));
}

bool DeleteTree(const wchar_t *path)
{
    const usize length = [&] {
        usize count = 0;
        while (path[count] != L'\0')
        {
            ++count;
        }
        return count;
    }();
    const HANDLE heap = GetProcessHeap();
    auto *pattern = static_cast<wchar_t *>(HeapAlloc(heap, 0, (length + MAX_PATH + 3) * sizeof(wchar_t)));
    if (pattern == nullptr)
    {
        return false;
    }
    for (usize i = 0; i < length; ++i)
    {
        pattern[i] = path[i];
    }
    pattern[length] = L'\\';
    pattern[length + 1] = L'*';
    pattern[length + 2] = L'\0';

    bool succeeded = true;
    WIN32_FIND_DATAW data;
    const HANDLE find =
        FindFirstFileExW(pattern, FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (IsDotEntry(data.cFileName))
            {
                continue;
            }
            usize nameLength = 0;
            for (; data.cFileName[nameLength] != L'\0'; ++nameLength)
            {
                pattern[length + 1 + nameLength] = data.cFileName[nameLength];
            }
            pattern[length + 1 + nameLength] = L'\0';
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0)
            {
                SetFileAttributesW(pattern, data.dwFileAttributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
            }
            const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                                     (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
            if (isDirectory)
            {
                succeeded = DeleteTree(pattern) && succeeded;
            }
            else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                succeeded = RemoveDirectoryW(pattern) != 0 && succeeded;
            }
            else
            {
                succeeded = DeleteFileW(pattern) != 0 && succeeded;
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
    }
    HeapFree(heap, 0, pattern);
    return RemoveDirectoryW(path) != 0 && succeeded;
}

wchar_t FoldCase(wchar_t c)
{
    return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : (c == L'/' ? L'\\' : c);
}

/// Skips "\\?\", "\\?\UNC\server\share\", "\\server\share\" or "C:\" so only the components below the volume remain.
const wchar_t *SkipVolume(const wchar_t *full)
{
    if (full[0] == L'\\' && full[1] == L'\\' && full[2] == L'?' && full[3] == L'\\')
    {
        full += 4;
        if (FoldCase(full[0]) == L'u' && FoldCase(full[1]) == L'n' && FoldCase(full[2]) == L'c' && full[3] == L'\\')
        {
            full += 4;
            for (int separators = 0; *full != L'\0' && separators < 2; ++full)
            {
                separators += *full == L'\\' ? 1 : 0;
            }
            return full;
        }
    }
    else if (full[0] == L'\\' && full[1] == L'\\')
    {
        full += 2;
        for (int separators = 0; *full != L'\0' && separators < 2; ++full)
        {
            separators += *full == L'\\' ? 1 : 0;
        }
        return full;
    }
    if (full[0] != L'\0' && full[1] == L':')
    {
        full += 2;
    }
    while (*full == L'\\')
    {
        ++full;
    }
    return full;
}

/// The number of '\'-separated components in *path*.
int CountComponents(const wchar_t *path)
{
    int components = 0;
    bool inComponent = false;
    for (const wchar_t *c = path; *c != L'\0'; ++c)
    {
        if (*c == L'\\')
        {
            inComponent = false;
        }
        else if (!inComponent)
        {
            inComponent = true;
            ++components;
        }
    }
    return components;
}

/// True if *ancestor* names *path* or one of its parents.
bool IsSameOrAncestor(const wchar_t *ancestor, const wchar_t *path)
{
    usize i = 0;
    for (; ancestor[i] != L'\0'; ++i)
    {
        if (FoldCase(ancestor[i]) != FoldCase(path[i]))
        {
            return false;
        }
    }
    return path[i] == L'\0' || path[i] == L'\\' || (i > 0 && ancestor[i - 1] == L'\\');
}

/// How many components *path* has below *ancestor*, or -1 if *ancestor* is not *path* or one of its parents.
int DepthBelow(const wchar_t *ancestor, const wchar_t *path)
{
    if (!IsSameOrAncestor(ancestor, path))
    {
        return -1;
    }
    usize length = 0;
    while (ancestor[length] != L'\0')
    {
        ++length;
    }
    return CountComponents(path + length);
}

/// Drops the long-path prefix so "\\?\C:\x" and "C:\x" compare equal.
const wchar_t *StripPrefix(const wchar_t *text)
{
    return text[0] == L'\\' && text[1] == L'\\' && text[2] == L'?' && text[3] == L'\\' ? text + 4 : text;
}

/// Long paths reach 32767 characters; buffers of this size go on the heap since fibers run on small stacks.
constexpr DWORD PathCapacity = 32768;

/// True if *full*, a full path, names *other* (any path) or one of its parents. *scratch* holds PathCapacity.
bool CoversPath(const wchar_t *full, const wchar_t *other, wchar_t *scratch)
{
    const DWORD length = GetFullPathNameW(other, PathCapacity, scratch, nullptr);
    return length != 0 && length < PathCapacity && IsSameOrAncestor(StripPrefix(full), StripPrefix(scratch));
}

/// True if *full* is a folder of the profile at *home*, such as Desktop, a OneDrive or a source folder, or another
/// profile beside it or one of that profile's folders. *home* is a full path and is changed in place.
bool IsProfileFolder(const wchar_t *full, wchar_t *home)
{
    home += StripPrefix(home) - home;
    full = StripPrefix(full);
    if (DepthBelow(home, full) == 1)
    {
        return true;
    }
    // Profiles share a parent, such as C:\Users; a home at a volume root has no such parent to guard.
    if (CountComponents(SkipVolume(home)) < 2)
    {
        return false;
    }
    usize end = 0;
    while (home[end] != L'\0')
    {
        ++end;
    }
    while (end > 0 && home[end - 1] == L'\\')
    {
        --end;
    }
    while (end > 0 && home[end - 1] != L'\\')
    {
        --end;
    }
    home[end - 1] = L'\0';
    const int depth = DepthBelow(home, full);
    return depth == 1 || depth == 2;
}

/// True if *full*, a full path, is a volume root or a top-level folder, or names a folder a delete must never take, or
/// one of its parents: the executable's directory, the current directory and every KnownFolder; or is a folder of the
/// user's profile or another profile beside it (see IsProfileFolder). *input* and *scratch* hold PathCapacity each.
bool IsProtected(const wchar_t *full, wchar_t *input, wchar_t *scratch)
{
    if (CountComponents(SkipVolume(full)) < 2)
    {
        return true;
    }
    const DWORD moduleLength = GetModuleFileNameW(nullptr, input, PathCapacity);
    if (moduleLength == 0 || moduleLength >= PathCapacity || CoversPath(full, input, scratch))
    {
        return true;
    }
    const DWORD currentLength = GetCurrentDirectoryW(PathCapacity, input);
    if (currentLength != 0 && currentLength < PathCapacity && CoversPath(full, input, scratch))
    {
        return true;
    }
    for (const KnownFolder folder :
         {KnownFolder::Home, KnownFolder::LocalAppData, KnownFolder::RoamingAppData, KnownFolder::Documents,
          KnownFolder::SavedGames, KnownFolder::Temp, KnownFolder::Desktop, KnownFolder::Downloads,
          KnownFolder::Pictures, KnownFolder::Music, KnownFolder::Videos})
    {
        auto *utf8 = reinterpret_cast<char *>(scratch);
        const usize utf8Capacity = PathCapacity * sizeof(wchar_t);
        const usize utf8Length = PlatformSystem::GetKnownFolder(folder, utf8, utf8Capacity);
        if (utf8Length == 0 || utf8Length >= utf8Capacity)
        {
            continue;
        }
        const usize wideLength = Microsoft::Utf8ToWide(utf8, input, PathCapacity);
        if (wideLength == 0 || wideLength >= PathCapacity)
        {
            continue;
        }
        const DWORD folderLength = GetFullPathNameW(input, PathCapacity, scratch, nullptr);
        if (folderLength == 0 || folderLength >= PathCapacity)
        {
            continue;
        }
        if (IsSameOrAncestor(StripPrefix(full), StripPrefix(scratch)) ||
            (folder == KnownFolder::Home && IsProfileFolder(full, scratch)))
        {
            return true;
        }
    }
    return false;
}

/// True for "X:/..." or "X:\..." and for a UNC or "\\?\" path: one that does not depend on the current drive or
/// directory.
bool IsFullPath(const char *path)
{
    if (path == nullptr)
    {
        return false;
    }
    const auto isSeparator = [](char c) { return c == '/' || c == '\\'; };
    const bool drive = ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':' &&
                       isSeparator(path[2]);
    return drive || (isSeparator(path[0]) && isSeparator(path[1]) && path[2] != '\0');
}

/// A recursive delete is refused for an empty path, a volume root or a top-level folder, and the executable's
/// directory, the current directory, a KnownFolder or a parent of one, both as named and as reached through links:
/// an empty or truncated path must never reach DeleteTree, whose "\*" pattern would then walk the current drive from
/// its root, and a link must not lead it into the user's profile.
bool IsSafeTreeRootWide(const wchar_t *path)
{
    if (path == nullptr || path[0] == L'\0')
    {
        return false;
    }
    const HANDLE heap = GetProcessHeap();
    auto *buffers = static_cast<wchar_t *>(HeapAlloc(heap, 0, 4 * PathCapacity * sizeof(wchar_t)));
    if (buffers == nullptr)
    {
        return false;
    }
    wchar_t *full = buffers;
    wchar_t *resolved = buffers + PathCapacity;
    wchar_t *input = buffers + 2 * PathCapacity;
    wchar_t *scratch = buffers + 3 * PathCapacity;
    const DWORD fullLength = GetFullPathNameW(path, PathCapacity, full, nullptr);
    bool safe = fullLength != 0 && fullLength < PathCapacity && !IsProtected(full, input, scratch);
    if (safe)
    {
        // Where the path really leads once every link on the way is followed; a missing path has nothing to follow.
        const HANDLE file =
            CreateFileW(path, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            const DWORD resolvedLength =
                GetFinalPathNameByHandleW(file, resolved, PathCapacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            CloseHandle(file);
            safe = resolvedLength != 0 && resolvedLength < PathCapacity && !IsProtected(resolved, input, scratch);
        }
    }
    HeapFree(heap, 0, buffers);
    return safe;
}
} // namespace

namespace PlatformFile
{
FileHandle Open(const char *path, FileAccess access, FileMode mode, FileFlags flags)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return {};
    }
    DWORD desiredAccess = 0;
    switch (access)
    {
    case FileAccess::Read:
        desiredAccess = GENERIC_READ;
        break;
    case FileAccess::Write:
        desiredAccess = GENERIC_WRITE;
        break;
    case FileAccess::ReadWrite:
        desiredAccess = GENERIC_READ | GENERIC_WRITE;
        break;
    }
    if (HasAnyFlags(flags, FileFlags::Append))
    {
        desiredAccess = (desiredAccess & ~static_cast<DWORD>(GENERIC_WRITE)) | FILE_APPEND_DATA | SYNCHRONIZE;
    }
    if (HasAnyFlags(flags, FileFlags::DeleteOnClose))
    {
        desiredAccess |= DELETE;
    }

    DWORD creation = OPEN_EXISTING;
    switch (mode)
    {
    case FileMode::OpenExisting:
        creation = OPEN_EXISTING;
        break;
    case FileMode::OpenOrCreate:
        creation = OPEN_ALWAYS;
        break;
    case FileMode::CreateAlways:
        creation = CREATE_ALWAYS;
        break;
    case FileMode::CreateNew:
        creation = CREATE_NEW;
        break;
    }

    const DWORD share =
        HasAnyFlags(flags, FileFlags::Exclusive) ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    DWORD attributes = FILE_ATTRIBUTE_NORMAL;
    if (HasAnyFlags(flags, FileFlags::Async))
    {
        attributes |= FILE_FLAG_OVERLAPPED;
    }
    if (HasAnyFlags(flags, FileFlags::DeleteOnClose))
    {
        attributes |= FILE_FLAG_DELETE_ON_CLOSE;
    }
    if (HasAnyFlags(flags, FileFlags::SequentialScan))
    {
        attributes |= FILE_FLAG_SEQUENTIAL_SCAN;
    }
    if (HasAnyFlags(flags, FileFlags::RandomAccess))
    {
        attributes |= FILE_FLAG_RANDOM_ACCESS;
    }
    const HANDLE file = CreateFileW(widePath.Get(), desiredAccess, share, nullptr, creation, attributes, nullptr);
    return file == INVALID_HANDLE_VALUE ? FileHandle{} : FileHandle{reinterpret_cast<uintptr>(file)};
}

void Close(FileHandle file)
{
    if (file.IsValid())
    {
        CloseHandle(ToHandle(file));
    }
}

bool Read(FileHandle file, void *buffer, usize size, usize &bytesRead)
{
    bytesRead = 0;
    auto *target = static_cast<uint8 *>(buffer);
    while (bytesRead < size)
    {
        DWORD chunk = 0;
        if (!ReadFile(ToHandle(file), target + bytesRead, ChunkSize(size - bytesRead), &chunk, nullptr))
        {
            return false;
        }
        if (chunk == 0)
        {
            break;
        }
        bytesRead += chunk;
    }
    return true;
}

bool Write(FileHandle file, const void *data, usize size)
{
    const auto *source = static_cast<const uint8 *>(data);
    usize written = 0;
    while (written < size)
    {
        DWORD chunk = 0;
        if (!WriteFile(ToHandle(file), source + written, ChunkSize(size - written), &chunk, nullptr))
        {
            return false;
        }
        written += chunk;
    }
    return true;
}

bool ReadAt(FileHandle file, uint64 offset, void *buffer, usize size, usize &bytesRead)
{
    bytesRead = 0;
    auto *target = static_cast<uint8 *>(buffer);
    while (bytesRead < size)
    {
        const uint64 position = offset + bytesRead;
        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(position);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD chunk = 0;
        if (!ReadFile(ToHandle(file), target + bytesRead, ChunkSize(size - bytesRead), &chunk, &overlapped))
        {
            return GetLastError() == ERROR_HANDLE_EOF;
        }
        if (chunk == 0)
        {
            break;
        }
        bytesRead += chunk;
    }
    return true;
}

bool WriteAt(FileHandle file, uint64 offset, const void *data, usize size)
{
    const auto *source = static_cast<const uint8 *>(data);
    usize written = 0;
    while (written < size)
    {
        const uint64 position = offset + written;
        OVERLAPPED overlapped = {};
        overlapped.Offset = static_cast<DWORD>(position);
        overlapped.OffsetHigh = static_cast<DWORD>(position >> 32);
        DWORD chunk = 0;
        if (!WriteFile(ToHandle(file), source + written, ChunkSize(size - written), &chunk, &overlapped))
        {
            return false;
        }
        written += chunk;
    }
    return true;
}

bool Seek(FileHandle file, uint64 position)
{
    LARGE_INTEGER distance;
    distance.QuadPart = static_cast<LONGLONG>(position);
    return SetFilePointerEx(ToHandle(file), distance, nullptr, FILE_BEGIN) != 0;
}

bool GetSize(FileHandle file, uint64 &size)
{
    LARGE_INTEGER result;
    if (!GetFileSizeEx(ToHandle(file), &result))
    {
        return false;
    }
    size = static_cast<uint64>(result.QuadPart);
    return true;
}

bool SetSize(FileHandle file, uint64 size)
{
    FILE_END_OF_FILE_INFO info;
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(size);
    return SetFileInformationByHandle(ToHandle(file), FileEndOfFileInfo, &info, sizeof(info)) != 0;
}

bool Flush(FileHandle file)
{
    return FlushFileBuffers(ToHandle(file)) != 0;
}

bool Map(FileHandle file, FileAccess access, uint64 offset, usize size, MappedRegion &region)
{
    region = {};
    const bool writable = access != FileAccess::Read;
    const HANDLE mapping =
        CreateFileMappingW(ToHandle(file), nullptr, writable ? PAGE_READWRITE : PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr)
    {
        return false;
    }
    if (size == 0)
    {
        uint64 fileSize = 0;
        if (!GetSize(file, fileSize) || fileSize <= offset)
        {
            CloseHandle(mapping);
            return false;
        }
        size = static_cast<usize>(fileSize - offset);
    }
    void *data = MapViewOfFile(mapping, writable ? FILE_MAP_WRITE : FILE_MAP_READ, static_cast<DWORD>(offset >> 32),
                               static_cast<DWORD>(offset), size);
    if (data == nullptr)
    {
        CloseHandle(mapping);
        return false;
    }
    region = {.Data = data, .Size = size, .Mapping = reinterpret_cast<uintptr>(mapping)};
    return true;
}

void Unmap(MappedRegion &region)
{
    if (region.Data != nullptr)
    {
        UnmapViewOfFile(region.Data);
        CloseHandle(reinterpret_cast<HANDLE>(region.Mapping));
    }
    region = {};
}

bool GetInfo(const char *path, FileInfo &info)
{
    const Microsoft::WidePath widePath(path);
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (widePath.Get() == nullptr || !GetFileAttributesExW(widePath.Get(), GetFileExInfoStandard, &data))
    {
        return false;
    }
    info = ToFileInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
    return true;
}

bool FileExists(const char *path)
{
    FileInfo info;
    return GetInfo(path, info) && !info.IsDirectory;
}

bool DirectoryExists(const char *path)
{
    FileInfo info;
    return GetInfo(path, info) && info.IsDirectory;
}

bool Delete(const char *path)
{
    const Microsoft::WidePath widePath(path);
    return widePath.Get() != nullptr && DeleteFileW(widePath.Get()) != 0;
}

bool Move(const char *from, const char *to, bool overwrite)
{
    const Microsoft::WidePath source(from);
    const Microsoft::WidePath target(to);
    const DWORD flags = overwrite ? MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING : MOVEFILE_COPY_ALLOWED;
    return source.Get() != nullptr && target.Get() != nullptr && MoveFileExW(source.Get(), target.Get(), flags) != 0;
}

bool Copy(const char *from, const char *to, bool overwrite)
{
    const Microsoft::WidePath source(from);
    const Microsoft::WidePath target(to);
    return source.Get() != nullptr && target.Get() != nullptr &&
           CopyFileExW(source.Get(), target.Get(), nullptr, nullptr, nullptr,
                       overwrite ? 0 : COPY_FILE_FAIL_IF_EXISTS) != 0;
}

bool SetReadOnly(const char *path, bool readOnly)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(widePath.Get());
    if (attributes == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }
    const DWORD updated =
        readOnly ? attributes | FILE_ATTRIBUTE_READONLY : attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY);
    return SetFileAttributesW(widePath.Get(), updated) != 0;
}

bool SetModifiedTime(const char *path, int64 time)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    const HANDLE file =
        CreateFileW(widePath.Get(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    const FILETIME modified = Microsoft::TicksToFileTime(time);
    const bool succeeded = SetFileTime(file, nullptr, nullptr, &modified) != 0;
    CloseHandle(file);
    return succeeded;
}

bool CreateDirectory(const char *path)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    if (CreateDirectoryW(widePath.Get(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        return DirectoryExists(path);
    }
    if (GetLastError() != ERROR_PATH_NOT_FOUND)
    {
        return false;
    }

    usize length = 0;
    while (path[length] != '\0')
    {
        ++length;
    }
    while (length > 0 && path[length - 1] != '/' && path[length - 1] != '\\')
    {
        --length;
    }
    while (length > 0 && (path[length - 1] == '/' || path[length - 1] == '\\'))
    {
        --length;
    }
    if (length == 0)
    {
        return false;
    }
    auto *parent = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, length + 1));
    if (parent == nullptr)
    {
        return false;
    }
    for (usize i = 0; i < length; ++i)
    {
        parent[i] = path[i];
    }
    parent[length] = '\0';
    const bool parentCreated = CreateDirectory(parent);
    HeapFree(GetProcessHeap(), 0, parent);
    return parentCreated && (CreateDirectoryW(widePath.Get(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

bool DeleteDirectory(const char *path, bool recursive)
{
    // A recursive delete takes only a full path: "", "\Folder" and "C:Folder" are resolved against the current
    // drive or directory, so a path that a failed lookup left empty or cut short would empty them.
    if (recursive && !IsFullPath(path))
    {
        MARLIN_ASSERT(!"DeleteDirectory needs a full path to delete recursively");
        return false;
    }
    const Microsoft::WidePath widePath(path, recursive);
    if (widePath.Get() == nullptr)
    {
        return false;
    }
    if (!recursive)
    {
        return RemoveDirectoryW(widePath.Get()) != 0;
    }
    const DWORD attributes = GetFileAttributesW(widePath.Get());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return false;
    }
    // A link is removed, never followed: walking it would empty its target.
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        return RemoveDirectoryW(widePath.Get()) != 0;
    }
    if (!IsSafeTreeRootWide(widePath.Get()))
    {
        MARLIN_ASSERT(!"DeleteDirectory refused a top-level, user, current or executable-holding folder");
        return false;
    }
    return DeleteTree(widePath.Get());
}

bool IsSafeTreeRoot(const char *path)
{
    const Microsoft::WidePath widePath(path, true);
    return widePath.Get() != nullptr && IsSafeTreeRootWide(widePath.Get());
}

usize GetFullPath(const char *path, char *buffer, usize capacity)
{
    const Microsoft::WidePath widePath(path);
    if (widePath.Get() == nullptr)
    {
        return 0;
    }
    const DWORD wideCapacity = GetFullPathNameW(widePath.Get(), 0, nullptr, nullptr);
    auto *full = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, wideCapacity * sizeof(wchar_t)));
    if (wideCapacity == 0 || full == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, full);
        return 0;
    }
    GetFullPathNameW(widePath.Get(), wideCapacity, full, nullptr);
    const usize length = Microsoft::WideToUtf8Path(full, buffer, capacity);
    HeapFree(GetProcessHeap(), 0, full);
    return length;
}

usize GetCurrentDirectory(char *buffer, usize capacity)
{
    const DWORD wideCapacity = GetCurrentDirectoryW(0, nullptr);
    auto *current = static_cast<wchar_t *>(HeapAlloc(GetProcessHeap(), 0, wideCapacity * sizeof(wchar_t)));
    if (wideCapacity == 0 || current == nullptr)
    {
        HeapFree(GetProcessHeap(), 0, current);
        return 0;
    }
    GetCurrentDirectoryW(wideCapacity, current);
    const usize length = Microsoft::WideToUtf8Path(current, buffer, capacity);
    HeapFree(GetProcessHeap(), 0, current);
    return length;
}
} // namespace PlatformFile

DirectoryIterator::~DirectoryIterator()
{
    Close();
}

bool DirectoryIterator::Open(const char *path)
{
    Close();
    usize length = 0;
    while (path[length] != '\0')
    {
        ++length;
    }
    auto *pattern = static_cast<char *>(HeapAlloc(GetProcessHeap(), 0, length + 3));
    if (pattern == nullptr)
    {
        return false;
    }
    for (usize i = 0; i < length; ++i)
    {
        pattern[i] = path[i];
    }
    pattern[length] = '/';
    pattern[length + 1] = '*';
    pattern[length + 2] = '\0';
    const Microsoft::WidePath widePattern(pattern);
    HeapFree(GetProcessHeap(), 0, pattern);
    if (widePattern.Get() == nullptr)
    {
        return false;
    }

    WIN32_FIND_DATAW data;
    const HANDLE find = FindFirstFileExW(widePattern.Get(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                         FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
    {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    _handle = find;
    if (IsDotEntry(data.cFileName))
    {
        Advance();
        return true;
    }
    Microsoft::WideToUtf8(data.cFileName, _entry.Name, DirectoryEntry::MaxNameLength);
    _entry.Info = ToFileInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
    _hasEntry = true;
    return true;
}

bool DirectoryIterator::Next(DirectoryEntry &entry)
{
    if (!_hasEntry)
    {
        return false;
    }
    entry = _entry;
    Advance();
    return true;
}

bool DirectoryIterator::Advance()
{
    _hasEntry = false;
    if (_handle == nullptr)
    {
        return false;
    }
    WIN32_FIND_DATAW data;
    while (FindNextFileW(_handle, &data))
    {
        if (IsDotEntry(data.cFileName))
        {
            continue;
        }
        Microsoft::WideToUtf8(data.cFileName, _entry.Name, DirectoryEntry::MaxNameLength);
        _entry.Info = ToFileInfo(data.dwFileAttributes, data.nFileSizeHigh, data.nFileSizeLow, data.ftLastWriteTime);
        _hasEntry = true;
        return true;
    }
    return false;
}

void DirectoryIterator::Close()
{
    if (_handle != nullptr)
    {
        FindClose(_handle);
        _handle = nullptr;
    }
    _hasEntry = false;
}
} // namespace Marlin
