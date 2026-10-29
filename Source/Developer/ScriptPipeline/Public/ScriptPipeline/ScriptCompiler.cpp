#include "ScriptPipeline/ScriptCompiler.h"

#include "Hash/Xxh3.h"
#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPassManager.h"
#include "LanguageIR/MirPasses.h"
#include "ScriptBackend/ScriptTarget.h"
#include "String/Format.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
void GetScriptSourceRoots(List<String> &roots)
{
    roots.Add(String("/Engine"));
    FileInfo project;
    if (Vfs::GetInfo("/Project", project))
    {
        roots.Add(String("/Project"));
    }
}

void GetScriptSourceFiles(List<String> &paths)
{
    List<String> roots;
    GetScriptSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    sources.ListFiles([&](StringView path) {
        if (path.EndsWith(".mscript") || path.EndsWith(".mcommon"))
        {
            paths.Add(String(path));
        }
    });
    paths.Sort([](const String &a, const String &b) { return a < b; });
}

void GetScriptPackages(List<String> &packages)
{
    List<String> roots;
    GetScriptSourceRoots(roots);
    for (const String &root : roots)
    {
        packages.Add(String(root.AsView().Substring(1)));
    }
}

bool HasScripts(StringView package, Span<const String> files)
{
    const String prefix = Format("/{0}/", package);
    for (const String &file : files)
    {
        if (file.AsView().StartsWith(prefix.AsView()) && file.AsView().EndsWith(".mscript"))
        {
            return true;
        }
    }
    return false;
}

ScriptCompileResult CompileScriptPackage(StringView package, const ScriptCompileOptions &options)
{
    ScriptCompileResult result;
    List<String> roots;
    GetScriptSourceRoots(roots);
    List<StringView> rootViews;
    for (const String &root : roots)
    {
        rootViews.Add(root.AsView());
    }
    VfsSourceProvider sources(rootViews.AsSpan());
    LanguageCompileOptions compileOptions;
    compileOptions.Language = LanguageDialect::Script;
    compileOptions.DebugOutput = options.DebugOutput;
    compileOptions.DebugInfo = options.DebugInfo;
    LanguageCompilation compilation(sources, compileOptions);
    MirModule module;
    const bool compiled = compilation.Load() && compilation.CompileScriptPackage(package, module);
    // A package sees every file of the source set (MarlinScript §1.3): a new file can add to it, so all of them are
    // dependencies, not only those this compile used; their hashes are of the texts it read.
    List<const SourceFile *> files;
    compilation.GetSourceFiles(files);
    for (const SourceFile *file : files)
    {
        result.Dependencies.Add({String(file->GetPath()), file->GetHash()});
    }
    if (!compiled)
    {
        result.Errors = compilation.GetDiagnostics().ToString();
        return result;
    }

    MirTargetRegistry targets;
    RegisterScriptTarget(targets);
    const MirPassRegistry registry = MirPassRegistry::CreateDefault();
    MirModulePassManager passes;
    String error;
    if (!registry.ParsePipeline(MirScriptPipeline, passes, error))
    {
        result.Errors = Move(error);
        return result;
    }
    List<String> errors;
    targets.Find(ScriptTargetName)->AddLegalizationPasses(passes, errors);
    MirModuleAnalysisManager analyses;
    passes.Run(module, analyses);
    MirTargetOptions targetOptions;
    targetOptions.DebugInfo = options.DebugInfo;
    if (errors.IsEmpty() && passes.GetErrors().IsEmpty() &&
        CompileScriptModule(module, targetOptions, result.Module, errors))
    {
        result.Succeeded = true;
        return result;
    }
    result.Errors = !errors.IsEmpty()               ? Move(errors[0])
                    : !passes.GetErrors().IsEmpty() ? String(passes.GetErrors()[0].AsView())
                                                    : Format("the module of {0} did not compile", package);
    return result;
}
} // namespace Marlin
