#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Diagnostics/Log.h"
#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextValue.h"
#include "Memory/UniquePtr.h"
#include "Reflection/TypeOf.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Templates/Function.h"

MARLIN_DECLARE_LOG_CATEGORY(CONFIG_API, LogConfig, Info, Warning);

namespace Marlin
{
/// Where config comes from. *Directories* are virtual and in rising priority, such as /Engine/Config and
/// /Project/Config; platform layers merge in through the mounts under each. *UserDirectories*, such as /User/Config,
/// are the ones players can write, read after them. *Overrides* are `File.Section.Key=Value` from `-Config:` switches,
/// applied last.
struct ConfigDesc
{
    List<String> Directories;
    List<String> UserDirectories;
    List<String> Overrides;
    /// Shipping (Documentation/Architecture/06 §1.4): user directories and overrides reach only sections whose type
    /// is marked `MCLASS(Config="...", UserConfigurable)` and has no field marked `Flags=Cheat`; other sections and
    /// raw keys read only Directories.
    bool RestrictUser = false;
    /// What `@if` tests, such as Platform, Configuration and Target.
    List<MarlinTextConditions::Variable> Variables;
};

struct ConfigVariable
{
    String Name;
    String Value;
};

/// A platform and target that data is converted for, as config sees it.
struct ConfigTarget
{
    /// Such as PS5: whose platform layers are read, and the value of `@if Platform`.
    String Platform;
    /// The other variables `@if` tests, such as Target; any not listed is undefined, so tests of it fail.
    List<ConfigVariable> Variables;
};

/// Config as one target sees it (Documentation/Architecture/06 §3.2): the process's directories and overrides,
/// but for each directory its own file and then, lowest first, the target platform's layers
/// (`<Root>/Platforms/<Layer>/Config`, read by path) instead of the layers this process mounted, with the target's
/// variables. Made by Config::GetView and kept for the process; Reload empties it. Thread-safe.
class CONFIG_API ConfigView
{
public:
    ~ConfigView();
    MARLIN_NONCOPYABLE(ConfigView);

    /// Visits section *section* in each layer that has it, lowest first, with the file it came from.
    void ForEachSection(StringView section,
                        FunctionRef<void(const MarlinTextObject &object, StringView source)> visitor) const;
    /// Applies section *section* to the instance fields of *object* of *type*, layer by layer; static fields, which
    /// belong to this process, are left alone. Problems are logged.
    bool Apply(StringView section, const TypeInfo &type, void *object) const;
    /// Every virtual path that makes up config file *file*, such as Engine, in this view, lowest first, whether it
    /// exists or not: the files whose changes change what the view reads.
    void GetSourcePaths(StringView file, List<String> &paths) const;

    struct State;

private:
    friend struct ConfigViews;
    explicit ConfigView(UniquePtr<State> state);

    UniquePtr<State> _state;
};

/// Config is MarlinText. Section "Engine.Renderer" is object `Renderer` in Engine.mtext of every config directory;
/// reading a section applies each layer in turn, so later layers override fields, `+=` and `-=` edit arrays and
/// `@if` blocks pick by platform. Types bind with `MCLASS(Config="Engine.Renderer")`.
namespace Config
{
CONFIG_API void Initialize(const ConfigDesc &desc);
CONFIG_API void Shutdown();
[[nodiscard]] CONFIG_API bool IsInitialized();

/// Reads the files again and applies them to every config type's static fields.
CONFIG_API void Reload();

/// Visits section *section* in each layer that has it, lowest first, with the file it came from.
CONFIG_API void ForEachSection(StringView section,
                               FunctionRef<void(const MarlinTextObject &object, StringView source)> visitor);

/// ForEachSection for readers that walk the raw fields instead of applying a type: also passes what the fields'
/// `@if` conditions are tested against (MarlinTextConditions::Holds).
CONFIG_API void ForEachSectionWithConditions(
    StringView section,
    FunctionRef<void(const MarlinTextObject &object, const MarlinTextConditions &conditions, StringView source)>
        visitor);

/// Applies section *section* to *object* of *type*, layer by layer; problems are logged. A null *object* reads
/// only static fields.
CONFIG_API bool Apply(StringView section, const TypeInfo &type, void *object);

/// Applies a type's own section, from its Config metadata.
CONFIG_API bool Apply(const TypeInfo &type, void *object);

template <typename T> bool Apply(T &object)
{
    return Apply(TypeOf<T>(), &object);
}

/// Applies every registered config type's section to its static fields.
CONFIG_API void ApplyStatics();

/// The last value of *key* ("Engine.Renderer.ShadowQuality") assigned in any layer, read as *type*.
CONFIG_API bool TryGet(StringView key, const TypeInfo &type, void *value);

template <typename T> bool TryGet(StringView key, T &value)
{
    return TryGet(key, TypeOf<T>(), &value);
}

template <typename T> T GetOr(StringView key, T fallback)
{
    T value = fallback;
    return TryGet(key, value) ? value : fallback;
}

/// The platform layers of *platform*, most specific first, as the toolkit defines them for every platform
/// (Documentation/Architecture/02 §5.4); false if this build knows no such platform.
CONFIG_API bool GetPlatformLayers(StringView platform, List<String> &layers);

/// The view of *target*, made on first use and kept for the process; null if its platform is unknown or config is
/// not initialized.
[[nodiscard]] CONFIG_API const ConfigView *GetView(const ConfigTarget &target);
} // namespace Config
} // namespace Marlin
