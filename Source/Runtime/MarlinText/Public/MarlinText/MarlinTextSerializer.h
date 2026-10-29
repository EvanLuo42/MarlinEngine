#pragma once

#include "CoreTypes.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextValue.h"
#include "Misc/Guid.h"
#include "Reflection/TypeInfo.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
class StringWriter;

struct MarlinTextWriteOptions
{
    /// Also write fields that equal the type's default value.
    bool WriteDefaults = false;
    /// Writes the `Path:Name` hint of an object link, such as from the asset index; without it links are `<#UUID>`.
    Function<void(const Guid &guid, StringWriter &output)> WriteLinkHint;
};

struct MarlinTextReadOptions
{
    /// The variables `@if` tests; null when the text may have no conditions.
    const MarlinTextConditions *Conditions = nullptr;
    /// Finds the object a `TypeName : <link>` overrides; without it such objects are errors.
    Function<const MarlinTextObject *(const MarlinTextValue &link)> ResolveParent;
    /// Read objects whose type name differs from the type, such as config sections bound to a type.
    bool IgnoreTypeName = false;
    /// Read only static fields, such as config values, so *target* may be null.
    bool StaticFieldsOnly = false;
    /// Skip static fields, such as when reading another platform's config, which must not change this process's.
    bool InstanceFieldsOnly = false;
};

/// How an opaque type, which reflection knows only by name, is written and read.
struct MarlinTextFormat
{
    void (*Write)(StringWriter &output, const void *value);
    bool (*Read)(const MarlinTextValue &value, void *target, MarlinTextDiagnostics &diagnostics);
};

/// Writes and reads reflected values as MarlinText. Fields marked Transient are neither written nor read; a
/// struct marked Tuple is written `(x, y, z)`; a type with FormatTo and TryParse, such as Guid, is a string.
namespace MarlinTextSerializer
{
MARLINTEXT_API void RegisterFormat(const TypeInfo &type, MarlinTextFormat format);

/// Writes *object* as `TypeName { ... }`, its dynamic type for a class, with `!UUID` and `!Name` when given and
/// `!Schema` when the type's version is above 1.
MARLINTEXT_API void WriteObject(StringWriter &output, const TypeInfo &type, const void *object,
                                const Guid &uuid = Guid(), StringView name = StringView(),
                                const MarlinTextWriteOptions &options = MarlinTextWriteOptions());
/// Writes one value; lines after the first are indented by *indent* levels.
MARLINTEXT_API void WriteValue(StringWriter &output, const TypeInfo &type, const void *value, uint32 indent = 0,
                               const MarlinTextWriteOptions &options = {});

/// Reads *object* into *target* after upgrading it to the type's schema, and after reading the object it overrides,
/// if any. Unknown fields are warnings. False if anything could not be read; what could be is.
MARLINTEXT_API bool ReadObject(const MarlinTextObject &object, const TypeInfo &type, void *target,
                               MarlinTextDiagnostics &diagnostics, const MarlinTextReadOptions &options = {});
MARLINTEXT_API bool ReadValue(const MarlinTextValue &value, const TypeInfo &type, void *target,
                              MarlinTextDiagnostics &diagnostics);

/// Changes object *index* of *document* to hold *object*: fields whose value differs from what the document says,
/// with *readOptions* resolving what it overrides, are rewritten in place, missing ones are added, and everything
/// else, comments included, is left as it was. An object at an older schema is rewritten whole, since its upgrades
/// may have renamed fields.
MARLINTEXT_API bool UpdateObject(MarlinTextDocument &document, usize index, const TypeInfo &type, const void *object,
                                 MarlinTextDiagnostics &diagnostics,
                                 const MarlinTextWriteOptions &options = MarlinTextWriteOptions(),
                                 const MarlinTextReadOptions &readOptions = MarlinTextReadOptions());
} // namespace MarlinTextSerializer
} // namespace Marlin
