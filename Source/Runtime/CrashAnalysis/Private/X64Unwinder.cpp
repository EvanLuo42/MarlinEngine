#include "X64Unwinder.h"

#include "Memory/MemoryOps.h"
#include "PeImage.h"

namespace Marlin
{
namespace
{
constexpr usize ContextGprOffset = 0x78;
constexpr usize ContextRipOffset = 0xF8;
constexpr uint32 Rsp = 4;
constexpr uint8 ChainInfoFlag = 0x4;
constexpr uint32 MaxChain = 32;

enum class UnwindOp : uint8
{
    PushNonVolatile = 0,
    AllocLarge = 1,
    AllocSmall = 2,
    SetFramePointer = 3,
    SaveNonVolatile = 4,
    SaveNonVolatileFar = 5,
    Epilog = 6,
    Spare = 7,
    SaveXmm128 = 8,
    SaveXmm128Far = 9,
    PushMachineFrame = 10,
};

bool ReadQword(X64MemoryReader read, uint64 address, uint64 &value)
{
    return read(address, &value, sizeof(value)) == sizeof(value);
}

/// How many unwind code slots an operation takes.
uint32 SlotsOf(uint8 op, uint8 info)
{
    switch (static_cast<UnwindOp>(op))
    {
    case UnwindOp::AllocLarge:
        return info == 0 ? 2 : 3;
    case UnwindOp::SaveNonVolatile:
    case UnwindOp::SaveXmm128:
    case UnwindOp::Epilog:
        return 2;
    case UnwindOp::SaveNonVolatileFar:
    case UnwindOp::SaveXmm128Far:
    case UnwindOp::Spare:
        return 3;
    default:
        return 1;
    }
}

/// If the code at the instruction pointer is an epilog, finishes it: the stack adjustment, the pops and the return.
/// Epilogs have no unwind codes, so this reads the instructions themselves.
bool UnwindEpilog(X64Registers &registers, const PeImage &image, uint64 base, uint8 frameRegister, X64MemoryReader read)
{
    uint8 code[64] = {};
    const usize size = image.Read(registers.Rip - base, code, sizeof(code));
    usize at = 0;
    const auto byteAt = [&](usize index) -> uint8 { return index < size ? code[index] : 0; };

    // First pass: is this an epilog? Optional add rsp or lea rsp, pops, then ret or a jump out.
    bool adjusted = false;
    int64 adjustment = 0;
    bool fromFrame = false;
    const uint8 rex = byteAt(at);
    if ((rex & 0xF8) == 0x48 && byteAt(at + 1) == 0x83 && byteAt(at + 2) == 0xC4)
    {
        adjustment = static_cast<int8>(byteAt(at + 3));
        at += 4;
        adjusted = true;
    }
    else if ((rex & 0xF8) == 0x48 && byteAt(at + 1) == 0x81 && byteAt(at + 2) == 0xC4)
    {
        int32 immediate = 0;
        MemCopy(&immediate, code + at + 3, sizeof(immediate));
        adjustment = immediate;
        at += 7;
        adjusted = true;
    }
    else if ((rex & 0xF8) == 0x48 && byteAt(at + 1) == 0x8D && frameRegister != 0 &&
             (byteAt(at + 2) & 0x38) == (Rsp << 3) && ((byteAt(at + 2) & 7) | ((rex & 1) << 3)) == frameRegister)
    {
        const uint8 mod = byteAt(at + 2) >> 6;
        if (mod == 1)
        {
            adjustment = static_cast<int8>(byteAt(at + 3));
            at += 4;
        }
        else if (mod == 2)
        {
            int32 displacement = 0;
            MemCopy(&displacement, code + at + 3, sizeof(displacement));
            adjustment = displacement;
            at += 7;
        }
        else
        {
            return false;
        }
        adjusted = true;
        fromFrame = true;
    }
    const usize pops = at;
    while (true)
    {
        const uint8 byte = byteAt(at);
        if ((byte & 0xF8) == 0x58)
        {
            ++at;
        }
        else if ((byte & 0xF0) == 0x40 && (byteAt(at + 1) & 0xF8) == 0x58)
        {
            at += 2;
        }
        else
        {
            break;
        }
    }
    const uint8 end = byteAt(at);
    const bool returns = end == 0xC3 || end == 0xC2 || (end == 0xF3 && byteAt(at + 1) == 0xC3);
    const bool jumps = end == 0xE9 || end == 0xEB || (end == 0xFF && byteAt(at + 1) == 0x25) ||
                       ((end & 0xF0) == 0x40 && byteAt(at + 1) == 0xFF && byteAt(at + 2) == 0x25);
    if (!returns && !(jumps && (adjusted || at > pops)))
    {
        return false;
    }

    // Second pass: emulate it.
    X64Registers next = registers;
    if (adjusted)
    {
        next.Gpr[Rsp] =
            static_cast<uint64>(static_cast<int64>(fromFrame ? next.Gpr[frameRegister] : next.Gpr[Rsp]) + adjustment);
    }
    at = pops;
    while (at < size)
    {
        uint32 reg = 0;
        if ((code[at] & 0xF8) == 0x58)
        {
            reg = code[at] & 7;
            ++at;
        }
        else if ((code[at] & 0xF0) == 0x40 && at + 1 < size && (code[at + 1] & 0xF8) == 0x58)
        {
            reg = (code[at + 1] & 7) | ((code[at] & 1) << 3);
            at += 2;
        }
        else
        {
            break;
        }
        if (!ReadQword(read, next.Gpr[Rsp], next.Gpr[reg]))
        {
            return false;
        }
        next.Gpr[Rsp] += 8;
    }
    if (!ReadQword(read, next.Gpr[Rsp], next.Rip))
    {
        return false;
    }
    next.Gpr[Rsp] += 8;
    registers = next;
    return true;
}
} // namespace

bool ReadX64Context(Span<const uint8> context, X64Registers &registers)
{
    if (context.Count() < ContextRipOffset + sizeof(uint64))
    {
        return false;
    }
    MemCopy(registers.Gpr, context.Data() + ContextGprOffset, sizeof(registers.Gpr));
    MemCopy(&registers.Rip, context.Data() + ContextRipOffset, sizeof(registers.Rip));
    return true;
}

bool UnwindX64(X64Registers &registers, X64MemoryReader read, X64ImageFinder find)
{
    uint64 base = 0;
    const PeImage *image = find(registers.Rip, base);
    PeRuntimeFunction function;
    if (image == nullptr || !image->FindFunction(static_cast<uint32>(registers.Rip - base), function))
    {
        // A leaf function, or code without unwind data: the return address is on top of the stack.
        const uint64 stack = registers.Gpr[Rsp];
        if (!ReadQword(read, stack, registers.Rip))
        {
            return false;
        }
        registers.Gpr[Rsp] = stack + 8;
        return registers.Rip != 0;
    }

    X64Registers next = registers;
    const uint64 offset = registers.Rip - base - function.Begin;
    bool machineFrame = false;
    for (uint32 chain = 0; chain < MaxChain; ++chain)
    {
        uint8 header[4] = {};
        if (image->Read(function.UnwindInfo, header, sizeof(header)) != sizeof(header))
        {
            return false;
        }
        const uint8 flags = header[0] >> 3;
        const uint8 prologSize = header[1];
        const uint8 codeCount = header[2];
        const uint8 frameRegister = header[3] & 0xF;
        const uint64 frameOffset = static_cast<uint64>(header[3] >> 4) * 16;
        uint16 codes[256] = {};
        const usize codeBytes = ((codeCount + 1u) & ~1u) * sizeof(uint16);
        if (image->Read(function.UnwindInfo + 4, codes, codeBytes) != codeBytes)
        {
            return false;
        }
        // Only the primary entry's prolog may be partly executed; chained entries describe what ran before.
        const bool primary = chain == 0;
        if (primary && offset >= prologSize && UnwindEpilog(next, *image, base, frameRegister, read))
        {
            registers = next;
            return registers.Rip != 0;
        }
        uint64 frame = next.Gpr[Rsp];
        for (uint32 i = 0; i < codeCount; ++i)
        {
            const uint8 codeOffset = codes[i] & 0xFF;
            const uint8 op = (codes[i] >> 8) & 0xF;
            if (static_cast<UnwindOp>(op) == UnwindOp::SetFramePointer && (!primary || offset >= codeOffset))
            {
                frame = next.Gpr[frameRegister] - frameOffset;
            }
        }
        for (uint32 i = 0; i < codeCount;)
        {
            const uint8 codeOffset = codes[i] & 0xFF;
            const uint8 op = (codes[i] >> 8) & 0xF;
            const uint8 info = codes[i] >> 12;
            const uint32 slots = SlotsOf(op, info);
            if (primary && offset < codeOffset && offset < prologSize)
            {
                i += slots;
                continue;
            }
            switch (static_cast<UnwindOp>(op))
            {
            case UnwindOp::PushNonVolatile:
                if (!ReadQword(read, next.Gpr[Rsp], next.Gpr[info]))
                {
                    return false;
                }
                next.Gpr[Rsp] += 8;
                break;
            case UnwindOp::AllocLarge:
                next.Gpr[Rsp] +=
                    info == 0 ? uint64{codes[i + 1]} * 8 : (uint64{codes[i + 1]} | (uint64{codes[i + 2]} << 16));
                break;
            case UnwindOp::AllocSmall:
                next.Gpr[Rsp] += uint64{info} * 8 + 8;
                break;
            case UnwindOp::SetFramePointer:
                next.Gpr[Rsp] = next.Gpr[frameRegister] - frameOffset;
                break;
            case UnwindOp::SaveNonVolatile:
                if (!ReadQword(read, frame + uint64{codes[i + 1]} * 8, next.Gpr[info]))
                {
                    return false;
                }
                break;
            case UnwindOp::SaveNonVolatileFar:
                if (!ReadQword(read, frame + (uint64{codes[i + 1]} | (uint64{codes[i + 2]} << 16)), next.Gpr[info]))
                {
                    return false;
                }
                break;
            case UnwindOp::PushMachineFrame: {
                const uint64 stack = next.Gpr[Rsp] + (info != 0 ? 8 : 0);
                if (!ReadQword(read, stack, next.Rip) || !ReadQword(read, stack + 24, next.Gpr[Rsp]))
                {
                    return false;
                }
                machineFrame = true;
                break;
            }
            default:
                break;
            }
            i += slots;
        }
        if ((flags & ChainInfoFlag) == 0)
        {
            break;
        }
        uint32 chained[3] = {};
        const usize at = function.UnwindInfo + 4 + codeBytes;
        if (image->Read(at, chained, sizeof(chained)) != sizeof(chained))
        {
            return false;
        }
        function = {.Begin = chained[0], .End = chained[1], .UnwindInfo = chained[2]};
    }
    if (!machineFrame)
    {
        if (!ReadQword(read, next.Gpr[Rsp], next.Rip))
        {
            return false;
        }
        next.Gpr[Rsp] += 8;
    }
    registers = next;
    return registers.Rip != 0;
}
} // namespace Marlin
