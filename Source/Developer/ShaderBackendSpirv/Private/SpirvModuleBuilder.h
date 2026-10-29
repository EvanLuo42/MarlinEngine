#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "String/StringView.h"
#include "spirv/unified1/spirv.hpp11"

namespace Marlin
{
/// The words of one SPIR-V instruction being assembled.
class SpirvInstruction
{
public:
    explicit SpirvInstruction(spv::Op op) : _op(op)
    {
    }

    SpirvInstruction &Add(uint32 word)
    {
        _words.Add(word);
        return *this;
    }

    template <typename TEnum>
        requires(__is_enum(TEnum))
    SpirvInstruction &Add(TEnum value)
    {
        return Add(static_cast<uint32>(value));
    }

    /// A literal string, NUL-terminated and padded to whole words.
    SpirvInstruction &Add(StringView text);

    SpirvInstruction &Add(Span<const uint32> words)
    {
        _words.AddRange(words);
        return *this;
    }

    void WriteTo(List<uint32> &section) const;

private:
    spv::Op _op;
    /// Most instructions are a few words; longer ones, such as big composites and strings, go to the heap.
    InlineList<uint32, 16> _words;
};

/// The binary format of a SPIR-V module, independent of MIR: ids, the logical sections in their required order,
/// and the capabilities and extensions the content declares.
class SpirvModuleBuilder
{
public:
    explicit SpirvModuleBuilder(bool debugInfo) : _debugInfo(debugInfo)
    {
    }

    [[nodiscard]] uint32 NewId()
    {
        return _nextId++;
    }

    [[nodiscard]] bool HasDebugInfo() const
    {
        return _debugInfo;
    }

    void Capability(spv::Capability capability);
    void Extension(StringView extension);
    /// The id of extended instruction set *name*, imported on first use.
    uint32 ExtendedInstructions(StringView name);

    void Name(uint32 id, StringView name);
    void MemberName(uint32 id, uint32 member, StringView name);
    void Decorate(uint32 id, spv::Decoration decoration, Span<const uint32> operands = {});
    void DecorateId(uint32 id, spv::Decoration decoration, Span<const uint32> operands);
    void MemberDecorate(uint32 id, uint32 member, spv::Decoration decoration, Span<const uint32> operands = {});

    /// Writes the header and the sections in the order the specification requires.
    void Assemble(List<uint32> &words) const;

    List<uint32> EntryPoints;
    List<uint32> ExecutionModes;
    List<uint32> DebugStrings;
    List<uint32> DebugNames;
    List<uint32> Annotations;
    /// Types, constants and global variables.
    List<uint32> Globals;
    List<uint32> Functions;

private:
    struct Import
    {
        StringView Name;
        uint32 Id;
    };

    bool _debugInfo;
    uint32 _nextId = 1;
    List<uint32> _capabilities;
    List<StringView> _extensions;
    List<Import> _imports;
};
} // namespace Marlin
