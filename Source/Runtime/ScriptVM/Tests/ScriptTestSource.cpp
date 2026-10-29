#include "Testing/Testing.h"

#include "LanguageFrontend/Compilation.h"
#include "LanguageIR/MirPassManager.h"
#include "LanguageIR/MirPasses.h"
#include "ScriptBackend/ScriptTarget.h"
#include "ScriptVM/ScriptDomain.h"

namespace Marlin::Tests
{
namespace
{
struct ScriptTestCase
{
    String Package;
    String Function;
    String Name;
};

/// Compiles the engine's script package once per run and lists its `[Test]` functions (MarlinScript §15).
struct ScriptTestCatalog
{
    ScriptVM Vm;
    List<const ScriptModuleData *> Modules;
    List<UniquePtr<ScriptTestCase>> Cases;
    String Problem;

    static ScriptTestCatalog &Get()
    {
        static ScriptTestCatalog catalog;
        return catalog;
    }

    void Compile()
    {
        static constexpr StringView Roots[] = {"/Engine"};
        VfsSourceProvider sources(Roots);
        LanguageCompileOptions options;
        options.Language = LanguageDialect::Script;
        LanguageCompilation compilation(sources, options);
        if (!compilation.Load())
        {
            Problem = compilation.GetDiagnostics().ToString();
            return;
        }
        MirTargetRegistry targets;
        RegisterScriptTarget(targets);
        const MirPassRegistry registry = MirPassRegistry::CreateDefault();
        for (const StringView package : {StringView("Engine")})
        {
            MirModule module;
            if (!compilation.CompileScriptPackage(package, module))
            {
                Problem = compilation.GetDiagnostics().ToString();
                return;
            }
            MirModulePassManager passes;
            String error;
            List<String> errors;
            if (!registry.ParsePipeline(MirScriptPipeline, passes, error))
            {
                Problem = Move(error);
                return;
            }
            targets.Find(ScriptTargetName)->AddLegalizationPasses(passes, errors);
            MirModuleAnalysisManager analyses;
            passes.Run(module, analyses);
            ScriptModuleData data;
            if (!errors.IsEmpty() || !CompileScriptModule(module, {}, data, errors))
            {
                Problem = errors.IsEmpty() ? String("the script module did not compile") : Move(errors[0]);
                return;
            }
            const ScriptModuleData *added = Vm.AddModule(Move(data));
            Modules.Add(added);
            for (const ScriptFunctionRecord &function : added->Functions)
            {
                if ((function.Flags & ScriptFunctionFlags::Test) == 0)
                {
                    continue;
                }
                const StringView name = added->Strings[function.Name].AsView();
                const usize open = name.IndexOf('(');
                const StringView qualified = open != StringView::NotFound ? name.Substring(0, open) : name;
                const usize dot = qualified.LastIndexOf('.');
                auto test = MakeUnique<ScriptTestCase>();
                test->Package = String(package);
                test->Function = String(name);
                test->Name = String(dot != StringView::NotFound ? qualified.Substring(dot + 1) : qualified);
                Cases.Add(Move(test));
            }
        }
    }
};

void RunScriptTest(const void *context)
{
    const auto &test = *static_cast<const ScriptTestCase *>(context);
    ScriptTestCatalog &catalog = ScriptTestCatalog::Get();
    ScriptDomain domain(catalog.Vm);
    for (const ScriptModuleData *module : catalog.Modules)
    {
        String error;
        if (!domain.Load(*module, error))
        {
            MARLIN_REQUIREF(false, "{0}", error.AsView());
        }
    }
    const ScriptFunctionInfo *function = domain.FindFunction(test.Function.AsView());
    MARLIN_REQUIREF(function != nullptr, "{0} is not in the domain", test.Function.AsView());
    ScriptSlot result[1] = {};
    const ScriptResult<void> outcome = domain.Invoke(*function, {}, Span<ScriptSlot>(result, function->ReturnSlots));
    MARLIN_REQUIREF(outcome, "{0}", outcome ? StringView() : outcome.GetFault().ToString().AsView());
    if (function->Kind != ScriptFunctionKind::Async)
    {
        return;
    }
    ScriptObject *task = nullptr;
    MemCopy(&task, result[0].Bytes, sizeof(task));
    const uint32 handle = domain.CreateHandle(task);
    ScriptTaskStatus status = ScriptTaskStatus::Pending;
    ScriptFaultInfo fault;
    for (uint32 frame = 0; frame < 600 && status == ScriptTaskStatus::Pending; ++frame)
    {
        status = domain.GetTaskStatus(domain.GetHandle(handle), &fault);
        if (status == ScriptTaskStatus::Pending)
        {
            domain.AdvanceTime(1.0 / 60.0);
            domain.BeginFrame();
            domain.RunReady();
        }
    }
    domain.DestroyHandle(handle);
    MARLIN_EXPECTF(status != ScriptTaskStatus::Faulted, "{0}", fault.ToString().AsView());
    MARLIN_EXPECTF(status != ScriptTaskStatus::Pending, "the task did not finish within 600 frames");
    MARLIN_EXPECTF(status != ScriptTaskStatus::Canceled, "the task was canceled");
}

void FailToCompile()
{
    MARLIN_REQUIREF(false, "{0}", ScriptTestCatalog::Get().Problem.AsView());
}
} // namespace

MARLIN_TEST_SOURCE(ScriptTests)
{
    ScriptTestCatalog &catalog = ScriptTestCatalog::Get();
    catalog.Compile();
    if (!catalog.Problem.IsEmpty())
    {
        Testing::RegisterTest({.Module = "Scripts",
                               .Suite = "Engine",
                               .Name = "Compiles",
                               .Tags = {},
                               .Run = &FailToCompile,
                               .RunWith = nullptr,
                               .Context = nullptr,
                               .File = __FILE__,
                               .Line = __LINE__});
        return;
    }
    for (const UniquePtr<ScriptTestCase> &test : catalog.Cases)
    {
        Testing::RegisterTest({.Module = "Scripts",
                               .Suite = test->Package.AsView(),
                               .Name = test->Name.AsView(),
                               .Tags = {},
                               .Run = nullptr,
                               .RunWith = &RunScriptTest,
                               .Context = test.Get(),
                               .File = test->Function.AsView(),
                               .Line = 0});
    }
}
} // namespace Marlin::Tests

