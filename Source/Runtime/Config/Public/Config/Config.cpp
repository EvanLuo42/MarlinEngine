#include "Config/Config.h"

#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformSync.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "Memory/MemoryOps.h"
#include "Memory/UniquePtr.h"
#include "Reflection/Reflection.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Vfs/Vfs.h"

MARLIN_DEFINE_LOG_CATEGORY(LogConfig);

namespace Marlin
{
namespace
{
struct ConfigLayer
{
    UniquePtr<MarlinTextDocument> Document;
    String Source;
    /// From a user directory or an override: what players can write.
    bool User = false;
};

/// An object of a section in one layer, and the file it came from.
struct ConfigSectionEntry
{
    const MarlinTextObject *Object = nullptr;
    StringView Source;
};

struct ConfigFiles
{
    /// Parsed layers by file name, lowest first, read on first use.
    HashMap<String, List<ConfigLayer>> Layers;
    /// What each section read so far resolved to, layer by layer: its `@if` blocks tested and its objects parsed
    /// once. Points into Layers, so they are cleared together.
    HashMap<String, List<ConfigSectionEntry>> Sections;

    void Clear()
    {
        Sections.Clear();
        Layers.Clear();
    }
};

struct ConfigState
{
    Mutex Lock;
    bool Initialized = false;
    ConfigDesc Desc;
    /// What user layers may set while Desc.RestrictUser is on; from FindUserSections.
    List<String> UserSections;
    ConfigFiles Files;
    /// Views by GetView's key; never destroyed while the process runs, so their pointers stay valid.
    HashMap<String, UniquePtr<ConfigView>> Views;

