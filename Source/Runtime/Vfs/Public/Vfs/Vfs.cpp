#include "Vfs/Vfs.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashSet.h"
#include "HAL/PlatformSync.h"
#include "Memory/MemoryOps.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"

namespace Marlin
{
namespace
{
struct MountEntry
{
    MountId Id = 0;
    String Point;
    int32 Priority = 0;
    UniquePtr<IFileMount> Mount;
};

struct MountTable
{
    ReadWriteLock Lock;
    /// Longest point first, then highest priority, then most recent.
    List<MountEntry> Mounts;
    MountId NextId = 1;

    /// Built on first use and never destroyed, so mounts outlive static destruction.
    static MountTable &Get()
    {
        alignas(MountTable) static uint8 storage[sizeof(MountTable)];
        static MountTable *table = ConstructAt(reinterpret_cast<MountTable *>(storage));
        return *table;
    }
};

/// The path below *point*, or false if *path* is not inside it.
bool Relative(StringView path, StringView point, StringView &relative)
{
    if (point == "/")
    {
        relative = path.RemoveStart(1);
        return true;
    }
    if (!path.StartsWith(point) || (path.Length() > point.Length() && path[point.Length()] != '/'))
    {
        return false;
    }
    relative = path.RemoveStart(point.Length());
    relative = relative.StartsWith('/') ? relative.RemoveStart(1) : relative;
    return true;
}

/// Calls *visitor* with each mount that contains *path*, in search order, until it returns true. A mount that
/// deletes the path, as a patch package does, hides the mounts below it.
template <typename TVisitor> bool ForEachCandidate(StringView path, TVisitor visitor)
{
    StringBuilder<512> normal;
    if (!Vfs::Normalize(path, normal))
    {
        return false;
    }
    MountTable &table = MountTable::Get();
    const SharedScopeLock lock(table.Lock);
    // The longest point that contains the path owns it; only the mounts stacked on that point are searched.
    const String *owner = nullptr;
    for (MountEntry &entry : table.Mounts)
    {
        StringView relative;
        if ((owner != nullptr && entry.Point != *owner) || !Relative(normal.AsView(), entry.Point.AsView(), relative))
        {
            continue;
        }
        owner = &entry.Point;
        if (visitor(entry, relative))
        {
            return true;
        }
        if (entry.Mount->IsDeleted(relative))
        {
            return false;
        }
    }
    return false;
}

void AppendPhysical(StringWriter &output, StringView root, StringView path)
{
    output.Append(root);
    if (!path.IsEmpty())
    {
        output.Append('/').Append(path);
    }
}

bool CreateParents(IFileMount &mount, StringView path)
{
    StringView parent;
    StringView name;
    return !path.SplitLast('/', parent, name) || mount.CreateDirectory(parent);
}

/// Calls *visitor* with the mount of *layer*, if it is one of the layers of *path*.
template <typename TVisitor> bool WithLayer(MountId layer, StringView path, TVisitor visitor)
{
    return ForEachCandidate(
        path, [&](MountEntry &entry, StringView relative) { return entry.Id == layer && visitor(entry, relative); });
}

/// Reads all of *file*, which may be invalid, and closes it.
bool ReadAndClose(FileHandle file, List<uint8> &bytes)
{
    uint64 size = 0;
    usize read = 0;
    const bool ok = file.IsValid() && PlatformFile::GetSize(file, size) &&
                    (bytes.ResizeUninitialized(static_cast<usize>(size)), true) &&
                    PlatformFile::Read(file, bytes.Data(), bytes.Count(), read) && read == bytes.Count();
    if (file.IsValid())
    {
        PlatformFile::Close(file);
    }
    return ok;
}

/// UTF-8 text without its byte order mark.
void ToText(const List<uint8> &bytes, String &text)
{
    usize start = 0;
    if (bytes.Count() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
    {
        start = 3;
    }
    text = StringView(reinterpret_cast<const char *>(bytes.Data()) + start, bytes.Count() - start);
}

bool WriteAndClose(FileHandle file, StringView text)
{
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

/// Reads what a mount resolved a file to: a native handle, or else a region for mounts without handles, such as
/// packages, which an asynchronous read fetches. Called without the mount table locked.
bool ReadResolved(FileHandle file, const FileRegion &region, List<uint8> &bytes)
{
    if (file.IsValid() || !region.IsValid())
    {
        return ReadAndClose(file, bytes);
    }
    const AsyncReadRequest request = Vfs::ReadAsync(region, IoPriority::Critical);
    if (!request.Wait() || !request.Succeeded())
    {
        return false;
    }
    bytes.Clear();
    bytes.AddRange(request.GetData());
    return true;
}

/// Resolves *path* in *mount* for ReadResolved.
bool Resolve(IFileMount &mount, StringView path, FileHandle &file, FileRegion &region)
{
    file = mount.Open(path, FileAccess::Read, FileMode::OpenExisting, FileFlags::None);
    return file.IsValid() || mount.OpenRegion(path, region);
}
} // namespace

SharedPtr<const CachedRegionBlock> RegionBlockCache::Find(uint64 index)
{
    const ScopeLock lock(_lock);
    for (Entry &entry : _entries)
    {
        if (entry.Index == index)
        {
            entry.LastUse = ++_clock;
            return entry.Block;
        }
    }
    return {};
}

void RegionBlockCache::Add(uint64 index, SharedPtr<const CachedRegionBlock> block)
{
    const ScopeLock lock(_lock);
    Entry *target = nullptr;
    for (Entry &entry : _entries)
    {
        if (entry.Index == index)
        {
            target = &entry;
            break;
        }
        if (target == nullptr || entry.LastUse < target->LastUse)
        {
            target = &entry;
        }
    }
    if (_entries.Count() < _capacity && (target == nullptr || target->Index != index))
    {
        target = &_entries.AddDefaulted();
    }
    if (target == nullptr)
    {
        return;
    }
    target->Index = index;
    target->LastUse = ++_clock;
    target->Block = Move(block);
}

SharedFile::~SharedFile()
{
    if (File.IsValid())
    {
        PlatformFile::Close(File);
    }
}

bool IFileMount::Delete(StringView /*path*/)
{
    return false;
}

bool IFileMount::CreateDirectory(StringView /*path*/)
{
    return false;
}

bool IFileMount::GetPhysicalPath(StringView /*path*/, StringWriter & /*output*/) const
{
    return false;
}

bool IFileMount::OpenRegion(StringView /*path*/, FileRegion & /*region*/)
{
    return false;
}

DirectoryMount::DirectoryMount(StringView root, bool writable) : _root(root), _writable(writable)
{
    _root.Replace('\\', '/');
    while (_root.EndsWith("/") && _root.Length() > 1)
    {
        _root.Remove(_root.Length() - 1);
    }
}

FileHandle DirectoryMount::Open(StringView path, FileAccess access, FileMode mode, FileFlags flags)
{
    if (access != FileAccess::Read && !_writable)
    {
        return {};
    }
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, path);
    return PlatformFile::Open(physical.CStr(), access, mode, flags);
}

bool DirectoryMount::GetInfo(StringView path, FileInfo &info)
{
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, path);
    return PlatformFile::GetInfo(physical.CStr(), info);
}

void DirectoryMount::Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor)
{
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, directory);
    DirectoryIterator iterator;
    if (!iterator.Open(physical.CStr()))
    {
        return;
    }
    DirectoryEntry entry;
    while (iterator.Next(entry))
    {
        visitor(StringView(entry.Name), entry.Info);
    }
}

