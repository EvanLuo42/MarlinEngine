#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
/// Which language a file is written in, from its extension.
enum class LanguageDialect : uint8
{
    Common,
    Shader,
    Script,
};

/// The dialect of *path* by its extension, false if it is not a language file.
[[nodiscard]] LANGUAGEFRONTEND_API bool GetDialectOf(StringView path, LanguageDialect &dialect);

/// The folder of a mount root that holds its language code (§1.3), such as `/Project/Language`.
inline constexpr StringView LanguageFolder = "/Language";

/// The folder under LanguageFolder that holds the files of *dialect*: `Common`, `Shaders` or `Scripts` (§1.5).
[[nodiscard]] LANGUAGEFRONTEND_API StringView GetDialectFolder(LanguageDialect dialect);

/// The dialect whose folder *path* is in, such as Shader for `/Project/Language/Shaders/Rock.mshader`; false if it is
/// not in a dialect folder of a mount root's LanguageFolder.
[[nodiscard]] LANGUAGEFRONTEND_API bool GetFolderDialectOf(StringView path, LanguageDialect &dialect);

/// Whether *path* is in the core library (§9.10): the `Core/` folder of a dialect folder of `/Engine/Language`.
[[nodiscard]] LANGUAGEFRONTEND_API bool IsCoreLibraryPath(StringView path);

/// Whether a compilation of *language* sees files of *dialect*: its own and the common subset (§1.3).
[[nodiscard]] constexpr bool IsInSourceSet(LanguageDialect language, LanguageDialect dialect)
{
    return dialect == language || dialect == LanguageDialect::Common;
}

/// A source file and its text. Offsets are bytes into the text; lines and columns are 1-based, columns in bytes.
class LANGUAGEFRONTEND_API SourceFile
{
public:
    SourceFile(uint32 id, StringView path, String text, LanguageDialect dialect);
    MARLIN_NONCOPYABLE(SourceFile);

    [[nodiscard]] uint32 GetId() const
    {
        return _id;
    }

    [[nodiscard]] StringView GetPath() const
    {
        return _path.AsView();
    }

    [[nodiscard]] StringView GetText() const
    {
        return _text.AsView();
    }

    [[nodiscard]] LanguageDialect GetDialect() const
    {
        return _dialect;
    }

    /// The hash of the text, for dependency tracking.
    [[nodiscard]] const Hash128 &GetHash() const
    {
        return _hash;
    }

    void GetLineAndColumn(uint32 offset, uint32 &line, uint32 &column) const;

private:
    String _path;
    String _text;
    List<uint32> _lineStarts;
    Hash128 _hash;
    uint32 _id;
    LanguageDialect _dialect;
};

/// A range of a source file.
struct SourceRange
{
    uint32 File = 0;
    uint32 Begin = 0;
    uint32 End = 0;

    [[nodiscard]] bool IsValid() const
    {
        return File != 0;
    }
};

/// Lists and reads the files of a source set (§1.3).
class LANGUAGEFRONTEND_API ISourceProvider
{
public:
    ISourceProvider() = default;
    virtual ~ISourceProvider() = default;
    MARLIN_NONCOPYABLE(ISourceProvider);

    /// Visits the path of every language file of the source set.
    virtual void ListFiles(FunctionRef<void(StringView path)> visitor) = 0;
    /// Reads the text of *path*; false if it cannot be read.
    virtual bool ReadFile(StringView path, String &text) = 0;
};

/// A source set held in memory, for tests and tools.
class LANGUAGEFRONTEND_API MemorySourceProvider final : public ISourceProvider
{
public:
    MemorySourceProvider() = default;

    void Add(StringView path, StringView text);

    void ListFiles(FunctionRef<void(StringView path)> visitor) override;
    bool ReadFile(StringView path, String &text) override;

private:
    struct Entry
    {
        String Path;
        String Text;
    };

    List<Entry> _files;
};

/// Several providers as one source set, such as the mounted engine and a test's own files.
class LANGUAGEFRONTEND_API CompositeSourceProvider final : public ISourceProvider
{
public:
    explicit CompositeSourceProvider(Span<ISourceProvider *const> providers);

    void ListFiles(FunctionRef<void(StringView path)> visitor) override;
    bool ReadFile(StringView path, String &text) override;

private:
    List<ISourceProvider *> _providers;
};

/// The source set of the mounted engine and project: every language file under the LanguageFolder of each of *roots*,
/// such as `/Engine` and `/Project`.
class LANGUAGEFRONTEND_API VfsSourceProvider final : public ISourceProvider
{
public:
    explicit VfsSourceProvider(Span<const StringView> roots);

    void ListFiles(FunctionRef<void(StringView path)> visitor) override;
    bool ReadFile(StringView path, String &text) override;

private:
    List<String> _roots;
};
} // namespace Marlin