    /// Built on first use and never destroyed.
    static ConfigState &Get()
    {
        alignas(ConfigState) static uint8 storage[sizeof(ConfigState)];
        static ConfigState *state = ConstructAt(reinterpret_cast<ConfigState *>(storage));
        return *state;
    }
};

/// Splits "Engine.Renderer" into the file Engine and the section Renderer.
bool SplitSection(StringView section, StringView &file, StringView &name)
{
    return section.SplitFirst('.', file, name) && !file.IsEmpty() && !name.IsEmpty();
}

void Report(const MarlinTextDiagnostics &diagnostics, StringView source)
{
    for (const MarlinTextDiagnostic &diagnostic : diagnostics.GetAll())
    {
        if (diagnostic.IsError)
        {
            MARLIN_LOG(LogConfig, Error, "{0}({1}): {2}", source, diagnostic.Line, diagnostic.Message);
        }
        else
        {
            MARLIN_LOG(LogConfig, Warning, "{0}({1}): {2}", source, diagnostic.Line, diagnostic.Message);
        }
    }
}

void AddLayer(List<ConfigLayer> &layers, StringView text, StringView source, bool user)
{
    ConfigLayer layer{MakeUnique<MarlinTextDocument>(), String(source), user};
    MarlinTextDiagnostics diagnostics;
    MarlinTextParseOptions options;
    options.AllowConditions = true;
    if (layer.Document->Parse(text, diagnostics, options))
    {
        layers.Add(Move(layer));
    }
    Report(diagnostics, source);
}

/// Adds *path* as read from the lowest layer of its mount point alone, the folder itself rather than a platform layer
/// mounted over it.
void AddOwnLayer(List<ConfigLayer> &layers, StringView path, bool user)
{
    List<MountId> mounts;
    Vfs::GetLayers(path, mounts);
    String text;
    if (!mounts.IsEmpty() && Vfs::ReadAll(mounts.Last(), path, text))
    {
        AddLayer(layers, text.AsView(), path, user);
    }
}

/// The sections user layers may set while they are restricted: those of types marked UserConfigurable, unless the
/// type has a cheat, which players must never set.
List<String> FindUserSections()
{
    List<String> sections;
    Reflection::ForEachType([&](const TypeInfo &type) {
        const Optional<StringView> section = type.GetMetadata().Get("Config");
        if (!section.HasValue() || !type.GetMetadata().Has("UserConfigurable"))
        {
            return;
        }
        for (const FieldInfo *field : type.GetFields())
        {
            if (field->GetMetadata().HasFlag("Flags", "Cheat"))
            {
                MARLIN_LOG(LogConfig, Error, "{0} is UserConfigurable but its {1} is a cheat; players cannot set {2}",
                           type.GetName(), field->GetName(), section.Value());
                return;
            }
        }
        sections.Add(String(section.Value()));
    });
    return sections;
}

/// Whether user layers reach *section*.
bool UserMaySet(bool restrict, const List<String> &userSections, StringView section)
{
    return !restrict || userSections.Contains(section);
}

/// The folder of *directory*'s platform layer *layer*: /Engine/Config in layer PS5 is /Engine/Platforms/PS5/Config.
bool LayerDirectory(StringView directory, StringView layer, StringWriter &path)
{
    constexpr StringView Folder = "/Config";
    if (!directory.EndsWith(Folder))
    {
        return false;
    }
    path.Append(directory.Substring(0, directory.Length() - Folder.Length()))
        .Append("/Platforms/")
        .Append(layer)
        .Append(Folder);
    return true;
}

void AddOverrides(List<ConfigLayer> &layers, Span<const String> overrides, StringView file)
{
    StringBuilder<1024> text;
    for (const String &entry : overrides)
    {
        const StringView assignment = entry.AsView();
        StringView target;
        StringView value;
        StringView entryFile;
        StringView rest;
        StringView section;
        StringView key;
        if (!assignment.SplitFirst('=', target, value) || !target.SplitFirst('.', entryFile, rest) ||
            !rest.SplitLast('.', section, key))
        {
            MARLIN_LOG(LogConfig, Error, "-Config:{0} is not File.Section.Key=Value", entry);
            continue;
        }
        if (entryFile == file)
        {
            text.Append(section).Append(" { ").Append(key).Append(" = ").Append(value).Append(" }\n");
        }
    }
    if (!text.IsEmpty())
    {
        AddLayer(layers, text.AsView(), "-Config", true);
    }
}

/// The layers of *file* in the mounted layers of *desc*'s directories, read on first use. Called with the owner's
/// lock held.
List<ConfigLayer> &MountedLayersOf(ConfigFiles &files, const ConfigDesc &desc, StringView file)
{
    if (List<ConfigLayer> *layers = files.Layers.Find(file))
    {
        return *layers;
    }
    List<ConfigLayer> &layers = files.Layers.FindOrAdd(String(file));
    const auto read = [&](const List<String> &directories, bool user) {
        for (const String &directory : directories)
        {
            StringBuilder<256> path(directory.AsView());
            path.Append('/').Append(file).Append(".mtext");
            Vfs::ReadEachLayer(path.AsView(),
                               [&](StringView text, StringView source) { AddLayer(layers, text, source, user); });
        }
    };
    read(desc.Directories, false);
    read(desc.UserDirectories, true);
    AddOverrides(layers, desc.Overrides.AsSpan(), file);
    return layers;
}

/// Collects section *name* of *layers* where *conditions* hold, skipping user layers unless *user* allows them. Called
/// with the owner's lock held.
void CollectSection(List<ConfigLayer> &layers, const MarlinTextConditions &conditions, StringView name, bool user,
                    List<ConfigSectionEntry> &entries)
{
    List<usize> active;
    for (ConfigLayer &layer : layers)
    {
        if (layer.User && !user)
        {
            continue;
        }
        active.Clear();
        layer.Document->GetActiveObjects(conditions, active);
        for (const usize index : active)
        {
            MarlinTextDiagnostics diagnostics;
            const MarlinTextObject *object = layer.Document->GetObject(index, diagnostics);
            Report(diagnostics, layer.Source);
            if (object != nullptr && object->GetTypeName() == name)
            {
                entries.Add({object, layer.Source.AsView()});
            }
        }
    }
}

/// Visits section *section* (File.Section) as *files* resolved it with *conditions*, resolving it on first use from
/// the layers *layersOf* gives for its file. Called with the owner's lock held.
template <typename TUser, typename TLayersOf>
void VisitSection(ConfigFiles &files, StringView section, const MarlinTextConditions &conditions, TUser &&user,
                  TLayersOf &&layersOf, FunctionRef<void(const MarlinTextObject &object, StringView source)> visitor)
{
    List<ConfigSectionEntry> *entries = files.Sections.Find(section);
    if (entries == nullptr)
    {
        StringView file;
        StringView name;
        (void)SplitSection(section, file, name);
        List<ConfigSectionEntry> resolved;
        CollectSection(layersOf(file), conditions, name, user(), resolved);
        entries = &files.Sections.FindOrAdd(String(section));
        *entries = Move(resolved);
    }
    for (const ConfigSectionEntry &entry : *entries)
    {
        visitor(*entry.Object, entry.Source);
    }
}

bool ApplyLayer(const MarlinTextObject &layer, StringView source, const MarlinTextConditions &conditions,
                const TypeInfo &type, void *object, bool instanceOnly = false)
{
    MarlinTextDiagnostics diagnostics;
    MarlinTextReadOptions options;
    options.Conditions = &conditions;
    options.IgnoreTypeName = true;
    options.StaticFieldsOnly = object == nullptr;
    options.InstanceFieldsOnly = instanceOnly;
    const bool ok = MarlinTextSerializer::ReadObject(layer, type, object, diagnostics, options);
    Report(diagnostics, source);
    return ok;
}

/// The toolkit's table of every platform's layers, most specific first: `Name:Layer;Layer|Name:Layer;...`.
constexpr StringView PlatformLayerTable = MARLIN_PLATFORM_LAYER_TABLE;
} // namespace

struct ConfigView::State
{
    mutable Mutex Lock;
    List<String> Directories;
    List<String> UserDirectories;
    List<String> Overrides;
    /// The process's ConfigDesc::RestrictUser and the sections it lets user layers set; copied so that reading a
    /// view never takes the process's lock.
    bool RestrictUser = false;
    List<String> UserSections;
    /// The target platform's layers, lowest first.
    List<String> Layers;
    List<ConfigVariable> Variables;
    /// Views of Variables for MarlinTextConditions.
    List<MarlinTextConditions::Variable> Conditions;
    mutable ConfigFiles Files;

