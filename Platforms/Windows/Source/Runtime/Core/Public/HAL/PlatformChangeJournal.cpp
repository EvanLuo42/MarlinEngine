#include "HAL/PlatformChangeJournal.h"

#include "HAL/MicrosoftHeaders.h"
#include "Memory/MemoryOps.h"

#include <winioctl.h>

namespace Marlin
{
namespace
{
constexpr DWORD ReadBufferSize = 256 * 1024;

HANDLE Handle(uintptr handle)
{
    return reinterpret_cast<HANDLE>(handle);
}

JournalReason ToReasons(DWORD reason)
{
    JournalReason reasons = JournalReason::None;
    if ((reason & USN_REASON_FILE_CREATE) != 0)
    {
        reasons |= JournalReason::Created;
    }
    if ((reason & USN_REASON_FILE_DELETE) != 0)
    {
        reasons |= JournalReason::Deleted;
    }
    if ((reason & (USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
                   USN_REASON_NAMED_DATA_OVERWRITE | USN_REASON_NAMED_DATA_EXTEND | USN_REASON_NAMED_DATA_TRUNCATION |
                   USN_REASON_STREAM_CHANGE)) != 0)
    {
        reasons |= JournalReason::Modified;
    }
    if ((reason & USN_REASON_RENAME_OLD_NAME) != 0)
    {
        reasons |= JournalReason::RenamedFrom;
    }
    if ((reason & USN_REASON_RENAME_NEW_NAME) != 0)
    {
        reasons |= JournalReason::RenamedTo;
    }
    if ((reason & (USN_REASON_BASIC_INFO_CHANGE | USN_REASON_SECURITY_CHANGE | USN_REASON_EA_CHANGE |
                   USN_REASON_HARD_LINK_CHANGE)) != 0)
    {
        reasons |= JournalReason::Metadata;
    }
    return reasons;
}

bool Query(HANDLE handle, USN_JOURNAL_DATA_V1 &data)
{
    DWORD bytes = 0;
    return DeviceIoControl(handle, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data), &bytes, nullptr) != 0;
}
} // namespace

ChangeJournal::~ChangeJournal()
{
    Close();
}

bool ChangeJournal::Open(const char *directory)
{
    Close();
    wchar_t path[MAX_PATH * 2];
    const usize length = Microsoft::Utf8ToWide(directory, path, MAX_PATH * 2);
    if (length == 0 || length >= MAX_PATH * 2)
    {
        return false;
    }
    const HANDLE handle =
        CreateFileW(path, FILE_READ_ATTRIBUTES | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    USN_JOURNAL_DATA_V1 data = {};
    if (!Query(handle, data))
    {
        CloseHandle(handle);
        return false;
    }
    _handle = reinterpret_cast<uintptr>(handle);
    return true;
}

void ChangeJournal::Close()
{
    if (_handle != 0)
    {
        CloseHandle(Handle(_handle));
        _handle = 0;
    }
}

bool ChangeJournal::GetEnd(JournalCursor &cursor) const
{
    USN_JOURNAL_DATA_V1 data = {};
    if (_handle == 0 || !Query(Handle(_handle), data))
    {
        return false;
    }
    cursor.JournalId = data.UsnJournalID;
    cursor.Position = data.NextUsn;
    return true;
}

bool ChangeJournal::Read(JournalCursor &cursor, void (*visitor)(const JournalChange &change, void *context),
                         void *context, uint32 maxChanges)
{
    USN_JOURNAL_DATA_V1 data = {};
    if (_handle == 0 || !Query(Handle(_handle), data))
    {
        return false;
    }
    if (data.UsnJournalID != cursor.JournalId || cursor.Position < data.FirstUsn || cursor.Position > data.NextUsn)
    {
        return false;
    }
    alignas(8) static thread_local uint8 buffer[ReadBufferSize];
    JournalChange change;
    uint32 visited = 0;
    while (cursor.Position < data.NextUsn && visited < maxChanges)
    {
        READ_USN_JOURNAL_DATA_V1 read = {};
        read.StartUsn = cursor.Position;
        read.ReasonMask = 0xFFFFFFFF;
        read.UsnJournalID = cursor.JournalId;
        read.MinMajorVersion = 2;
        read.MaxMajorVersion = 2;
        DWORD bytes = 0;
        if (!DeviceIoControl(Handle(_handle), FSCTL_READ_UNPRIVILEGED_USN_JOURNAL, &read, sizeof(read), buffer,
                             ReadBufferSize, &bytes, nullptr))
        {
            return GetLastError() != ERROR_JOURNAL_ENTRY_DELETED &&
                   GetLastError() != ERROR_JOURNAL_DELETE_IN_PROGRESS && GetLastError() != ERROR_INVALID_PARAMETER;
        }
        if (bytes <= sizeof(USN))
        {
            break;
        }
        USN next = 0;
        MemCopy(&next, buffer, sizeof(next));
        DWORD offset = sizeof(USN);
        while (offset + sizeof(USN_RECORD_V2) <= bytes)
        {
            const auto *record = reinterpret_cast<const USN_RECORD_V2 *>(buffer + offset);
            if (record->RecordLength == 0 || offset + record->RecordLength > bytes)
            {
                break;
            }
            change.FileId = record->FileReferenceNumber;
            change.ParentId = record->ParentFileReferenceNumber;
            change.Reasons = ToReasons(record->Reason);
            change.IsDirectory = (record->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            wchar_t name[JournalChange::MaxNameLength];
            const usize characters = record->FileNameLength / sizeof(wchar_t);
            if (characters < JournalChange::MaxNameLength)
            {
                MemCopy(name, reinterpret_cast<const uint8 *>(record) + record->FileNameOffset,
                        characters * sizeof(wchar_t));
                name[characters] = L'\0';
                if (Microsoft::WideToUtf8(name, change.Name, JournalChange::MaxNameLength) <
                    JournalChange::MaxNameLength)
                {
                    visitor(change, context);
                    ++visited;
                }
            }
            offset += record->RecordLength;
        }
        cursor.Position = next;
    }
    return true;
}

usize ChangeJournal::GetPath(uint64 fileId, char *buffer, usize capacity) const
{
    if (_handle == 0)
    {
        return 0;
    }
    FILE_ID_DESCRIPTOR descriptor = {};
    descriptor.dwSize = sizeof(descriptor);
    descriptor.Type = FileIdType;
    descriptor.FileId.QuadPart = static_cast<LONGLONG>(fileId);
    const HANDLE file =
        OpenFileById(Handle(_handle), &descriptor, FILE_READ_ATTRIBUTES,
                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, FILE_FLAG_BACKUP_SEMANTICS);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0;
    }
    wchar_t path[MAX_PATH * 4];
    const DWORD length = GetFinalPathNameByHandleW(file, path, MAX_PATH * 4, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (length == 0 || length >= MAX_PATH * 4)
    {
        return 0;
    }
    return Microsoft::WideToUtf8Path(path, buffer, capacity);
}
} // namespace Marlin