bool DirectoryMount::Delete(StringView path)
{
    // The mount's own root is never deleted: it may be any folder, such as the one -UserDir names.
    if (!_writable || path.IsEmpty())
    {
        return false;
    }
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, path);
    FileInfo info;
    if (!PlatformFile::GetInfo(physical.CStr(), info))
    {
        return false;
    }
    return info.IsDirectory ? PlatformFile::DeleteDirectory(physical.CStr(), true)
                            : PlatformFile::Delete(physical.CStr());
}

bool DirectoryMount::CreateDirectory(StringView path)
{
    if (!_writable)
    {
        return false;
    }
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, path);
    return PlatformFile::CreateDirectory(physical.CStr());
}

bool DirectoryMount::GetPhysicalPath(StringView path, StringWriter &output) const
{
    AppendPhysical(output, _root, path);
    return true;
}

bool DirectoryMount::OpenRegion(StringView path, FileRegion &region)
{
    if (!_writable)
    {
        const ScopeLock lock(_filesLock);
        if (const SharedPtr<SharedFile> *cached = _files.Find(path))
        {
            region = {*cached, 0, (*cached)->Size, {}, {}};
            return true;
        }
    }
    StringBuilder<512> physical;
    AppendPhysical(physical, _root, path);
    const FileHandle file =
        PlatformFile::Open(physical.CStr(), FileAccess::Read, FileMode::OpenExisting, FileFlags::Async);
    uint64 size = 0;
    if (!file.IsValid() || !PlatformFile::GetSize(file, size))
    {
        if (file.IsValid())
        {
            PlatformFile::Close(file);
        }
        return false;
    }
    SharedPtr<SharedFile> shared(new SharedFile(file, size));
    if (!_writable)
    {
        const ScopeLock lock(_filesLock);
        if (_files.Count() >= MaxOpenFiles)
        {
            _files.Clear();
        }
        _files.Set(String(path), shared);
    }
    region = {Move(shared), 0, size, {}, {}};
    return true;
}

