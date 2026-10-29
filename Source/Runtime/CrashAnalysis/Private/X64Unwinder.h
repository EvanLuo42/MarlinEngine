#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Templates/Function.h"

namespace Marlin
{
class PeImage;

/// The integer registers in the order of x64 unwind codes and of a Windows CONTEXT: rax, rcx, rdx, rbx, rsp, rbp,
/// rsi, rdi, r8 to r15.
struct X64Registers
{
    uint64 Gpr[16] = {};
    uint64 Rip = 0;
};

/// Copies the dumped memory at an address; returns how many bytes it could.
using X64MemoryReader = FunctionRef<usize(uint64 address, void *buffer, usize size)>;
/// The image that holds an address and its base, or null.
using X64ImageFinder = FunctionRef<const PeImage *(uint64 address, uint64 &base)>;

/// Reads the registers out of a Windows x64 CONTEXT.
bool ReadX64Context(Span<const uint8> context, X64Registers &registers);
/// Moves *registers* from a frame to its caller with the image's unwind data; false when the walk cannot go on.
bool UnwindX64(X64Registers &registers, X64MemoryReader read, X64ImageFinder find);
} // namespace Marlin
