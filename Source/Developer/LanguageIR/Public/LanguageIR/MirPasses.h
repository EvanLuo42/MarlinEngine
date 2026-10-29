#pragma once

#include "Containers/HashSet.h"
#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirPassManager.h"

namespace Marlin
{
/// The predecessors and dominator tree of a function (MirControlFlow).
struct LANGUAGEIR_API MirControlFlowAnalysis
{
    static MirAnalysisKey Key;
    static constexpr bool DependsOnlyOnCfg = true;
    using Result = MirControlFlow;

    static Result Run(MirFunction &function, MirFunctionAnalysisManager &analyses);
};

/// The values that may differ between the invocations of a draw or dispatch: stage inputs, results of atomics
/// and everything that depends on them, including through control flow.
struct LANGUAGEIR_API MirUniformityAnalysis
{
    static MirAnalysisKey Key;
    static constexpr bool DependsOnlyOnCfg = false;

    struct Result
    {
        HashSet<const MirValue *> NonUniform;

        [[nodiscard]] bool IsNonUniform(const MirValue *value) const
        {
            return NonUniform.Contains(value);
        }
    };

    static Result Run(MirFunction &function, MirFunctionAnalysisManager &analyses);
};

/// Whether *instruction* has an effect beyond its result, so it stays even when its result is unused.
[[nodiscard]] LANGUAGEIR_API bool HasSideEffects(const MirInstruction &instruction);

/// Rewrites a function with several `Return`s, none inside a loop, to one structured exit: a single-iteration loop
/// whose returns become breaks.
class LANGUAGEIR_API MirMergeReturnPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "mergereturn";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Inlines every call to an AlwaysInline function with a single `Return`, callees first, and removes functions
/// nothing calls that are not entry points.
class LANGUAGEIR_API MirInlinePass final : public IMirModulePass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "inline";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;
};

/// Splits function variables of struct, array or vector type that are only reached through constant indices
/// into a variable per element. With *keepWholeVectors* ("sroa-keepvectors"), a vector variable only ever loaded and
/// stored whole stays one, for the script VM, whose registers hold a vector of up to 128 bits.
class LANGUAGEIR_API MirSroaPass final : public IMirFunctionPass
{
public:
    explicit MirSroaPass(bool keepWholeVectors = false) : _keepWholeVectors(keepWholeVectors)
    {
    }

    [[nodiscard]] StringView GetName() const override
    {
        return _keepWholeVectors ? "sroa-keepvectors" : "sroa";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;

private:
    bool _keepWholeVectors;
};

/// Promotes function variables that are only loaded and stored to SSA values, with phis at dominance frontiers.
class LANGUAGEIR_API MirMem2RegPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "mem2reg";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Replaces operations on constants with their results.
class LANGUAGEIR_API MirConstantFoldPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "constfold";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Turns branches and switches on constants into jumps, dropping the merge of the selection they headed.
class LANGUAGEIR_API MirDeadBranchPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "deadbranch";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Replaces an operation by an identical one that dominates it, and copies by their source.
class LANGUAGEIR_API MirCsePass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "cse";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Removes instructions and variables whose results nothing with an effect uses.
class LANGUAGEIR_API MirDcePass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "dce";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Removes unreachable blocks. One that is still the target of a reachable construct stays, as structured control
/// flow requires: a merge block holding only `Unreachable`, a continue block holding only the back edge.
class LANGUAGEIR_API MirSimplifyCfgPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "simplifycfg";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// Replaces each ForwardDerivative and BackwardDerivative with a call to the forward or reverse derivative it
/// generates (§9.12); it runs after inlining, so the differentiated function is selections and loops, and calls only
/// to functions with custom derivatives. Then it replaces each Detach with its operand and clears the custom
/// derivatives, so the next inline pass inlines their functions.
class LANGUAGEIR_API MirAutodiffPass final : public IMirModulePass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "autodiff";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;
};

/// Checks every `Compiler.Require`: its condition must have folded to true, as compile-time properties of the code
/// such as a Surface's shape do once the code is inlined and folded (MarlinLanguage.md §9.4); reports the message
/// otherwise, then removes them all.
class LANGUAGEIR_API MirRequirePass final : public IMirModulePass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "require";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;
};

/// Fully unrolls the `[Unroll]` loops whose trip count is a constant (§11.2): a `for`/`while` loop that leaves only
/// through its condition, comparing an induction variable that starts at a constant and steps by a constant against a
/// constant. Other loops keep the request for the target to pass on.
class LANGUAGEIR_API MirLoopUnrollPass final : public IMirFunctionPass
{
public:
    /// The largest trip count unrolled fully.
    static constexpr uint32 MaxTripCount = 1024;

    [[nodiscard]] StringView GetName() const override
    {
        return "unroll";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};

/// The number of times the body of the loop *header* starts runs, when the loop has the shape MirLoopUnrollPass
/// unrolls and the count is a constant within its limit; false otherwise.
LANGUAGEIR_API bool GetConstantTripCount(MirBlock *header, const MirControlFlow &flow, uint32 &tripCount);

/// Marks resource accesses whose handle may differ between invocations as NonUniform (§4.4).
class LANGUAGEIR_API MirNonUniformPass final : public IMirFunctionPass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "nonuniform";
    }

    MirPreservedAnalyses Run(MirFunction &function, MirFunctionAnalysisManager &analyses) override;
};


/// The pipeline script modules are compiled with.
inline constexpr StringView MirScriptPipeline =
    "inline,function(sroa-keepvectors,mem2reg,constfold,deadbranch,simplifycfg,cse,dce)";

/// The pipeline shaders are compiled with.
inline constexpr StringView MirShaderPipeline =
    "function(mergereturn),inline,function(sroa,mem2reg,constfold,deadbranch,simplifycfg,unroll,constfold,deadbranch,"
    "simplifycfg,sroa,mem2reg,constfold,deadbranch,simplifycfg),autodiff,inline,function(sroa,mem2reg,constfold,deadbranch,simplifycfg,cse,dce,nonuniform),require";
} // namespace Marlin