namespace Vfs
{
MountId Mount(StringView point, UniquePtr<IFileMount> mount, int32 priority)
{
    StringBuilder<256> normal;
    if (!Normalize(point, normal))
    {
        return 0;
    }
    MountTable &table = MountTable::Get();
    const ScopeLock lock(table.Lock);
    const MountId id = table.NextId++;
    usize index = 0;
    while (index < table.Mounts.Count())
    {
        const MountEntry &entry = table.Mounts[index];
        const bool after = entry.Point.Length() > normal.Length() ||
                           (entry.Point.Length() == normal.Length() && entry.Priority > priority);
        if (!after)
        {
            break;
        }
        ++index;
    }
    table.Mounts.Insert(index, MountEntry{id, String(normal.AsView()), priority, Move(mount)});
    return id;
}

bool Unmount(MountId id)
{
    MountTable &table = MountTable::Get();
    const ScopeLock lock(table.Lock);
    return table.Mounts.RemoveAll([id](const MountEntry &entry) { return entry.Id == id; }) != 0;
}

void UnmountAll()
{
    MountTable &table = MountTable::Get();
    const ScopeLock lock(table.Lock);
    table.Mounts.Clear();
}

bool IsMounted(StringView point)
{
    StringBuilder<256> normal;
    if (!Normalize(point, normal))
    {
        return false;
    }
    MountTable &table = MountTable::Get();
    const SharedScopeLock lock(table.Lock);
    for (const MountEntry &entry : table.Mounts)
    {
        if (entry.Point == normal.AsView())
        {
            return true;
        }
    }
    return false;
}

FileHandle Open(StringView path, FileAccess access, FileMode mode, FileFlags flags)
{
    FileHandle result;
    if (access == FileAccess::Read || mode == FileMode::OpenExisting)
    {
        // Trying the open is cheaper than asking each mount whether it has the file first.
        ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
            result = entry.Mount->Open(relative, access, FileMode::OpenExisting, flags);
            return result.IsValid();
        });
        if (result.IsValid() || access == FileAccess::Read)
        {
            return result;
        }
    }
    ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        if (!entry.Mount->IsWritable())
        {
            return false;
        }
        (void)CreateParents(*entry.Mount, relative);
        result = entry.Mount->Open(relative, access, mode, flags);
        return true;
    });
    return result;
}

bool GetInfo(StringView path, FileInfo &info)
{
    return ForEachCandidate(
        path, [&](MountEntry &entry, StringView relative) { return entry.Mount->GetInfo(relative, info); });
}

bool Exists(StringView path)
{
    FileInfo info;
    return GetInfo(path, info);
}

bool ReadAll(StringView path, List<uint8> &bytes)
{
    // The first mount that has the file serves it, whether or not it has native handles.
    FileHandle file;
    FileRegion region;
    ForEachCandidate(
        path, [&](MountEntry &entry, StringView relative) { return Resolve(*entry.Mount, relative, file, region); });
    return ReadResolved(file, region, bytes);
}

bool ReadAll(StringView path, String &text)
{
    List<uint8> bytes;
    if (!ReadAll(path, bytes))
    {
        return false;
    }
    ToText(bytes, text);
    return true;
}

bool WriteAll(StringView path, StringView text)
{
    return WriteAndClose(Open(path, FileAccess::Write, FileMode::CreateAlways), text);
}

bool Delete(StringView path)
{
    return ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        FileInfo info;
        return entry.Mount->IsWritable() && entry.Mount->GetInfo(relative, info) && entry.Mount->Delete(relative);
    });
}

bool CreateDirectory(StringView path)
{
    return ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        return entry.Mount->IsWritable() && entry.Mount->CreateDirectory(relative);
    });
}

void Enumerate(StringView directory, FunctionRef<void(StringView name, const FileInfo &info)> visitor)
{
    HashSet<String> seen;
    // The mounts already visited: an entry one of them deletes, as a patch package does, is hidden.
    List<const IFileMount *> above;
    ForEachCandidate(directory, [&](MountEntry &entry, StringView relative) {
        entry.Mount->Enumerate(relative, [&](StringView name, const FileInfo &info) {
            if (!above.IsEmpty())
            {
                const String child = relative.IsEmpty() ? String(name) : Format("{0}/{1}", relative, name);
                for (const IFileMount *mount : above)
                {
                    if (mount->IsDeleted(child.AsView()))
                    {
                        return;
                    }
                }
            }
            if (seen.Add(String(name)))
            {
                visitor(name, info);
            }
        });
        above.Add(entry.Mount.Get());
        return false;
    });
}

