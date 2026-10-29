#include "Generator.h"
#include "Model.h"
#include "Parser.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace FileSystem = std::filesystem;

namespace Marlin::HeaderTool
{
namespace
{
struct Options
{
    std::string Module;
    FileSystem::path Output;
    FileSystem::path Manifest;
    std::vector<FileSystem::path> Roots;
    std::vector<FileSystem::path> Excluded;
    std::set<std::string> SkippedNames;
};

constexpr const char Usage[] =
    "usage: MarlinHeaderTool --module <Name> --output <dir> --manifest <file> --root <dir>... "
    "[--exclude <dir>...] [--skip <folder name>...]\n"
    "Reads the reflection markup of a module, highest layer root first, and writes its .gen.h and .gen.cpp files.\n";

bool ParseOptions(int argc, const char *const *argv, Options &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view argument = argv[i];
        if (i + 1 >= argc)
        {
            return false;
        }
        const char *value = argv[++i];
        if (argument == "--module")
        {
            options.Module = value;
        }
        else if (argument == "--output")
        {
            options.Output = value;
        }
        else if (argument == "--manifest")
        {
            options.Manifest = value;
        }
        else if (argument == "--root")
        {
            options.Roots.emplace_back(value);
        }
        else if (argument == "--exclude")
        {
            options.Excluded.emplace_back(FileSystem::path(value).lexically_normal());
        }
        else if (argument == "--skip")
        {
            options.SkippedNames.insert(value);
        }
        else
        {
            return false;
        }
    }
    return !options.Module.empty() && !options.Output.empty() && !options.Manifest.empty() && !options.Roots.empty();
}

bool ReadFile(const FileSystem::path &path, std::string &text)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
    {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return true;
}

/// Writes *text* unless the file already holds it, so unchanged outputs keep their timestamps.
bool WriteIfChanged(const FileSystem::path &path, const std::string &text)
{
    std::string existing;
    if (ReadFile(path, existing) && existing == text)
    {
        return true;
    }
    std::error_code error;
    FileSystem::create_directories(path.parent_path(), error);
    std::ofstream stream(path, std::ios::binary);
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(stream);
}

std::string MakeFileId(const std::string &module, const std::string &includePath)
{
    std::string id = module + "_";
    for (const char c : includePath)
    {
        id += std::isalnum(static_cast<unsigned char>(c)) != 0 ? c : '_';
    }
    return id;
}

class Scanner
{
public:
    explicit Scanner(const Options &options) : _options(&options)
    {
    }

    /// Calls *visit* with each file below *directory* whose extension is in *extensions*, and its path relative to
    /// *directory*.
    template <typename TVisitor>
    void Walk(const FileSystem::path &directory, std::initializer_list<std::string_view> extensions,
              TVisitor visit) const
    {
        std::error_code error;
        if (!FileSystem::is_directory(directory, error))
        {
            return;
        }
        for (auto it = FileSystem::recursive_directory_iterator(directory, error);
             !error && it != FileSystem::recursive_directory_iterator(); it.increment(error))
        {
            const FileSystem::path &path = it->path();
            if (it->is_directory(error))
            {
                if (IsSkipped(path))
                {
                    it.disable_recursion_pending();
                }
                continue;
            }
            const std::string extension = path.extension().string();
            if (std::ranges::find(extensions, extension) != extensions.end())
            {
                visit(path, path.lexically_relative(directory).generic_string());
            }
        }
    }

    /// Whether Walk leaves out the directory *path* and what is below it.
    bool IsSkipped(const FileSystem::path &path) const
    {
        const std::string name = path.filename().string();
        return name.starts_with('.') || _options->SkippedNames.contains(name) || IsExcluded(path);
    }

private:
    bool IsExcluded(const FileSystem::path &path) const
    {
        const FileSystem::path normal = path.lexically_normal();
        return std::ranges::find(_options->Excluded, normal) != _options->Excluded.end();
    }

