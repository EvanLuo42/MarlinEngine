#pragma once

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPassManager.h"
#include "LanguageIR/MirPasses.h"
#include "ScriptBackend/ScriptTarget.h"
#include "ScriptVM/ScriptDomain.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
/// Compiles the engine package and *source* as the `Project` package, adds both to a VM and loads them into a new
/// domain (MarlinScript §6, §9.6). Script functions are found by their link names, such as `Fib(int)`. With *vm*, the
/// modules go to that VM instead of the run's own.
// RECOVERY-DECISION S16: the VM tests are written to the outline and the recovered assert texts; their script
// sources are new.
struct ScriptRun
{
    ScriptRun(StringView source, const ScriptDomainOptions &options = {}, bool debugOutput = false,
              ScriptVM *vm = nullptr)
        : Target(vm != nullptr ? vm : &Vm)
    {
        Memory.Add("/Project/Language/Scripts/Test.mscript", source);
        ISourceProvider *providers[] = {&Engine, &Memory};
        Sources = MakeUnique<CompositeSourceProvider>(Span<ISourceProvider *const>(providers));
        CompileOptions.Language = LanguageDialect::Script;
        CompileOptions.DebugOutput = debugOutput;
        Compilation = MakeUnique<LanguageCompilation>(*Sources, CompileOptions);
        if (!Compilation->Load())
        {
            Problems.Add(Compilation->GetDiagnostics().ToString());
            return;
        }
        const ScriptModuleData *engine = Compile("Engine");
        const ScriptModuleData *project = engine != nullptr ? Compile("Project") : nullptr;
        if (project == nullptr)
        {
            return;
        }
        Domain = MakeUnique<ScriptDomain>(*Target, options);
        for (const ScriptModuleData *module : {engine, project})
        {
            String error;
            if (!Domain->Load(*module, error))
            {
                Problems.Add(Move(error));
                return;
            }
        }
        Ok = true;
    }

    const ScriptModuleData *Compile(StringView package)
    {
        MirModule module;
        if (!Compilation->CompileScriptPackage(package, module))
        {
            Problems.Add(Compilation->GetDiagnostics().ToString());
            return nullptr;
        }
        MirTargetRegistry targets;
        RegisterScriptTarget(targets);
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        MirModulePassManager passes;
        String error;
        if (!registry.ParsePipeline(MirScriptPipeline, passes, error))
        {
            Problems.Add(Move(error));
            return nullptr;
        }
        targets.Find(ScriptTargetName)->AddLegalizationPasses(passes, Problems);
        MirModuleAnalysisManager analyses;
        passes.Run(module, analyses);
        ScriptModuleData data;
        if (!Problems.IsEmpty() || !CompileScriptModule(module, {}, data, Problems))
        {
            return nullptr;
        }
        return Target->AddModule(Move(data));
    }

    [[nodiscard]] String Report() const
    {
        StringBuilder<512> report;
        for (const String &problem : Problems)
        {
            report.Append(problem.AsView()).Append('\n');
        }
        return report.ToString();
    }

    static constexpr StringView EngineRoots[] = {"/Engine"};
    VfsSourceProvider Engine{EngineRoots};
    MemorySourceProvider Memory;
    UniquePtr<CompositeSourceProvider> Sources;
    LanguageCompileOptions CompileOptions;
    UniquePtr<LanguageCompilation> Compilation;
    ScriptVM Vm;
    ScriptVM *Target;
    UniquePtr<ScriptDomain> Domain;
    List<String> Problems;
    bool Ok = false;
};
} // namespace Marlin::Tests
