#pragma once

#include "Containers/List.h"
#include "LanguageIR/MirType.h"
#include "ScriptVM/ScriptBytecode.h"

namespace Marlin
{
/// The registers a value of *type* takes (MarlinScript §14.1): one for scalars, pointers and references, and its bytes
/// in 16-byte slots for the rest.
[[nodiscard]] uint32 ScriptSlotsOf(const MirType *type);

/// The access that reads a value of *type* from memory into registers.
[[nodiscard]] uint32 ScriptReadAccessOf(const MirType *type);

/// The access that writes a value of *type* to memory; with *barrier*, references go through the write barrier,
/// and a value holding them is Typed with *shape*.
[[nodiscard]] uint32 ScriptWriteAccessOf(const MirType *type, bool barrier, uint32 shape);

/// Whether a value of *type* holds references to GC objects.
[[nodiscard]] bool ScriptHasReferences(const MirType *type);

/// The numeric kind of a scalar.
[[nodiscard]] ScriptNumber ScriptNumberOf(const MirType *scalar);

/// Sets the bits of the 8-byte units that hold object references and managed pointers in a value of *type* at
/// byte *offset*. Pointers to statics are left out: the GC neither moves nor scans them.
void MarkScriptReferences(const MirType *type, uint32 offset, List<uint8> &objects, List<uint8> &managed);
} // namespace Marlin