    const Options *_options;
};

/// A source file of a module root, from the one walk of the root, read at most once.
struct SourceFile
{
    FileSystem::path Path;
    /// The path relative to the root.
    std::string Relative;
    std::string Text;
    bool Read = false;
    bool Readable = false;

    /// The text of the file, or null if it cannot be read.
    const std::string *GetText()
    {
        if (!Read)
        {
            Read = true;
            Readable = ReadFile(Path, Text);
        }
        return Readable ? &Text : nullptr;
    }
};

/// Every file below *root* that a scan of the module reads: headers, C and C++ sources.
std::vector<SourceFile> WalkRoot(const Scanner &scanner, const FileSystem::path &root)
{
    std::vector<SourceFile> files;
    scanner.Walk(root, {".h", ".hpp", ".inl", ".c", ".cpp"},
                 [&](const FileSystem::path &path, const std::string &relative) {
                     files.push_back({path, relative, {}, false, false});
                 });
    return files;
}

bool IsHeader(const FileSystem::path &path)
{
    const FileSystem::path extension = path.extension();
    return extension == ".h" || extension == ".hpp" || extension == ".inl";
}

/// A 64-bit hash of a byte stream, eight bytes at a time.
class SourceHasher
{
public:
    /// Adds *bytes* and their length, so consecutive pieces cannot run into each other.
    void Mix(std::string_view bytes)
    {
        const char *data = bytes.data();
        std::size_t size = bytes.size();
        for (; size >= 8; data += 8, size -= 8)
        {
            std::uint64_t word;
            std::memcpy(&word, data, 8);
            Add(word);
        }
        std::uint64_t tail = 0;
        std::memcpy(&tail, data, size);
        Add(tail);
        Add(bytes.size());
    }

    [[nodiscard]] std::uint64_t Get() const
    {
        std::uint64_t hash = _hash;
        hash = (hash ^ (hash >> 33)) * 0xFF51AFD7ED558CCDull;
        hash = (hash ^ (hash >> 33)) * 0xC4CEB9FE1A85EC53ull;
        return hash ^ (hash >> 33);
    }

private:
    void Add(std::uint64_t word)
    {
        _hash = std::rotl(_hash ^ word, 31) * 0x9E3779B97F4A7C15ull;
    }

