#include "LanguageIR/MirPassManager.h"

#include "Hash/Hash.h"
#include "LanguageIR/MirVerifier.h"
#include "String/Format.h"

namespace Marlin
{
MirAnalysisKey MirCfgAnalyses::Key{"cfg"};
MirAnalysisKey MirModuleAnalysisManager::AllFunctionAnalyses{"all-function-analyses"};

MirPreservedAnalyses MirPreservedAnalyses::All()
{
    MirPreservedAnalyses preserved;
    preserved._all = true;
    return preserved;
}

MirPreservedAnalyses MirPreservedAnalyses::None()
{
    return {};
}

void MirPreservedAnalyses::Preserve(const MirAnalysisKey &key)
{
    if (!_all && !_preserved.Contains(&key))
    {
        _preserved.Add(&key);
    }
}

bool MirPreservedAnalyses::IsPreserved(const MirAnalysisKey &key) const
{
    return _all || _preserved.Contains(&key);
}

void MirPreservedAnalyses::Intersect(const MirPreservedAnalyses &other)
{
    if (other._all)
    {
        return;
    }
    if (_all)
    {
        *this = other;
        return;
    }
    _preserved.RemoveAll([&](const MirAnalysisKey *key) { return !other._preserved.Contains(key); });
}

void MirFunctionAnalysisManager::Invalidate(MirFunction &function, const MirPreservedAnalyses &preserved)
{
    if (preserved.AreAllPreserved())
    {
        return;
    }
    List<CachedResult> *results = _results.Find(&function);
    if (results == nullptr)
    {
        return;
    }
    for (usize i = results->Count(); i-- > 0;)
    {
        if ((*results)[i].Result->IsInvalidatedBy(preserved))
        {
            results->RemoveAtSwap(i);
        }
    }
}

void MirFunctionAnalysisManager::Clear(MirFunction &function)
{
    _results.Remove(&function);
}

void MirFunctionAnalysisManager::Clear()
{
    _results.Clear();
}

void MirModuleAnalysisManager::Invalidate(MirModule &module, const MirPreservedAnalyses &preserved)
{
    if (preserved.AreAllPreserved())
    {
        return;
    }
    _results.RemoveIf([&](const auto &entry) { return entry.Value->IsInvalidatedBy(preserved); });
    if (!preserved.IsPreserved(AllFunctionAnalyses))
    {
        for (MirFunction *function : module.GetFunctions())
        {
            _functions.Invalidate(*function, preserved);
        }
    }
}

void MirFunctionPassManager::AddPass(UniquePtr<IMirFunctionPass> pass)
{
    _passes.Add(Move(pass));
}

MirPreservedAnalyses MirFunctionPassManager::Run(MirFunction &function, MirFunctionAnalysisManager &analyses)
{
    MirPreservedAnalyses combined = MirPreservedAnalyses::All();
    for (UniquePtr<IMirFunctionPass> &pass : _passes)
    {
        const MirPreservedAnalyses preserved = pass->Run(function, analyses);
        analyses.Invalidate(function, preserved);
        combined.Intersect(preserved);
        if (_options.VerifyEach)
        {
            List<String> problems;
            if (!VerifyMirFunction(function, problems))
            {
                for (const String &problem : problems)
                {
                    _errors.Add(Format("after {0}: {1}", pass->GetName(), problem.AsView()));
                    function.GetModule().ReportError(_errors.Last());
                }
                break;
            }
        }
    }
    return combined;
}

void MirModulePassManager::AddPass(UniquePtr<IMirModulePass> pass)
{
    _passes.Add(Move(pass));
}

MirPreservedAnalyses MirModulePassManager::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    MirPreservedAnalyses combined = MirPreservedAnalyses::All();
    for (UniquePtr<IMirModulePass> &pass : _passes)
    {
        const MirPreservedAnalyses preserved = pass->Run(module, analyses);
        analyses.Invalidate(module, preserved);
        combined.Intersect(preserved);
        const List<String> reported = module.TakeErrors();
        if (!reported.IsEmpty())
        {
            _errors.AddRange(reported.AsSpan());
            break;
        }
        if (_options.VerifyEach)
        {
            List<String> problems;
            if (!VerifyMirModule(module, problems))
            {
                for (const String &problem : problems)
                {
                    _errors.Add(Format("after {0}: {1}", pass->GetName(), problem.AsView()));
                }
                break;
            }
        }
    }
    return combined;
}