void ReadEachLayer(StringView path, FunctionRef<void(StringView text, StringView source)> visitor)
{
    struct Layer
    {
        IFileMount *Mount;
        String Relative;
    };
    List<Layer> layers;
    ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        FileInfo info;
        if (entry.Mount->GetInfo(relative, info) && !info.IsDirectory)
        {
            layers.Add({entry.Mount.Get(), String(relative)});
        }
        return false;
    });
    for (usize i = layers.Count(); i-- > 0;)
    {
        IFileMount &mount = *layers[i].Mount;
        FileHandle file;
        FileRegion region;
        List<uint8> bytes;
        if (!Resolve(mount, layers[i].Relative, file, region) || !ReadResolved(file, region, bytes))
        {
            continue;
        }
        String text;
        ToText(bytes, text);
        StringBuilder<512> source;
        if (!mount.GetPhysicalPath(layers[i].Relative, source))
        {
            // A package's files have no native path.
            source.Append(path);
        }
        visitor(text.AsView(), source.AsView());
    }
}

bool OpenRegion(StringView path, FileRegion &region)
{
    return ForEachCandidate(
        path, [&](MountEntry &entry, StringView relative) { return entry.Mount->OpenRegion(relative, region); });
}

bool GetPhysicalPath(StringView path, StringWriter &output)
{
    bool exists = false;
    const bool found = ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        FileInfo info;
        if (!entry.Mount->GetInfo(relative, info))
        {
            return false;
        }
        exists = true;
        return entry.Mount->GetPhysicalPath(relative, output);
    });
    if (found || exists)
    {
        return found;
    }
    return ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        return entry.Mount->IsWritable() && entry.Mount->GetPhysicalPath(relative, output);
    });
}

void GetLayers(StringView path, List<MountId> &layers)
{
    layers.Clear();
    ForEachCandidate(path, [&](MountEntry &entry, StringView /*relative*/) {
        layers.Add(entry.Id);
        return false;
    });
}

MountId FindLayer(StringView path)
{
    MountId layer = 0;
    ForEachCandidate(path, [&](MountEntry &entry, StringView relative) {
        FileInfo info;
        if (!entry.Mount->GetInfo(relative, info))
        {
            return false;
        }
        layer = entry.Id;
        return true;
    });
    return layer;
}

FileHandle Open(MountId layer, StringView path, FileAccess access, FileMode mode, FileFlags flags)
{
    FileHandle result;
    WithLayer(layer, path, [&](MountEntry &entry, StringView relative) {
        if (access != FileAccess::Read)
        {
            if (!entry.Mount->IsWritable())
            {
                return true;
            }
            (void)CreateParents(*entry.Mount, relative);
        }
        result = entry.Mount->Open(relative, access, mode, flags);
        return true;
    });
    return result;
}

bool GetInfo(MountId layer, StringView path, FileInfo &info)
{
    return WithLayer(layer, path,
                     [&](MountEntry &entry, StringView relative) { return entry.Mount->GetInfo(relative, info); });
}

bool ReadAll(MountId layer, StringView path, String &text)
{
    FileHandle file;
    FileRegion region;
    List<uint8> bytes;
    WithLayer(layer, path,
              [&](MountEntry &entry, StringView relative) { return Resolve(*entry.Mount, relative, file, region); });
    if (!ReadResolved(file, region, bytes))
    {
        return false;
    }
    ToText(bytes, text);
    return true;
}

bool WriteAll(MountId layer, StringView path, StringView text)
{
    return WriteAndClose(Open(layer, path, FileAccess::Write, FileMode::CreateAlways), text);
}

bool GetPhysicalPath(MountId layer, StringView path, StringWriter &output)
{
    return WithLayer(layer, path, [&](MountEntry &entry, StringView relative) {
        return entry.Mount->GetPhysicalPath(relative, output);
    });
}

bool Normalize(StringView path, StringWriter &output)
{
    if (path.IsEmpty() || (path[0] != '/' && path[0] != '\\'))
    {
        return false;
    }
    output.Append('/');
    usize i = 0;
    while (i < path.Length())
    {
        while (i < path.Length() && (path[i] == '/' || path[i] == '\\'))
        {
            ++i;
        }
        const usize start = i;
        while (i < path.Length() && path[i] != '/' && path[i] != '\\')
        {
            ++i;
        }
        const StringView segment = path.Substring(start, i - start);
        if (segment.IsEmpty() || segment == ".")
        {
            continue;
        }
        if (segment == "..")
        {
            return false;
        }
        if (output.Length() > 1)
        {
            output.Append('/');
        }
        output.Append(segment);
    }
    return true;
}
} // namespace Vfs
} // namespace Marlin