    std::uint64_t _hash = 0xCBF29CE484222325ull;
};

/// *text* without what cannot change what the code does: comments, and runs of spaces and tabs, which become one
/// space. Line breaks stay, as they end preprocessor directives; string, character and raw string literals are kept
/// as written. A digit separator (`1'000`) is no character literal.
std::string StripForHash(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    std::size_t i = 0;
    const auto copyUntil = [&](std::string_view end) {
        const std::size_t found = text.find(end, i);
        const std::size_t stop = found == std::string_view::npos ? text.size() : found + end.size();
        out.append(text.substr(i, stop - i));
        i = stop;
    };
    while (i < text.size())
    {
        const char c = text[i];
        const char next = i + 1 < text.size() ? text[i + 1] : '\0';
        if (c == '/' && next == '/')
        {
            const std::size_t end = text.find('\n', i);
            i = end == std::string_view::npos ? text.size() : end;
            continue;
        }
        if (c == '/' && next == '*')
        {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 2;
            out += ' ';
            continue;
        }
        if (c == 'R' && next == '"' && (out.empty() || !std::isalnum(static_cast<unsigned char>(out.back()))))
        {
            const std::size_t open = text.find('(', i + 2);
            if (open != std::string_view::npos)
            {
                const std::string close = ")" + std::string(text.substr(i + 2, open - i - 2)) + "\"";
                copyUntil(close);
                continue;
            }
        }
        const bool separator = c == '\'' && !out.empty() && std::isalnum(static_cast<unsigned char>(out.back()));
        if ((c == '"' || c == '\'') && !separator)
        {
            out += c;
            ++i;
            while (i < text.size() && text[i] != c && text[i] != '\n')
            {
                if (text[i] == '\\' && i + 1 < text.size())
                {
                    out += text[i++];
                }
                out += text[i++];
            }
            if (i < text.size())
            {
                out += text[i++];
            }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r')
        {
            if (!out.empty() && out.back() != ' ' && out.back() != '\n')
            {
                out += ' ';
            }
            ++i;
            continue;
        }
        if (c == '\n')
        {
            while (!out.empty() && out.back() == ' ')
            {
                out.pop_back();
            }
            if (!out.empty() && out.back() != '\n')
            {
                out += '\n';
            }
            ++i;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

bool HasMarkup(std::string_view text, std::initializer_list<std::string_view> markers)
{
    return std::ranges::any_of(markers,
                               [&](std::string_view marker) { return text.find(marker) != std::string_view::npos; });
}

bool Validate(const std::string &path, const GeneratedFile &file, std::vector<Diagnostic> &errors)
{
    const std::string expected = file.IncludePath.substr(0, file.IncludePath.rfind('.')) + ".gen.h";
    const auto &includes = file.Parsed.Includes;
    if (includes.empty() || includes.back().first != expected)
    {
        const int line = includes.empty() ? 1 : includes.back().second;
        errors.push_back({path, line, "a header with reflected types must include \"" + expected + "\" last"});
        return false;
    }
    for (const RecordDecl &record : file.Parsed.Records)
    {
        for (const MetadataItem &item : record.Metadata)
        {
            if (item.Key == "Version" && (item.Value.empty() || !std::ranges::all_of(item.Value, [](char c) {
                                              return std::isdigit(static_cast<unsigned char>(c)) != 0;
                                          })))
            {
                errors.push_back({path, record.Line, record.Name + ": Version must be a positive integer"});
                return false;
            }
        }
    }
    return true;
}

/// `<Module>.SourceHash.gen.h`: `<MODULE>_SOURCE_HASH`, a hash of every source file of the module (StripForHash),
/// for keys of results the module's code computes, such as compiled shaders, which must change whenever that code
/// does.
std::string GenerateSourceHash(const Options &options, std::vector<std::vector<SourceFile>> &roots)
{
    // Highest layer first, as for headers: a file a higher layer overrides is not part of the module.
    std::map<std::string, SourceFile *> sources;
    for (std::vector<SourceFile> &files : roots)
    {
        for (SourceFile &file : files)
        {
            sources.emplace(file.Relative, &file);
        }
    }
    // Each relative path and text, in path order: the hash changes whenever any source does, which is all its users
    // rely on. Comments and spacing are left out, so editing them invalidates nothing compiled with the module.
    SourceHasher hasher;
    for (const auto &[relative, file] : sources)
    {
        const std::string *text = file->GetText();
        if (text == nullptr)
        {
            continue;
        }
        hasher.Mix(relative);
        hasher.Mix(StripForHash(*text));
    }
    const std::uint64_t hash = hasher.Get();
    std::string api;
    for (const char c : options.Module)
    {
        api += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    char digits[17];
    std::snprintf(digits, sizeof(digits), "%016llx", static_cast<unsigned long long>(hash));
    std::string out = "// Generated by MarlinHeaderTool for module " + options.Module + ". Do not edit.\n";
    out += "#pragma once\n\n";
    out += "/// A hash of the sources of " + options.Module + ", which changes whenever any of them does.\n";
    out += "#define " + api + "_SOURCE_HASH 0x" + std::string(digits) + "ull\n";
    return out;
}

void RemoveStale(const FileSystem::path &directory, const std::set<FileSystem::path> &keep)
{
    std::error_code error;
    if (!FileSystem::is_directory(directory, error))
    {
        return;
    }
    std::vector<FileSystem::path> stale;
    for (auto it = FileSystem::recursive_directory_iterator(directory, error);
         !error && it != FileSystem::recursive_directory_iterator(); it.increment(error))
    {
        if (it->is_regular_file(error) && !keep.contains(it->path().lexically_normal()))
        {
            stale.push_back(it->path());
        }
    }
    for (const FileSystem::path &path : stale)
    {
        FileSystem::remove(path, error);
    }
}

int Run(const Options &options)
{
    const Scanner scanner(options);
    std::vector<Diagnostic> errors;
    std::map<std::string, GeneratedFile> headers;
    std::set<std::string> seen;
    std::vector<UpgradeDecl> upgrades;
    std::vector<TestDecl> tests;
    std::set<std::string> seenSources;

    // One walk of each root, whose files every scan below shares.
    std::vector<std::vector<SourceFile>> rootFiles;
    for (const FileSystem::path &root : options.Roots)
    {
        rootFiles.push_back(WalkRoot(scanner, root));
    }
    for (std::size_t rootIndex = 0; rootIndex < options.Roots.size(); ++rootIndex)
    {
        const FileSystem::path &root = options.Roots[rootIndex];
        std::vector<SourceFile> &files = rootFiles[rootIndex];
        std::vector<FileSystem::path> includeDirectories;
        std::error_code error;
        for (const char *sub : {"Public", "Internal", "Private"})
        {
            if (FileSystem::is_directory(root / sub, error))
            {
                includeDirectories.push_back(root / sub);
            }
        }
        if (includeDirectories.empty())
        {
            includeDirectories.push_back(root);
        }
        for (const FileSystem::path &directory : includeDirectories)
        {
            const auto scan = [&](SourceFile &source, const std::string &relative) {
                const FileSystem::path &path = source.Path;
                if (!seen.insert(directory.filename().string() + "/" + relative).second)
                {
                    return;
                }
                const std::string *read = source.GetText();
                if (read == nullptr ||
                    !HasMarkup(*read, {"MSTRUCT(", "MCLASS(", "MINTERFACE(", "MENUM(", "MARLIN_UPGRADE("}))
                {
                    return;
                }
                const std::string &text = *read;
                GeneratedFile file{relative, MakeFileId(options.Module, relative), {}};
                const std::string display = path.string();
                if (!ParseFile(display, text, file.Parsed, errors))
                {
                    return;
                }
                upgrades.insert(upgrades.end(), file.Parsed.Upgrades.begin(), file.Parsed.Upgrades.end());
                if (!file.Parsed.IsReflected())
                {
                    return;
                }
                if (!Validate(display, file, errors))
                {
                    return;
                }
                if (headers.contains(relative))
                {
                    errors.push_back({
                        display,
                        1,
                        "another header of " + options.Module + " is also " + relative +
                            "; reflected headers need distinct include paths",
                    });
                    return;
                }
                headers.emplace(relative, std::move(file));
            };
            if (directory == root)
            {
                for (SourceFile &source : files)
                {
                    if (IsHeader(source.Path))
                    {
                        scan(source, source.Relative);
                    }
                }
                continue;
            }
            if (scanner.IsSkipped(directory))
            {
                // The walk of the root left it out, but it is still a folder of the module's headers.
                std::vector<SourceFile> own = WalkRoot(scanner, directory);
                for (SourceFile &source : own)
                {
                    if (IsHeader(source.Path))
                    {
                        scan(source, source.Relative);
                    }
                }
                continue;
            }
            const std::string prefix = directory.filename().string() + "/";
            for (SourceFile &source : files)
            {
                if (IsHeader(source.Path) && source.Relative.starts_with(prefix))
                {
                    scan(source, source.Relative.substr(prefix.size()));
                }
            }
        }
        for (SourceFile &source : files)
        {
            const FileSystem::path &path = source.Path;
            const std::string &relative = source.Relative;
            if (path.extension() != ".cpp" || !seenSources.insert(relative).second)
            {
                continue;
            }
            const std::string *text = source.GetText();
            if (text == nullptr || !HasMarkup(*text, {"MARLIN_UPGRADE(", "MARLIN_TEST"}))
            {
                continue;
            }
            ParsedFile parsed;
            if (ParseFile(path.generic_string(), *text, parsed, errors))
            {
                upgrades.insert(upgrades.end(), parsed.Upgrades.begin(), parsed.Upgrades.end());
                for (TestDecl &test : parsed.Tests)
                {
                    const bool duplicate = std::ranges::any_of(tests, [&](const TestDecl &other) {
                        return other.Suite == test.Suite && other.Name == test.Name;
                    });
                    if (duplicate)
                    {
                        errors.push_back({
                            test.File,
                            test.Line,
                            "test " + test.Suite + "." + test.Name + " is defined twice in " + options.Module,
                        });
                        continue;
                    }
                    tests.push_back(std::move(test));
                }
            }
        }
    }

    if (!errors.empty())
    {
        for (const Diagnostic &diagnostic : errors)
        {
            std::fprintf(stderr, "%s(%d): error: %s\n", diagnostic.File.c_str(), diagnostic.Line,
                         diagnostic.Message.c_str());
        }
        return 1;
    }

    const ModuleInfo module{
        options.Module,
        [&] {
            std::string api;
            for (const char c : options.Module)
            {
                api += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            }
            return api + "_API";
        }(),
    };
    const FileSystem::path includeRoot = options.Output / "Include";
    const FileSystem::path sourceRoot = options.Output / "Source";
    std::set<FileSystem::path> written;
    std::vector<const GeneratedFile *> files;
    bool ok = true;
    for (const auto &[relative, file] : headers)
    {
        const FileSystem::path header =
            (includeRoot / (relative.substr(0, relative.rfind('.')) + ".gen.h")).lexically_normal();
        const FileSystem::path source = (sourceRoot / (file.FileId + ".gen.cpp")).lexically_normal();
        ok &= WriteIfChanged(header, GenerateHeader(module, file));
        ok &= WriteIfChanged(source, GenerateSource(module, file));
        written.insert(header);
        written.insert(source);
        files.push_back(&file);
    }
    const FileSystem::path registration = (sourceRoot / "Module.gen.cpp").lexically_normal();
    ok &= WriteIfChanged(registration, GenerateModule(module, files, upgrades, tests));
    written.insert(registration);
    const FileSystem::path sourceHash = (includeRoot / (options.Module + ".SourceHash.gen.h")).lexically_normal();
    ok &= WriteIfChanged(sourceHash, GenerateSourceHash(options, rootFiles));
    written.insert(sourceHash);
    RemoveStale(includeRoot, written);
    RemoveStale(sourceRoot, written);

    std::ostringstream manifest;
    manifest << "// " << options.Module << ": " << files.size() << " reflected headers, " << upgrades.size()
             << " upgrades, " << tests.size() << " tests\n";
    for (const FileSystem::path &path : written)
    {
        manifest << path.generic_string() << "\n";
    }
    std::error_code error;
    FileSystem::create_directories(options.Manifest.parent_path(), error);
    std::ofstream stream(options.Manifest, std::ios::binary);
    stream << manifest.str();
    if (!ok || !stream)
    {
        std::fprintf(stderr, "MarlinHeaderTool: cannot write the outputs of %s\n", options.Module.c_str());
        return 1;
    }
    return 0;
}
} // namespace
} // namespace Marlin::HeaderTool

int main(int argc, const char *const *argv)
{
    Marlin::HeaderTool::Options options;
    if (!Marlin::HeaderTool::ParseOptions(argc, argv, options))
    {
        std::fputs(Marlin::HeaderTool::Usage, stderr);
        return 2;
    }
    return Marlin::HeaderTool::Run(options);
}