MirPreservedAnalyses MirModuleToFunctionAdaptor::Run(MirModule &module, MirModuleAnalysisManager &analyses)
{
    MirFunctionAnalysisManager &functionAnalyses = analyses.GetFunctionAnalyses();
    MirPreservedAnalyses combined = MirPreservedAnalyses::All();
    List<MirFunction *> functions;
    functions.AddRange(module.GetFunctions());
    for (MirFunction *function : functions)
    {
        // RECOVERY-DECISION lang-lower-D10: declarations (imported, native and abstract script functions) have no
        // blocks for a function pass to work on.
        if (function->GetBlocks().IsEmpty())
        {
            continue;
        }
        const MirPreservedAnalyses preserved = _pass->Run(*function, functionAnalyses);
        functionAnalyses.Invalidate(*function, preserved);
        combined.Intersect(preserved);
    }
    combined.Preserve(MirModuleAnalysisManager::AllFunctionAnalyses);
    return combined;
}

void MirPassRegistry::RegisterFunctionPass(StringView name, FunctionPassFactory factory)
{
    _entries.Add({String(name.Data(), name.Length()), factory, nullptr});
}

void MirPassRegistry::RegisterModulePass(StringView name, ModulePassFactory factory)
{
    _entries.Add({String(name.Data(), name.Length()), nullptr, factory});
}

const MirPassRegistry::Entry *MirPassRegistry::Find(StringView name) const
{
    for (const Entry &entry : _entries)
    {
        if (entry.Name.AsView() == name)
        {
            return &entry;
        }
    }
    return nullptr;
}

bool MirPassRegistry::ParsePipeline(StringView pipeline, MirModulePassManager &manager, String &error,
                                    MirPassOptions options) const
{
    usize position = 0;
    auto skipSpace = [&] {
        while (position < pipeline.Length() && (pipeline[position] == ' ' || pipeline[position] == '\t'))
        {
            ++position;
        }
    };
    auto readName = [&] {
        skipSpace();
        const usize begin = position;
        while (position < pipeline.Length() && pipeline[position] != ',' && pipeline[position] != '(' &&
               pipeline[position] != ')' && pipeline[position] != ' ')
        {
            ++position;
        }
        return pipeline.Substring(begin, position - begin);
    };
    while (true)
    {
        const StringView name = readName();
        skipSpace();
        if (name.IsEmpty())
        {
            error = Format("expected a pass name at offset {0}", position);
            return false;
        }
        if (name == "function" && position < pipeline.Length() && pipeline[position] == '(')
        {
            ++position;
            auto functions = MakeUnique<MirFunctionPassManager>(options);
            while (true)
            {
                const StringView inner = readName();
                const Entry *entry = Find(inner);
                if (entry == nullptr || entry->Function == nullptr)
                {
                    error = Format("unknown function pass `{0}`", inner);
                    return false;
                }
                functions->AddPass(entry->Function());
                skipSpace();
                if (position < pipeline.Length() && pipeline[position] == ',')
                {
                    ++position;
                    continue;
                }
                if (position < pipeline.Length() && pipeline[position] == ')')
                {
                    ++position;
                    break;
                }
                error = Format("expected `,` or `)` at offset {0}", position);
                return false;
            }
            manager.AddPass(MakeUnique<MirModuleToFunctionAdaptor>(Move(functions)));
        }
        else
        {
            const Entry *entry = Find(name);
            if (entry == nullptr)
            {
                error = Format("unknown pass `{0}`", name);
                return false;
            }
            if (entry->Module != nullptr)
            {
                manager.AddPass(entry->Module());
            }
            else
            {
                manager.AddPass(MakeUnique<MirModuleToFunctionAdaptor>(entry->Function()));
            }
        }
        skipSpace();
        if (position >= pipeline.Length())
        {
            return true;
        }
        if (pipeline[position] != ',')
        {
            error = Format("expected `,` at offset {0}", position);
            return false;
        }
        ++position;
    }
}
} // namespace Marlin
