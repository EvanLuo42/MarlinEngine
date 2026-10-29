//! Runtime reflection: module registration, dynamic types, lookup, value operations and enum access.

#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Reflection/TypeInfo.h"
#include "Reflection/TypeOf.h"
#include "String/StringView.h"
#include "Templates/Function.h"

namespace Marlin
{
class StringWriter;

/// One module of an executable.
struct ModuleTableEntry
{
    /// The module's name.
    const char *Name;
    /// Its generated `Register_<Module>`, or null for a module in a shared library loaded by name.
    void (*Register)();
    /// The shared library that contains the module, or null when `Register` is set.
    const char *Library;
};

/// A default-constructed value of a reflected type in heap storage, destroyed with this object.
class CORE_API ReflectedValue
{
public:
    /// Creates a default-constructed value of `type`.
    ///
    /// # Errors
    /// The value is invalid when the type cannot be default-constructed; check `IsValid`.
    explicit ReflectedValue(const TypeInfo &type);
    /// Destroys the value.
    ~ReflectedValue();
    MARLIN_NONCOPYABLE(ReflectedValue);

    /// Returns whether the value was constructed.
    [[nodiscard]] bool IsValid() const
    {
        return _value != nullptr;
    }

    /// Returns the value, or null when it is invalid.
    [[nodiscard]] void *Get() const
    {
        return _value;
    }

    /// Returns the value's type.
    [[nodiscard]] const TypeInfo &GetType() const
    {
        return _type;
    }

private:
    const TypeInfo &_type;
    void *_value = nullptr;
};

namespace Reflection
{
/// Registers every module of `table` that is not registered yet.
///
/// `table` is in dependency order. The build generates the executable's table; registering it is the launch's job.
///
/// # Errors
/// Loading a module's shared library or finding its registration function is fatal.
CORE_API void RegisterModules(Span<const ModuleTableEntry> table);
/// Returns whether the module `name` is registered.
[[nodiscard]] CORE_API bool IsModuleRegistered(StringView name);

/// Registers a type described at run time, such as a script struct whose layout the VM computed.
///
/// `type` and everything its description points at must outlive the process. Its parent, if any, must already be
/// registered. Returns false, registering nothing, when a type of the same name is registered.
CORE_API bool RegisterDynamicType(TypeInfo &type);

/// Returns the registered type named `name`, or null.
[[nodiscard]] CORE_API const TypeInfo *FindType(StringView name);
/// Returns the registered type whose name hashes to `id`, or null.
[[nodiscard]] CORE_API const TypeInfo *FindType(uint64 id);
/// Calls `visitor` for every registered type, containers included, in registration order.
CORE_API void ForEachType(FunctionRef<void(const TypeInfo &)> visitor);
/// Adds the registered types that derive from or implement `base`; `base` itself is not added.
CORE_API void GetDerivedTypes(const TypeInfo &base, List<const TypeInfo *> &types);

/// Compares two values of `type`.
///
/// Uses the type's `operator==` for non-container types that have one; containers are compared field by field and
/// element by element. Pointers compare what they point to.
[[nodiscard]] CORE_API bool Equals(const TypeInfo &type, const void *a, const void *b);
/// Copies `source` over `target`.
///
/// Assigns non-container types as a whole when they are assignable; containers are copied field by field and element
/// by element. Pointers are deep-copied. Returns false when some part cannot be copied.
CORE_API bool Copy(const TypeInfo &type, void *target, const void *source);

/// Writes `value` for people to read.
///
/// Scalars are written as text, enums by name (`A|B` for flags), and composites as `{Field = value, ...}`, `[a, b]`
/// or `None`.
CORE_API void FormatValue(StringWriter &output, const TypeInfo &type, const void *value);
/// Parses `text` into `value`.
///
/// Accepts a scalar, an enum by name or number, a string, or a type with `TryParse`. Returns false if `text` is not a
/// valid value of `type` or the type is not one of those.
[[nodiscard]] CORE_API bool ParseValue(StringView text, const TypeInfo &type, void *value);

/// Returns the value of an enum object, widened to 64 bits according to its underlying type's signedness.
[[nodiscard]] CORE_API int64 GetEnumValue(const TypeInfo &type, const void *value);
/// Stores `number` in an enum object.
CORE_API void SetEnumValue(const TypeInfo &type, void *value, int64 number);
} // namespace Reflection
} // namespace Marlin