    /// The layers of *file*, read on first use. Called with Lock held.
    List<ConfigLayer> &LayersOf(StringView file) const
    {
        if (List<ConfigLayer> *layers = Files.Layers.Find(file))
        {
            return *layers;
        }
        List<ConfigLayer> &layers = Files.Layers.FindOrAdd(String(file));
        List<String> paths;
        for (const String &path : SourcePaths(Directories, file, paths))
        {
            AddOwnLayer(layers, path.AsView(), false);
        }
        paths.Clear();
        for (const String &path : SourcePaths(UserDirectories, file, paths))
        {
            AddOwnLayer(layers, path.AsView(), true);
        }
        AddOverrides(layers, Overrides.AsSpan(), file);
        return layers;
    }

    List<String> &SourcePaths(const List<String> &directories, StringView file, List<String> &paths) const
    {
        for (const String &directory : directories)
        {
            paths.Add(Format("{0}/{1}.mtext", directory, file));
            for (const String &layer : Layers)
            {
                StringBuilder<256> folder;
                if (LayerDirectory(directory.AsView(), layer.AsView(), folder))
                {
                    paths.Add(Format("{0}/{1}.mtext", folder.AsView(), file));
                }
            }
        }
        return paths;
    }
};

/// Makes views for Config::GetView, which ConfigView's constructor is private to.
struct ConfigViews
{
    static UniquePtr<ConfigView> Make(UniquePtr<ConfigView::State> state)
    {
        return UniquePtr<ConfigView>(new ConfigView(Move(state)));
    }

