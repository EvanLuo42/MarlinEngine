#include "LanguageFrontend/Source.h"

#include "Hash/Xxh3.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
bool GetDialectOf(StringView path, LanguageDialect &dialect)
{
    if (path.EndsWith(".mcommon"))
    {
        dialect = LanguageDialect::Common;
        return true;
    }
    if (path.EndsWith(".mshader"))
    {
        dialect = LanguageDialect::Shader;
        return true;
    }
    if (path.EndsWith(".mscript"))
    {
        dialect = LanguageDialect::Script;
        return true;
    }
    return false;
}

StringView GetDialectFolder(LanguageDialect dialect)
{
    switch (dialect)
    {
    case LanguageDialect::Common:
        return "Common";
    case LanguageDialect::Shader:
        return "Shaders";
    case LanguageDialect::Script:
        return "Scripts";
    }
    return {};
}

bool GetFolderDialectOf(StringView path, LanguageDialect &dialect)
{
    StringBuilder<64> marker(LanguageFolder);
    marker.Append('/');
    const usize start = path.IndexOf(marker.AsView());
    if (start == StringView::NotFound)
    {
        return false;
    }
    const StringView rest = path.RemoveStart(start + marker.Length());
    for (const LanguageDialect candidate : {LanguageDialect::Common, LanguageDialect::Shader, LanguageDialect::Script})
    {
        const StringView folder = GetDialectFolder(candidate);
        if (rest.Length() > folder.Length() && rest.StartsWith(folder) && rest[folder.Length()] == '/')
        {
            dialect = candidate;
            return true;
        }
    }
    return false;
}

bool IsCoreLibraryPath(StringView path)
{
    StringBuilder<64> prefix("/Engine");
    prefix.Append(LanguageFolder).Append('/');
    LanguageDialect dialect;
    if (!path.StartsWith(prefix.AsView()) || !GetFolderDialectOf(path, dialect))
    {
        return false;
    }
    return path.RemoveStart(prefix.Length() + GetDialectFolder(dialect).Length()).StartsWith("/Core/");
}

SourceFile::SourceFile(uint32 id, StringView path, String text, LanguageDialect dialect)
    : _path(path.Data(), path.Length()), _text(Move(text)), _id(id), _dialect(dialect)
{
    _hash = Xxh3Hash128(_text.Data(), _text.Length());
    _lineStarts.Add(0);
    for (usize i = 0; i < _text.Length(); ++i)
    {
        if (_text[i] == '\n')
        {
            _lineStarts.Add(static_cast<uint32>(i + 1));
        }
    }
}

void SourceFile::GetLineAndColumn(uint32 offset, uint32 &line, uint32 &column) const
{
    usize low = 0;
    usize high = _lineStarts.Count();
    while (high - low > 1)
    {
        const usize middle = (low + high) / 2;
        if (_lineStarts[middle] <= offset)
        {
            low = middle;
        }
        else
        {
            high = middle;
        }
    }
    line = static_cast<uint32>(low + 1);
    column = offset - _lineStarts[low] + 1;
}

void MemorySourceProvider::Add(StringView path, StringView text)
{
    _files.Add({String(path.Data(), path.Length()), String(text.Data(), text.Length())});
}

void MemorySourceProvider::ListFiles(FunctionRef<void(StringView path)> visitor)
{
    for (const Entry &entry : _files)
    {
        visitor(entry.Path.AsView());
    }
}

bool MemorySourceProvider::ReadFile(StringView path, String &text)
{
    for (const Entry &entry : _files)
    {
        if (entry.Path.AsView() == path)
        {
            text = entry.Text;
            return true;
        }
    }
    return false;
}

CompositeSourceProvider::CompositeSourceProvider(Span<ISourceProvider *const> providers)
{
    _providers.AddRange(providers);
}

void CompositeSourceProvider::ListFiles(FunctionRef<void(StringView path)> visitor)
{
    for (ISourceProvider *provider : _providers)
    {
        provider->ListFiles(visitor);
    }
}

bool CompositeSourceProvider::ReadFile(StringView path, String &text)
{
    for (ISourceProvider *provider : _providers)
    {
        if (provider->ReadFile(path, text))
        {
            return true;
        }
    }
    return false;
}

VfsSourceProvider::VfsSourceProvider(Span<const StringView> roots)
{
    for (const StringView root : roots)
    {
        _roots.Add(String(root.Data(), root.Length()));
    }
}

void VfsSourceProvider::ListFiles(FunctionRef<void(StringView path)> visitor)
{
    List<String> pending;
    for (const String &root : _roots)
    {
        StringBuilder<256> directory(root.AsView());
        directory.Append(LanguageFolder);
        pending.Add(directory.ToString());
    }
    while (!pending.IsEmpty())
    {
        const String directory = pending.Pop();
        Vfs::Enumerate(directory.AsView(), [&](StringView name, const FileInfo &info) {
            StringBuilder<256> path(directory.AsView());
            path.Append('/').Append(name);
            LanguageDialect dialect;
            if (info.IsDirectory)
            {
                pending.Add(path.ToString());
            }
            else if (GetDialectOf(name, dialect))
            {
                visitor(path.AsView());
            }
        });
    }
}

bool VfsSourceProvider::ReadFile(StringView path, String &text)
{
    return Vfs::ReadAll(path, text);
}
} // namespace Marlin
