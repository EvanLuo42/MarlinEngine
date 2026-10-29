#include "Books.h"
#include "CppExtractor.h"
#include "DataWriter.h"
#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "Json/JsonValue.h"
#include "LanguageExtractor.h"
#include "Links.h"
#include "Misc/CommandLine.h"
#include "String/Format.h"
#include "Text.h"

namespace
{
using namespace Marlin;
using namespace Marlin::Doc;

/// The version of the job file `marlin doc` writes.
constexpr int64 JobFormatVersion = 1;

StringView StringOf(const JsonValue &object, StringView key)
{
    const JsonValue *value = object.Find(key);
    return value != nullptr ? value->AsString() : StringView();
}

List<String> StringsOf(const JsonValue &object, StringView key)
{
    List<String> result;
    if (const JsonValue *value = object.Find(key))
    {
        for (const JsonValue &element : value->GetElements())
        {
            result.Add(element.AsString());
        }
    }
    return result;
}

/// Reports each public item without docs, or how many there are per module unless *each*.
void ReportMissingDocs(const Project &project, bool each, Reporter &reporter)
{
    for (const Module &module : project.Modules)
    {
        uint32 missing = 0;
        for (const Item &item : module.Items)
        {
            if (!item.Docs.IsEmpty() || item.Visibility != "public" || item.Kind == "namespace")
            {
                continue;
            }
            ++missing;
            if (each)
            {
                reporter.Warn(item.File, item.Line, "missing-docs",
                              Format("{0} has no `///` docs", item.QualifiedName));
            }
        }
        if (!each && missing > 0)
        {
            MARLIN_LOG(LogCore, Info, "{0}: {1} of {2} items have no docs (marlin doc --missing-docs lists them)",
                       module.Name, missing, module.Items.Count());
        }
    }
}

int32 Run(const JsonValue &job, Reporter &reporter)
{
    int64 version = 0;
    const JsonValue *versionValue = job.Find("format_version");
    if (versionValue == nullptr || !versionValue->TryGetInteger(version) || version != JobFormatVersion)
    {
        reporter.Error("the job file is from another version of the toolkit; rebuild MarlinDoc");
        return 1;
    }
    Project project;
    project.Root = StringOf(job, "root");
    const JsonValue *modules = job.Find("modules");
    const JsonValue *languages = job.Find("languages");
    const bool hasCpp = modules != nullptr && modules->Count() > 0;
    if (hasCpp && !LoadLibClang(StringOf(job, "libclang"), reporter))
    {
        return 1;
    }
    if (modules != nullptr)
    {
        for (const JsonValue &entry : modules->GetElements())
        {
            CppModuleJob cpp;
            cpp.Name = StringOf(entry, "name");
            cpp.Kind = StringOf(entry, "kind");
            cpp.Directory = StringOf(entry, "directory");
            cpp.DocsFile = StringOf(entry, "docs");
            cpp.Headers = StringsOf(entry, "headers");
            cpp.Arguments = StringOf(entry, "arguments");
            Module &module = project.Modules.AddDefaulted();
            module.Name = cpp.Name;
            module.Kind = cpp.Kind;
            module.Directory = cpp.Directory;
            if (!cpp.DocsFile.IsEmpty() && !ReadFile(cpp.DocsFile, module.Docs))
            {
                reporter.Error(Format("{0} cannot be read", cpp.DocsFile));
            }
            MARLIN_LOG(LogCore, Info, "{0}: {1} headers", cpp.Name, cpp.Headers.Count());
            ExtractCpp(cpp, project.Root, module, reporter);
        }
    }
    if (hasCpp)
    {
        UnloadLibClang();
    }
    if (languages != nullptr)
    {
        for (const JsonValue &entry : languages->GetElements())
        {
            LanguageLibraryJob library;
            library.Name = StringOf(entry, "name");
            library.Language = StringOf(entry, "language");
            library.Directory = StringOf(entry, "directory");
            library.Files = StringsOf(entry, "files");
            Module &module = project.Modules.AddDefaulted();
            module.Name = library.Name;
            module.Kind = "Language";
            module.Directory = library.Directory;
            ExtractLanguage(library, project.Root, module, reporter);
        }
    }
    for (const String &directory : StringsOf(job, "books"))
    {
        Book book;
        if (ReadBook(directory, project.Root, book, reporter))
        {
            project.Books.Add(Move(book));
        }
    }
    project.Books.StableSort([](const Book &a, const Book &b) {
        if (a.HasOrder != b.HasOrder)
        {
            return a.HasOrder;
        }
        return a.HasOrder && a.Order != b.Order ? a.Order < b.Order : a.Name.AsView() < b.Name.AsView();
    });
    ResolveLinks(project, reporter);
    const JsonValue *missingDocs = job.Find("missing_docs");
    ReportMissingDocs(project, missingDocs != nullptr && missingDocs->AsBool(), reporter);

    const String data = Format("{0}/Data", StringOf(job, "output"));
    if (!WriteData(project, data, reporter))
    {
        return 1;
    }
    usize items = 0;
    for (const Module &module : project.Modules)
    {
        items += module.Items.Count();
    }
    MARLIN_LOG(LogCore, Info, "MarlinDoc: {0} items in {1} modules, {2} books; {3} warnings", items,
               project.Modules.Count(), project.Books.Count(), reporter.GetWarnings());
    return reporter.GetErrors() > 0 ? 1 : (reporter.GetWarnings() > 0 ? 2 : 0);
}
} // namespace

/// `MarlinDoc -Job=<file>`: runs the job `marlin doc` wrote (Documentation/Architecture/16 §6). Exits with 0, with 2
/// when something was reported, or with 1 on an error.
int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinDoc"))
    {
        return 1;
    }
    int32 exitCode = 1;
    StringView jobPath;
    String text;
    JsonValue job;
    JsonError error;
    Doc::Reporter reporter;
    if (!CommandLine::TryGetValue("Job", jobPath))
    {
        MARLIN_LOG(LogCore, Error, "MarlinDoc needs -Job=<file>, which `marlin doc` writes");
    }
    else if (!Doc::ReadFile(jobPath, text))
    {
        MARLIN_LOG(LogCore, Error, "{0} cannot be read", jobPath);
    }
    else if (!Json::Parse(text, job, error))
    {
        MARLIN_LOG(LogCore, Error, "{0}({1}): {2}", jobPath, error.Line, error.Message);
    }
    else
    {
        exitCode = Run(job, reporter);
    }
    ProgramHost::Shutdown();
    return exitCode;
}
