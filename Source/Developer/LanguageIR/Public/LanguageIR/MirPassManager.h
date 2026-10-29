#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "Hash/Hash.h"
#include "LanguageIR/MirModule.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"

namespace Marlin
{
/// Identifies an analysis or a set of analyses by its address. Each analysis declares one as a static member
/// defined in its .cpp, so it is unique across modules (§11.2).
struct MirAnalysisKey
{
    const char *Name = "";
};

/// What a pass kept valid: everything, nothing, or the analyses and sets it names.
class LANGUAGEIR_API MirPreservedAnalyses
{
public:
    [[nodiscard]] static MirPreservedAnalyses All();
    [[nodiscard]] static MirPreservedAnalyses None();

    void Preserve(const MirAnalysisKey &key);

    template <typename TAnalysis> void Preserve()
    {
        Preserve(TAnalysis::Key);
    }

    [[nodiscard]] bool AreAllPreserved() const
    {
        return _all;
    }

    [[nodiscard]] bool IsPreserved(const MirAnalysisKey &key) const;
    /// Keeps only what both preserve.
    void Intersect(const MirPreservedAnalyses &other);

private:
    bool _all = false;
    List<const MirAnalysisKey *> _preserved;
};

/// Analyses that depend only on the blocks and edges of a function, such as MirControlFlow; a pass that keeps the
/// control flow preserves this set.
struct LANGUAGEIR_API MirCfgAnalyses
{
    static MirAnalysisKey Key;
};

class MirFunctionAnalysisManager;
class MirModuleAnalysisManager;

namespace Private
{
class IMirAnalysisResult
{
public:
    IMirAnalysisResult() = default;
    virtual ~IMirAnalysisResult() = default;
    MARLIN_NONCOPYABLE(IMirAnalysisResult);

    /// Whether the result is stale after a pass that preserved *preserved*.
    [[nodiscard]] virtual bool IsInvalidatedBy(const MirPreservedAnalyses &preserved) const = 0;
};

template <typename TAnalysis> class MirAnalysisResult final : public IMirAnalysisResult
{
public:
    template <typename... TArgs> explicit MirAnalysisResult(TArgs &&...args) : Value(Forward<TArgs>(args)...)
    {
    }

    [[nodiscard]] bool IsInvalidatedBy(const MirPreservedAnalyses &preserved) const override
    {
        if (preserved.IsPreserved(TAnalysis::Key))
        {
            return false;
        }
        if constexpr (TAnalysis::DependsOnlyOnCfg)
        {
            return !preserved.IsPreserved(MirCfgAnalyses::Key);
        }
        return true;
    }

    typename TAnalysis::Result Value;
};
} // namespace Private

/// Computes function analyses on first request and caches them until a pass invalidates them. An analysis is a
/// type with `static MirAnalysisKey Key`, `using Result`, `static constexpr bool DependsOnlyOnCfg` and
/// `static Result Run(MirFunction &, MirFunctionAnalysisManager &)`.
class LANGUAGEIR_API MirFunctionAnalysisManager
{
public:
    MirFunctionAnalysisManager() = default;
    MARLIN_NONCOPYABLE(MirFunctionAnalysisManager);

    template <typename TAnalysis> typename TAnalysis::Result &GetResult(MirFunction &function)
    {
        if (typename TAnalysis::Result *cached = GetCachedResult<TAnalysis>(function))
        {
            return *cached;
        }
        // Run may ask for other results of the function, so the list is looked up again after it.
        auto *result = new Private::MirAnalysisResult<TAnalysis>(TAnalysis::Run(function, *this));
        _results.FindOrAdd(&function).Add({&TAnalysis::Key, UniquePtr<Private::IMirAnalysisResult>(result)});
        return result->Value;
    }

    template <typename TAnalysis> typename TAnalysis::Result *GetCachedResult(MirFunction &function)
    {
        if (List<CachedResult> *results = _results.Find(&function))
        {
            for (CachedResult &cached : *results)
            {
                if (cached.Key == &TAnalysis::Key)
                {
                    return &static_cast<Private::MirAnalysisResult<TAnalysis> *>(cached.Result.Get())->Value;
                }
            }
        }
        return nullptr;
    }

    /// Drops the results of *function* that *preserved* does not keep.
    void Invalidate(MirFunction &function, const MirPreservedAnalyses &preserved);
    /// Drops every result of *function*, as before it is removed.
    void Clear(MirFunction &function);
    void Clear();

private:
    struct CachedResult
    {
        const MirAnalysisKey *Key;
        UniquePtr<Private::IMirAnalysisResult> Result;
    };

    /// The results of each function, a few apiece, so invalidating one function never looks at the others.
    HashMap<const MirFunction *, List<CachedResult>> _results;
};

/// The module-level counterpart of MirFunctionAnalysisManager. It owns the function analysis manager that module
/// passes reach through GetFunctionAnalyses, and passes invalidation of a module pass on to it.
class LANGUAGEIR_API MirModuleAnalysisManager
{
public:
    MirModuleAnalysisManager() = default;
    MARLIN_NONCOPYABLE(MirModuleAnalysisManager);

