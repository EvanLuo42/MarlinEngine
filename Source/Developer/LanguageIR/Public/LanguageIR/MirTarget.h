#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "LanguageIR/MirPassManager.h"
#include "Memory/UniquePtr.h"
#include "String/String.h"

namespace Marlin
{
/// What a target does with an operation (§11.4).
enum class MirLegality : uint8
{
    /// The emitter handles it as it is.
    Legal,
    /// The legalize pass rewrites it into more basic MIR.
    Expand,
    /// The emitter handles it in its own way.
    Custom,
    /// The target cannot express it; the legalize pass reports it.
    Unsupported,
};

/// The kind of operand an operation's legality depends on.
enum class MirOperandClass : uint8
{
    Bool,
    Integer,
    Float,
    Matrix,
    Other,
};

/// The class of *type*: matrices first, then the scalar of a scalar or vector.
[[nodiscard]] LANGUAGEIR_API MirOperandClass GetOperandClass(const MirType *type);

struct MirTargetOptions
{
    /// Keeps what debuggers need to map back to the source.
    bool DebugInfo = true;
};

/// A backend's description of what it can emit and how MIR must be shaped for it, and its emitter. Backends
/// register one each with MirTargetRegistry.
class LANGUAGEIR_API MirTarget
{
public:
    MirTarget() = default;
    virtual ~MirTarget() = default;
    MARLIN_NONCOPYABLE(MirTarget);

    [[nodiscard]] virtual StringView GetName() const = 0;

    /// The legality of *op*, or of intrinsic *intrinsic* when *op* is Intrinsic, on operands of *operandClass*.
    [[nodiscard]] MirLegality GetLegality(MirOp op, MirIntrinsic intrinsic, MirOperandClass operandClass) const;
    [[nodiscard]] MirLegality GetLegality(const MirInstruction &instruction) const;

    /// Whether values of *type* can exist on this target.
    [[nodiscard]] bool IsTypeLegal(const MirType *type) const;

    /// Whether the target has integer matrices; without them the legalize pass rewrites them to arrays of rows.
    [[nodiscard]] bool HasIntegerMatrices() const
    {
        return _integerMatrices;
    }

    [[nodiscard]] bool SupportsStage(MirStage stage) const
    {
        return (_stages & (1u << static_cast<uint32>(stage))) != 0;
    }

    /// The optional features (§9.7) the target can emit.
    [[nodiscard]] Span<const String> GetFeatures() const
    {
        return _features.AsSpan();
    }

    /// Appends the passes that shape MIR for this target, after the shader pipeline. The default adds `legalize`;
    /// errors it finds go to *errors*.
    virtual void AddLegalizationPasses(MirModulePassManager &passes, List<String> &errors) const;

    /// Writes *module*, legalized for this target, in the target's format.
    virtual bool Emit(MirModule &module, const MirTargetOptions &options, List<uint8> &output,
                      List<String> &errors) const = 0;

protected:
    void SetLegality(MirOp op, MirOperandClass operandClass, MirLegality legality);
    void SetLegality(MirIntrinsic intrinsic, MirOperandClass operandClass, MirLegality legality);
    /// Sets the legality of *op* for every operand class.
    void SetLegality(MirOp op, MirLegality legality);
    void SetLegality(MirIntrinsic intrinsic, MirLegality legality);
    void AddStage(MirStage stage);
    void AddFeature(StringView feature);
    /// The scalar widths the target supports, as sets of bit counts: `{16, 32}`.
    void SetScalarWidths(Span<const uint32> integers, Span<const uint32> floats);
    void SetIntegerMatrices(bool supported)
    {
        _integerMatrices = supported;
    }

private:
    struct Key
    {
        uint16 Operation;
        uint8 Class;

        [[nodiscard]] uint64 GetHash() const
        {
            return (uint64{Operation} << 8) | Class;
        }

        bool operator==(const Key &) const = default;
    };

    static uint16 OperationOf(MirOp op, MirIntrinsic intrinsic)
    {
        return op == MirOp::Intrinsic ? static_cast<uint16>(0x100 | static_cast<uint16>(intrinsic))
                                      : static_cast<uint16>(op);
    }

    HashMap<Key, MirLegality> _legality;
    uint32 _stages = 0;
    List<String> _features;
    List<uint32> _integerWidths = {8, 16, 32, 64};
    List<uint32> _floatWidths = {16, 32, 64};
    bool _integerMatrices = true;
};

/// The targets of a compilation, by name. Whoever compiles registers the backends it links, such as
/// `RegisterSpirvTarget(registry)`; nothing registers itself at static initialization.
class LANGUAGEIR_API MirTargetRegistry
{
public:
    MirTargetRegistry() = default;
    MARLIN_NONCOPYABLE(MirTargetRegistry);

    void Register(UniquePtr<MirTarget> target);
    [[nodiscard]] const MirTarget *Find(StringView name) const;

    [[nodiscard]] Span<const UniquePtr<MirTarget>> GetAll() const
    {
        return _targets.AsSpan();
    }

private:
    List<UniquePtr<MirTarget>> _targets;
};

/// The debug output buffer (§9.11), in 32-bit words: the words appended, the words the records may take, the
/// records, then scratch words that records which do not fit are written to instead. A record is its length in
/// words, the hash of its format (MirDebugFormatHash), then each argument: a descriptor (MirDebugArgument kind in
/// bits 0-7, the scalar's bits in 8-15, the components in 16-23) and each component in one word, or two for 64
/// bits, low first; half-precision floats are widened to float.
inline constexpr uint32 MirDebugBufferCount = 0;
inline constexpr uint32 MirDebugBufferCapacity = 1;
inline constexpr uint32 MirDebugBufferHeaderWords = 2;
/// Scratch words after the records: enough for the longest record, four arguments of four 64-bit components.
inline constexpr uint32 MirDebugBufferScratchWords = 64;

/// Names a format in debug output records: the low 32 bits of the XXH3-64 of its text, which the reader of the
/// buffer computes alike from the formats the shaders' reflection lists.
[[nodiscard]] LANGUAGEIR_API uint32 MirDebugFormatHash(StringView format);

enum class MirDebugArgument : uint8
{
    Unsigned,
    Signed,
    Float,
    Bool,
};

/// Rewrites each DebugPrint into an append to the debug output buffer whose address is the last field of the entry
/// point's Uniform global (§9.11), for targets without output of their own; drops them where there is none.
class LANGUAGEIR_API MirExpandDebugPrintPass final : public IMirModulePass
{
public:
    [[nodiscard]] StringView GetName() const override
    {
        return "expand-debug-print";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;
};

/// Rewrites what *target* marks Expand into more basic MIR and reports what it marks Unsupported, as well as
/// types and entry point stages it cannot express (§11.4).
class LANGUAGEIR_API MirLegalizePass final : public IMirModulePass
{
public:
    MirLegalizePass(const MirTarget &target, List<String> &errors) : _target(&target), _errors(&errors)
    {
    }

    [[nodiscard]] StringView GetName() const override
    {
        return "legalize";
    }

    MirPreservedAnalyses Run(MirModule &module, MirModuleAnalysisManager &analyses) override;

private:
    const MirTarget *_target;
    List<String> *_errors;
};
} // namespace Marlin
