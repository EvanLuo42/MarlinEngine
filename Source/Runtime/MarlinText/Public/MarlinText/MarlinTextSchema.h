#pragma once

#include "CoreTypes.h"
#include "String/StringView.h"

namespace Marlin
{
class MarlinTextObject;
class MarlinTextDiagnostics;

using MarlinTextUpgradeFunction = void (*)(MarlinTextObject &object);

namespace MarlinTextSchema
{
/// Registers a step that rewrites objects of type *typeName* from schema *from* to *to*; MARLIN_UPGRADE does this
/// through the generated Register_<Module>.
MARLINTEXT_API void RegisterUpgrade(StringView typeName, uint32 from, uint32 to, MarlinTextUpgradeFunction upgrade);

/// Rewrites *object*, written at schema *from*, up to *to* by chaining registered steps, each going as far as it
/// can. False, with an error in *diagnostics*, if no step leads on from some version.
MARLINTEXT_API bool Upgrade(StringView typeName, MarlinTextObject &object, uint32 from, uint32 to,
                            MarlinTextDiagnostics &diagnostics);
} // namespace MarlinTextSchema
} // namespace Marlin

/// Defines a schema upgrade, found and registered by MarlinHeaderTool; it may be in any source of a module that
/// sees the type:
///
///     MARLIN_UPGRADE(TextureSource, 2, 3)
///     {
///         object.Rename("Srgb", "ColorSpace");
///     }
#define MARLIN_UPGRADE(Type, From, To) void MarlinUpgrade_##Type##_##From##_##To(::Marlin::MarlinTextObject &object)
