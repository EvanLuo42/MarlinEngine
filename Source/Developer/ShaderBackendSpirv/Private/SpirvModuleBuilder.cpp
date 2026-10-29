#include "SpirvModuleBuilder.h"

namespace Marlin
{
SpirvInstruction &SpirvInstruction::Add(StringView text)
{
    uint32 word = 0;
    usize shift = 0;
    for (const char c : text)
    {
        word |= static_cast<uint32>(static_cast<uint8>(c)) << shift;
        shift += 8;
        if (shift == 32)
        {
            _words.Add(word);
            word = 0;
            shift = 0;
        }
    }
    _words.Add(word);
    return *this;
}

void SpirvInstruction::WriteTo(List<uint32> &section) const
{
    section.Add((static_cast<uint32>(_words.Count() + 1) << 16) | static_cast<uint32>(_op));
    section.AddRange(_words.AsSpan());
}

void SpirvModuleBuilder::Capability(spv::Capability capability)
{
    const auto value = static_cast<uint32>(capability);
    if (!_capabilities.Contains(value))
    {
        _capabilities.Add(value);
    }
}

void SpirvModuleBuilder::Extension(StringView extension)
{
    if (!_extensions.Contains(extension))
    {
        _extensions.Add(extension);
    }
}

uint32 SpirvModuleBuilder::ExtendedInstructions(StringView name)
{
    for (const Import &import : _imports)
    {
        if (import.Name == name)
        {
            return import.Id;
        }
    }
    const uint32 id = NewId();
    _imports.Add({name, id});
    return id;
}

void SpirvModuleBuilder::Name(uint32 id, StringView name)
{
    if (_debugInfo && !name.IsEmpty())
    {
        SpirvInstruction(spv::Op::OpName).Add(id).Add(name).WriteTo(DebugNames);
    }
}

void SpirvModuleBuilder::MemberName(uint32 id, uint32 member, StringView name)
{
    if (_debugInfo && !name.IsEmpty())
    {
        SpirvInstruction(spv::Op::OpMemberName).Add(id).Add(member).Add(name).WriteTo(DebugNames);
    }
}

void SpirvModuleBuilder::Decorate(uint32 id, spv::Decoration decoration, Span<const uint32> operands)
{
    SpirvInstruction(spv::Op::OpDecorate).Add(id).Add(decoration).Add(operands).WriteTo(Annotations);
}

void SpirvModuleBuilder::DecorateId(uint32 id, spv::Decoration decoration, Span<const uint32> operands)
{
    SpirvInstruction(spv::Op::OpDecorateId).Add(id).Add(decoration).Add(operands).WriteTo(Annotations);
}

void SpirvModuleBuilder::MemberDecorate(uint32 id, uint32 member, spv::Decoration decoration,
                                        Span<const uint32> operands)
{
    SpirvInstruction(spv::Op::OpMemberDecorate).Add(id).Add(member).Add(decoration).Add(operands).WriteTo(Annotations);
}

void SpirvModuleBuilder::Assemble(List<uint32> &words) const
{
    words.Add(spv::MagicNumber);
    words.Add(0x00010600);
    words.Add(0);
    words.Add(_nextId);
    words.Add(0);
    for (const uint32 capability : _capabilities)
    {
        SpirvInstruction(spv::Op::OpCapability).Add(capability).WriteTo(words);
    }
    for (const StringView extension : _extensions)
    {
        SpirvInstruction(spv::Op::OpExtension).Add(extension).WriteTo(words);
    }
    for (const Import &import : _imports)
    {
        SpirvInstruction(spv::Op::OpExtInstImport).Add(import.Id).Add(import.Name).WriteTo(words);
    }
    // RECOVERY-DECISION lang-spirv-D8: the Vulkan memory model when the content declares it, as cooperative matrices
    // require; GLSL450 otherwise.
    const bool vulkan = _capabilities.Contains(static_cast<uint32>(spv::Capability::VulkanMemoryModel));
    // Buffers are device addresses (MarlinLanguage §4.5), which need the 64-bit physical addressing model.
    const bool addresses = _capabilities.Contains(static_cast<uint32>(spv::Capability::PhysicalStorageBufferAddresses));
    SpirvInstruction(spv::Op::OpMemoryModel)
        .Add(addresses ? spv::AddressingModel::PhysicalStorageBuffer64 : spv::AddressingModel::Logical)
        .Add(vulkan ? spv::MemoryModel::Vulkan : spv::MemoryModel::GLSL450)
        .WriteTo(words);
    words.AddRange(EntryPoints.AsSpan());
    words.AddRange(ExecutionModes.AsSpan());
    words.AddRange(DebugStrings.AsSpan());
    words.AddRange(DebugNames.AsSpan());
    words.AddRange(Annotations.AsSpan());
    words.AddRange(Globals.AsSpan());
    words.AddRange(Functions.AsSpan());
}
} // namespace Marlin
