#include "Remote/RemoteDiscovery.h"

#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Reflection/TypeOf.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin::RemoteDiscovery
{
namespace
{
String FileOf(uint32 processId)
{
    StringBuilder<512> path(GetDirectory().AsView());
    char digits[16];
    path.Append('/').Append(StringView(digits, FormatUnsigned(processId, digits))).Append(".mtext");
    return path.ToString();
}

bool ReadFile(const String &path, String &text)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    bool ok = PlatformFile::GetSize(file, size) && size < (uint64{1} << 20);
    if (ok)
    {
        text.Resize(static_cast<usize>(size));
        usize read = 0;
        ok = PlatformFile::Read(file, text.Data(), static_cast<usize>(size), read) && read == size;
    }
    PlatformFile::Close(file);
    return ok;
}
} // namespace

String GetDirectory()
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(KnownFolder::Temp, buffer, sizeof(buffer));
    StringView folder(buffer, length < sizeof(buffer) ? length : 0);
    while (!folder.IsEmpty() && (folder.Last() == '/' || folder.Last() == '\\'))
    {
        folder = folder.Substring(0, folder.Length() - 1);
    }
    StringBuilder<1024> path(folder);
    path.Append("/Marlin/Processes");
    return path.ToString();
}

bool Register(const RemoteProcessInfo &info)
{
    const String directory = GetDirectory();
    StringView parent;
    StringView name;
    if (directory.AsView().SplitLast('/', parent, name))
    {
        (void)PlatformFile::CreateDirectory(String(parent).CStr());
    }
    (void)PlatformFile::CreateDirectory(directory.CStr());
    StringBuilder<1024> text("!Version \"1.0\"\n\n");
    MarlinTextSerializer::WriteObject(text, TypeOf<RemoteProcessInfo>(), &info);
    const String path = FileOf(info.ProcessId);
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

void Unregister(uint32 processId)
{
    (void)PlatformFile::Delete(FileOf(processId).CStr());
}

void List(::Marlin::List<RemoteProcessInfo> &processes)
{
    const String directory = GetDirectory();
    DirectoryIterator iterator;
    if (!iterator.Open(directory.CStr()))
    {
        return;
    }
    ::Marlin::List<String> files;
    DirectoryEntry entry;
    while (iterator.Next(entry))
    {
        if (!entry.Info.IsDirectory && StringView(entry.Name).EndsWith(".mtext"))
        {
            StringBuilder<512> path(directory.AsView());
            path.Append('/').Append(entry.Name);
            files.Add(path.ToString());
        }
    }
    for (const String &path : files)
    {
        String text;
        MarlinTextDocument document;
        MarlinTextDiagnostics diagnostics;
        RemoteProcessInfo info;
        const bool parsed =
            ReadFile(path, text) && document.Parse(text.AsView(), diagnostics) && document.GetObjectCount() > 0;
        const MarlinTextObject *object = parsed ? document.GetObject(0, diagnostics) : nullptr;
        if (object != nullptr &&
            MarlinTextSerializer::ReadObject(*object, TypeOf<RemoteProcessInfo>(), &info, diagnostics) &&
            PlatformProcess::IsRunning(info.ProcessId))
        {
            processes.Add(Move(info));
            continue;
        }
        (void)PlatformFile::Delete(path.CStr());
    }
}
} // namespace Marlin::RemoteDiscovery