    static ConfigView::State &StateOf(ConfigView &view)
    {
        return *view._state;
    }
};

ConfigView::ConfigView(UniquePtr<State> state) : _state(Move(state))
{
}

ConfigView::~ConfigView() = default;

void ConfigView::ForEachSection(StringView section,
                                FunctionRef<void(const MarlinTextObject &object, StringView source)> visitor) const
{
    StringView file;
    StringView name;
    if (!SplitSection(section, file, name))
    {
        MARLIN_LOG(LogConfig, Error, "Config section {0} is not File.Section", section);
        return;
    }
    const ScopeLock lock(_state->Lock);
    const MarlinTextConditions conditions(_state->Conditions.AsSpan());
    VisitSection(
        _state->Files, section, conditions,
        [&] { return UserMaySet(_state->RestrictUser, _state->UserSections, section); },
        [&](StringView of) -> List<ConfigLayer> & { return _state->LayersOf(of); }, visitor);
}

bool ConfigView::Apply(StringView section, const TypeInfo &type, void *object) const
{
    const MarlinTextConditions conditions(_state->Conditions.AsSpan());
    bool ok = true;
    ForEachSection(section, [&](const MarlinTextObject &layer, StringView source) {
        ok &= ApplyLayer(layer, source, conditions, type, object, true);
    });
    return ok;
}

void ConfigView::GetSourcePaths(StringView file, List<String> &paths) const
{
    (void)_state->SourcePaths(_state->Directories, file, paths);
    (void)_state->SourcePaths(_state->UserDirectories, file, paths);
}

namespace
{
/// Applies every config type's static fields, returning how many types that was: the summary line of Initialize.
uint32 ApplyStaticFields()
{
    List<const TypeInfo *> types;
    Reflection::ForEachType([&](const TypeInfo &type) {
        if (type.HasFields() && type.GetMetadata().Has("Config"))
        {
            types.Add(&type);
        }
    });
    uint32 applied = 0;
    for (const TypeInfo *type : types)
    {
        bool hasStatics = false;
        for (const FieldInfo &field : type->GetOwnFields())
        {
            hasStatics = hasStatics || field.IsStatic();
        }
        if (hasStatics)
        {
            (void)Config::Apply(*type, nullptr);
            ++applied;
        }
    }
    return applied;
}
} // namespace

namespace Config
{
void Initialize(const ConfigDesc &desc)
{
    ConfigState &state = ConfigState::Get();
    List<String> userSections = desc.RestrictUser ? FindUserSections() : List<String>();
    {
        const ScopeLock lock(state.Lock);
        state.Desc = desc;
        state.UserSections = Move(userSections);
        state.Files.Clear();
        state.Initialized = true;
        for (auto &entry : state.Views)
        {
            ConfigView::State &view = ConfigViews::StateOf(*entry.Value);
            const ScopeLock viewLock(view.Lock);
            view.Directories = desc.Directories;
            view.UserDirectories = desc.UserDirectories;
            view.Overrides = desc.Overrides;
            view.RestrictUser = desc.RestrictUser;
            view.UserSections = state.UserSections;
            view.Files.Clear();
        }
    }
    const uint32 applied = ApplyStaticFields();
    MARLIN_LOG(LogConfig, Info,
               "Config ready: {0} types applied from {1} directories and {2} user directories; {3} overrides{4}",
               applied, desc.Directories.Count(), desc.UserDirectories.Count(), desc.Overrides.Count(),
               desc.RestrictUser ? StringView("; user settings restricted") : StringView());
}

void Shutdown()
{
    ConfigState &state = ConfigState::Get();
    const ScopeLock lock(state.Lock);
    state.Files.Clear();
    state.Desc = {};
    state.Initialized = false;
    for (auto &entry : state.Views)
    {
        const ScopeLock viewLock(ConfigViews::StateOf(*entry.Value).Lock);
        ConfigViews::StateOf(*entry.Value).Files.Clear();
    }
}

bool IsInitialized()
{
    ConfigState &state = ConfigState::Get();
    const ScopeLock lock(state.Lock);
    return state.Initialized;
}

void Reload()
{
    ConfigState &state = ConfigState::Get();
    {
        const ScopeLock lock(state.Lock);
        state.Files.Clear();
        for (auto &entry : state.Views)
        {
            const ScopeLock viewLock(ConfigViews::StateOf(*entry.Value).Lock);
            ConfigViews::StateOf(*entry.Value).Files.Clear();
        }
    }
    const uint32 applied = ApplyStaticFields();
    MARLIN_LOG(LogConfig, Info, "Config reloaded: {0} types applied", applied);
}

void ForEachSection(StringView section, FunctionRef<void(const MarlinTextObject &object, StringView source)> visitor)
{
    StringView file;
    StringView name;
    if (!SplitSection(section, file, name))
    {
        MARLIN_LOG(LogConfig, Error, "Config section {0} is not File.Section", section);
        return;
    }
    ConfigState &state = ConfigState::Get();
    const ScopeLock lock(state.Lock);
    if (!state.Initialized)
    {
        return;
    }
    const MarlinTextConditions conditions(state.Desc.Variables.AsSpan());
    VisitSection(
        state.Files, section, conditions,
        [&] { return UserMaySet(state.Desc.RestrictUser, state.UserSections, section); },
        [&](StringView of) -> List<ConfigLayer> & { return MountedLayersOf(state.Files, state.Desc, of); }, visitor);
}

void ForEachSectionWithConditions(
    StringView section,
    FunctionRef<void(const MarlinTextObject &object, const MarlinTextConditions &conditions, StringView source)>
        visitor)
{
    StringView file;
    StringView name;
    if (!SplitSection(section, file, name))
    {
        MARLIN_LOG(LogConfig, Error, "Config section {0} is not File.Section", section);
        return;
    }
    ConfigState &state = ConfigState::Get();
    const ScopeLock lock(state.Lock);
    if (!state.Initialized)
    {
        return;
    }
    const MarlinTextConditions conditions(state.Desc.Variables.AsSpan());
    VisitSection(
        state.Files, section, conditions,
        [&] { return UserMaySet(state.Desc.RestrictUser, state.UserSections, section); },
        [&](StringView of) -> List<ConfigLayer> & { return MountedLayersOf(state.Files, state.Desc, of); },
        [&](const MarlinTextObject &object, StringView source) { visitor(object, conditions, source); });
}

bool Apply(StringView section, const TypeInfo &type, void *object)
{
    // The layers apply with the conditions they were picked by, without copying the process's variables.
    bool ok = true;
    ForEachSectionWithConditions(section,
                                 [&](const MarlinTextObject &layer, const MarlinTextConditions &conditions,
                                     StringView source) { ok &= ApplyLayer(layer, source, conditions, type, object); });
    return ok;
}

bool Apply(const TypeInfo &type, void *object)
{
    const Optional<StringView> section = type.GetMetadata().Get("Config");
    if (!section.HasValue())
    {
        MARLIN_LOG(LogConfig, Error, "{0} has no Config section", type.GetName());
        return false;
    }
    return Apply(section.Value(), type, object);
}

void ApplyStatics()
{
    (void)ApplyStaticFields();
}

bool TryGet(StringView key, const TypeInfo &type, void *value)
{
    StringView section;
    StringView name;
    if (!key.SplitLast('.', section, name))
    {
        return false;
    }
    bool found = false;
    ForEachSection(section, [&](const MarlinTextObject &object, StringView source) {
        const MarlinTextField *field = object.Find(name);
        if (field == nullptr || field->Operation != MarlinTextOperation::Assign)
        {
            return;
        }
        MarlinTextDiagnostics diagnostics;
        found = MarlinTextSerializer::ReadValue(field->Value, type, value, diagnostics) || found;
        Report(diagnostics, source);
    });
    return found;
}

bool GetPlatformLayers(StringView platform, List<String> &layers)
{
    for (const StringView entry : PlatformLayerTable.Split('|'))
    {
        StringView name;
        StringView stack;
        if (entry.SplitFirst(':', name, stack) && name == platform)
        {
            for (const StringView layer : stack.Split(';'))
            {
                layers.Add(String(layer));
            }
            return true;
        }
    }
    return false;
}

const ConfigView *GetView(const ConfigTarget &target)
{
    StringBuilder<256> key(target.Platform.AsView());
    for (const ConfigVariable &variable : target.Variables)
    {
        key.Append('|').Append(variable.Name).Append('=').Append(variable.Value);
    }
    ConfigState &state = ConfigState::Get();
    const ScopeLock lock(state.Lock);
    if (!state.Initialized)
    {
        return nullptr;
    }
    if (const UniquePtr<ConfigView> *view = state.Views.Find(key.ToString()))
    {
        return view->Get();
    }
    List<String> layers;
    if (!GetPlatformLayers(target.Platform.AsView(), layers))
    {
        MARLIN_LOG(LogConfig, Error, "No platform {0} is known to this build; its config cannot be read",
                   target.Platform);
        return nullptr;
    }
    auto view = MakeUnique<ConfigView::State>();
    view->Directories = state.Desc.Directories;
    view->UserDirectories = state.Desc.UserDirectories;
    view->Overrides = state.Desc.Overrides;
    view->RestrictUser = state.Desc.RestrictUser;
    view->UserSections = state.UserSections;
    for (usize index = layers.Count(); index > 0; --index)
    {
        view->Layers.Add(Move(layers[index - 1]));
    }
    view->Variables.Add({String("Platform"), target.Platform});
    view->Variables.AddRange(target.Variables.AsSpan());
    for (const ConfigVariable &variable : view->Variables)
    {
        view->Conditions.Add({variable.Name.AsView(), variable.Value.AsView()});
    }
    UniquePtr<ConfigView> &made = state.Views.FindOrAdd(key.ToString());
    made = ConfigViews::Make(Move(view));
    return made.Get();
}
} // namespace Config
} // namespace Marlin