    template <typename TAnalysis> typename TAnalysis::Result &GetResult(MirModule &module)
    {
        if (UniquePtr<Private::IMirAnalysisResult> *cached = _results.Find(&TAnalysis::Key))
        {
            return static_cast<Private::MirAnalysisResult<TAnalysis> *>(cached->Get())->Value;
        }
        auto *result = new Private::MirAnalysisResult<TAnalysis>(TAnalysis::Run(module, *this));
        _results.Add(&TAnalysis::Key, UniquePtr<Private::IMirAnalysisResult>(result));
        return result->Value;
    }

    [[nodiscard]] MirFunctionAnalysisManager &GetFunctionAnalyses()
    {
        return _functions;
    }

    /// Drops the module results *preserved* does not keep, and every function result unless *preserved* keeps
    /// the set of all function analyses.
    void Invalidate(MirModule &module, const MirPreservedAnalyses &preserved);

    /// Set by a module pass that kept every function analysis valid, such as one that only adds globals.
    static MirAnalysisKey AllFunctionAnalyses;

private:
    HashMap<const MirAnalysisKey *, UniquePtr<Private::IMirAnalysisResult>> _results;
    MirFunctionAnalysisManager _functions;
};

struct MirPassOptions
{
    /// Runs the verifier after every pass and stops at the first failure.
    bool VerifyEach = false;
};

class LANGUAGEIR_API IMirFunctionPass
{
public:
    IMirFunctionPass() = default;
    virtual ~IMirFunctionPass() = default;
    MARLIN_NONCOPYABLE(IMirFunctionPass);

    [[nodiscard]] virtual StringView GetName() const = 0;
    virtual MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) = 0;
};

class LANGUAGEIR_API IMirModulePass
{
public:
    IMirModulePass() = default;
    virtual ~IMirModulePass() = default;
    MARLIN_NONCOPYABLE(IMirModulePass);

    [[nodiscard]] virtual StringView GetName() const = 0;
    virtual MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) = 0;
};

/// Runs function passes in order, invalidating after each.
class LANGUAGEIR_API MirFunctionPassManager final : public IMirFunctionPass
{
public:
    explicit MirFunctionPassManager(MirPassOptions options = {}) : _options(options)
    {
    }

    void AddPass(UniquePtr<IMirFunctionPass> pass);

    [[nodiscard]] StringView GetName() const override
    {
        return "function";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;

    /// Problems the verifier found with VerifyEach, each naming the pass after which it failed.
    [[nodiscard]] Span<const String> GetErrors() const
    {
        return _errors.AsSpan();
    }

private:
    MirPassOptions _options;
    List<UniquePtr<IMirFunctionPass>> _passes;
    List<String> _errors;
};

/// Runs module passes in order, invalidating after each.
class LANGUAGEIR_API MirModulePassManager final : public IMirModulePass
{
public:
    explicit MirModulePassManager(MirPassOptions options = {}) : _options(options)
    {
    }

    void AddPass(UniquePtr<IMirModulePass> pass);

    [[nodiscard]] StringView GetName() const override
    {
        return "module";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;

    [[nodiscard]] Span<const String> GetErrors() const
    {
        return _errors.AsSpan();
    }

private:
    MirPassOptions _options;
    List<UniquePtr<IMirModulePass>> _passes;
    List<String> _errors;
};

/// Runs a function pass on every function of a module.
class LANGUAGEIR_API MirModuleToFunctionAdaptor final : public IMirModulePass
{
public:
    explicit MirModuleToFunctionAdaptor(UniquePtr<IMirFunctionPass> pass) : _pass(Move(pass))
    {
    }

    [[nodiscard]] StringView GetName() const override
    {
        return _pass->GetName();
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;

private:
    UniquePtr<IMirFunctionPass> _pass;
};

/// Passes by name, open to registration: each module that defines passes registers them, and pipelines are built
/// from text such as `function(sroa,mem2reg,constfold,dce),inline,function(simplifycfg)`.
class LANGUAGEIR_API MirPassRegistry
{
public:
    using FunctionPassFactory = UniquePtr<IMirFunctionPass> (*)();
    using ModulePassFactory = UniquePtr<IMirModulePass> (*)();

    void RegisterFunctionPass(StringView name, FunctionPassFactory factory);
    void RegisterModulePass(StringView name, ModulePassFactory factory);

    /// Appends the passes of *pipeline* to *manager*; false with a message in *error* if it does not parse or names
    /// an unknown pass.
    bool ParsePipeline(StringView pipeline, MirModulePassManager &manager, String &error,
                       MirPassOptions options = {}) const;

    /// A registry with the passes of LanguageIR.
    [[nodiscard]] static MirPassRegistry CreateDefault();

private:
    struct Entry
    {
        String Name;
        FunctionPassFactory Function = nullptr;
        ModulePassFactory Module = nullptr;
    };

    [[nodiscard]] const Entry *Find(StringView name) const;

    List<Entry> _entries;
};
} // namespace Marlin
