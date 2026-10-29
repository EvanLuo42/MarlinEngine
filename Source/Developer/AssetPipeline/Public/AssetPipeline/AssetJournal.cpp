#include "AssetPipeline/AssetJournal.h"

#include "AssetPipeline/AssetScanner.h"

#include "Containers/HashSet.h"
#include "HAL/PlatformFile.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
bool SameText(StringView a, StringView b)
{
    return MARLIN_PLATFORM_WINDOWS ? a.EqualsIgnoreCase(b) : a == b;
}

/// The part of *path* below *root*, or false if it is not below it.
bool Below(StringView path, StringView root, StringView &relative)
{
    if (path.Length() < root.Length() || !SameText(path.Substring(0, root.Length()), root))
    {
        return false;
    }
    if (path.Length() == root.Length())
    {
        relative = {};
        return true;
    }
    if (path[root.Length()] != '/')
    {
        return false;
    }
    relative = path.Substring(root.Length() + 1);
    return true;
}

/// The volume of a native path: its drive on Windows, the root elsewhere.
StringView VolumeOf(StringView path)
{
    const usize colon = path.IndexOf(':');
    return colon != StringView::NotFound && colon < 3 ? path.Substring(0, colon + 1) : StringView("/");
}

} // namespace

AssetJournal::~AssetJournal() = default;

bool AssetJournal::Open(Span<const String> folders)
{
    _volumes.Clear();
    for (const String &folder : folders)
    {
        // Every layer of the folder, such as its platform layers, may sit on its own volume.
        const List<String> physicalFolders = GetPhysicalFolders(folder.AsView());
        if (physicalFolders.IsEmpty())
        {
            _volumes.Clear();
            return false;
        }
        for (const String &physical : physicalFolders)
        {
            const StringView key = VolumeOf(physical.AsView());
            Volume *volume = nullptr;
            for (UniquePtr<Volume> &candidate : _volumes)
            {
                if (SameText(candidate->Key.AsView(), key))
                {
                    volume = candidate.Get();
                }
            }
            if (volume == nullptr)
            {
                auto created = MakeUnique<Volume>();
                created->Key = key;
                created->Journal = MakeUnique<ChangeJournal>();
                if (!created->Journal->Open(physical.CStr()))
                {
                    _volumes.Clear();
                    return false;
                }
                volume = created.Get();
                _volumes.Add(Move(created));
            }
            volume->Roots.Add({physical, folder});
        }
    }
    MarkEnd();
    return !_volumes.IsEmpty();
}

void AssetJournal::MarkEnd()
{
    for (UniquePtr<Volume> &volume : _volumes)
    {
        (void)volume->Journal->GetEnd(volume->Cursor);
    }
}

bool AssetJournal::Save(StringView path) const
{
    StringBuilder<1024> text;
    for (const UniquePtr<Volume> &volume : _volumes)
    {
        char digits[24];
        text.Append(volume->Key.AsView()).Append(' ');
        text.Append(StringView(digits, FormatUnsigned(volume->Cursor.JournalId, digits))).Append(' ');
        text.Append(StringView(digits, FormatSigned(volume->Cursor.Position, digits))).Append('\n');
    }
    const String target(path);
    const FileHandle file = PlatformFile::Open(target.CStr(), FileAccess::Write, FileMode::CreateAlways);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

bool AssetJournal::Load(StringView path)
{
    const String source(path);
    const FileHandle file = PlatformFile::Open(source.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    char buffer[4096];
    usize read = 0;
    const bool ok = PlatformFile::Read(file, buffer, sizeof(buffer), read);
    PlatformFile::Close(file);
    if (!ok)
    {
        return false;
    }
    usize matched = 0;
    for (const StringView line : StringView(buffer, read).Split('\n'))
    {
        StringView key;
        StringView rest;
        StringView id;
        StringView position;
        JournalCursor cursor;
        if (!line.Trim().SplitFirst(' ', key, rest) || !rest.SplitFirst(' ', id, position) ||
            !TryParse(id, cursor.JournalId) || !TryParse(position.Trim(), cursor.Position))
        {
            continue;
        }
        for (UniquePtr<Volume> &volume : _volumes)
        {
            if (SameText(volume->Key.AsView(), key))
            {
                volume->Cursor = cursor;
                ++matched;
            }
        }
    }
    return matched == _volumes.Count();
}

const String *AssetJournal::ResolveDirectory(Volume &volume, uint64 id)
{
    String *directory = volume.Directories.Find(id);
    if (directory == nullptr)
    {
        char buffer[2048];
        const usize length = volume.Journal->GetPath(id, buffer, sizeof(buffer));
        String resolved;
        if (length != 0 && length < sizeof(buffer))
        {
            const StringView native(buffer, length);
            for (const Root &root : volume.Roots)
            {
                StringView relative;
                if (Below(native, root.Physical.AsView(), relative))
                {
                    resolved = native;
                    break;
                }
            }
        }
        directory = &volume.Directories.FindOrAdd(id);
        *directory = Move(resolved);
    }
    return directory->IsEmpty() ? nullptr : directory;
}

bool AssetJournal::ToVirtual(const Volume &volume, StringView native, String &path) const
{
    for (const Root &root : volume.Roots)
    {
        StringView relative;
        if (Below(native, root.Physical.AsView(), relative))
        {
            StringBuilder<1024> text(root.Virtual.AsView());
            if (!relative.IsEmpty())
            {
                text.Append('/').Append(relative);
            }
            path = text.AsView();
            return true;
        }
    }
    return false;
}

bool AssetJournal::Resolve(Volume &volume, const JournalChange &change, String &path)
{
    const String *directory = ResolveDirectory(volume, change.ParentId);
    if (directory == nullptr)
    {
        return false;
    }
    if (change.Name[0] != '\0')
    {
        StringBuilder<1024> native(directory->AsView());
        native.Append('/').Append(StringView(change.Name));
        return ToVirtual(volume, native.AsView(), path);
    }
    char buffer[2048];
    const usize length = volume.Journal->GetPath(change.FileId, buffer, sizeof(buffer));
    if (length != 0 && length < sizeof(buffer) && ToVirtual(volume, StringView(buffer, length), path))
    {
        return true;
    }
    return ToVirtual(volume, directory->AsView(), path);
}

bool AssetJournal::Poll(List<String> &paths)
{
    HashSet<String> seen;
    for (UniquePtr<Volume> &volume : _volumes)
    {
        PollContext context{this, volume.Get(), &paths, &seen};
        if (!volume->Journal->Read(volume->Cursor, &Visit, &context))
        {
            return false;
        }
    }
    return true;
}

void AssetJournal::Visit(const JournalChange &change, void *state)
{
    auto &context = *static_cast<PollContext *>(state);
    String path;
    if (!context.Journal->Resolve(*context.Target, change, path))
    {
        return;
    }
    if (change.IsDirectory && HasAnyFlags(change.Reasons, JournalReason::Deleted | JournalReason::RenamedFrom))
    {
        context.Target->Directories.Remove(change.FileId);
    }
    if (context.Seen->Add(path))
    {
        context.Paths->Add(Move(path));
    }
}
} // namespace Marlin
