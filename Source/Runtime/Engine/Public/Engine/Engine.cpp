#include "Engine/Engine.h"

#include "Concurrency/Atomic.h"
#include "Config/Config.h"
#include "Console/Console.h"
#include "CrashReport/CrashReportClient.h"
#include "Diagnostics/Log.h"
#include "Engine/FramePacer.h"
#include "Engine/StartupProgress.h"
#include "HAL/Platform.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "HAL/PlatformTime.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Memory/MemoryOps.h"
#include "Misc/CommandLine.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/AsyncRead.h"
#include "Vfs/Vfs.h"

namespace Marlin
{
namespace
{
constexpr StringView PlatformName = MARLIN_PLATFORM_WINDOWS ? "Windows"
                                    : MARLIN_PLATFORM_LINUX ? "Linux"
                                    : MARLIN_PLATFORM_MACOS ? "macOS"
                                    : MARLIN_PLATFORM_PS5   ? "PS5"
                                    : MARLIN_PLATFORM_XBOX  ? "Xbox"
                                                            : "Switch2";

constexpr StringView ConfigurationName = MARLIN_BUILD_DEBUG         ? "Debug"
                                         : MARLIN_BUILD_DEVELOPMENT ? "Development"
                                         : MARLIN_BUILD_PROFILE     ? "Profile"
                                                                    : "Shipping";

constexpr StringView FormName(EngineForm form)
{
    return form == EngineForm::Packaged ? "packaged" : form == EngineForm::Installed ? "installed" : "source";
}

/// Mount priorities: platform layers sit above the folder they overlay, the most specific highest.
constexpr int32 LayerPriorityBase = 100;

struct EngineState
{
    EngineEnvironment Environment;
    ProgramFeatures Features = ProgramFeatures::None;
    SubsystemCollection *Subsystems = nullptr;
    alignas(SubsystemCollection) uint8 SubsystemStorage[sizeof(SubsystemCollection)];
    String TempDirectory;
    CompressionCodecSet Codecs;
    List<Engine::MountedPackage> Packages;
    bool Started = false;
    /// When Startup began and how many frames the loop ran, for the shutdown line.
    float64 StartTime = 0;
    uint64 Frames = 0;
    const StartupSplash *Splash = nullptr;
    Atomic<bool> ExitRequested{false};
    Atomic<int32> ExitCode{0};

    /// Built on first use and never destroyed.
    static EngineState &Get()
    {
        alignas(EngineState) static uint8 storage[sizeof(EngineState)];
        static EngineState *state = ConstructAt(reinterpret_cast<EngineState *>(storage));
        return *state;
    }
};

String TrimSeparators(StringView path)
{
    String result(path);
    result.Replace('\\', '/');
    while (result.Length() > 1 && result.EndsWith("/"))
    {
        result.Remove(result.Length() - 1);
    }
    return result;
}

String ParentOf(StringView path, uint32 levels)
{
    StringView current = path;
    for (uint32 i = 0; i < levels; ++i)
    {
        StringView parent;
        StringView name;
        if (!current.SplitLast('/', parent, name))
        {
            return {};
        }
        current = parent;
    }
    return {current};
}

String Join(StringView a, StringView b)
{
    StringBuilder<512> path(a);
    path.Append('/').Append(b);
    return path.ToString();
}

bool IsDirectory(StringView path)
{
    const String terminated(path);
    return PlatformFile::DirectoryExists(terminated.CStr());
}

bool IsFile(StringView path)
{
    const String terminated(path);
    return PlatformFile::FileExists(terminated.CStr());
}

String FolderPath(KnownFolder folder)
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(folder, buffer, sizeof(buffer));
    return length < sizeof(buffer) ? TrimSeparators(StringView(buffer, length)) : String();
}

String FullPath(StringView path)
{
    const String terminated(path);
    char buffer[1024];
    const usize length = PlatformFile::GetFullPath(terminated.CStr(), buffer, sizeof(buffer));
    return length < sizeof(buffer) ? TrimSeparators(StringView(buffer, length)) : TrimSeparators(path);
}

/// Reads object *type* of a MarlinText file on disk, for files read before the VFS is up.
bool ReadObject(StringView path, StringView type, MarlinTextDocument &document, const MarlinTextObject *&object)
{
    const String terminated(path);
    const FileHandle file = PlatformFile::Open(terminated.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    String text;
    if (PlatformFile::GetSize(file, size))
    {
        char *data = text.ResizeUninitialized(static_cast<usize>(size));
        if (!PlatformFile::Read(file, data, text.Length(), read))
        {
            text.Clear();
        }
    }
    PlatformFile::Close(file);
    MarlinTextDiagnostics diagnostics;
    if (!document.Parse(text, diagnostics))
    {
        MARLIN_LOG(LogCore, Error, "{0}", diagnostics.ToString(path));
        return false;
    }
    for (usize i = 0; i < document.GetObjectCount(); ++i)
    {
        object = document.GetObject(i, diagnostics);
        if (object != nullptr && object->GetTypeName() == type)
        {
            return true;
        }
    }
    return false;
}

String StringField(const MarlinTextObject &object, StringView key)
{
    const MarlinTextField *field = object.Find(key);
    return field != nullptr && field->Value.Kind == MarlinTextValueKind::String ? String(field->Value.Text) : String();
}

void ReadProduct(StringView path, ProductIdentity &product)
{
    MarlinTextDocument document;
    const MarlinTextObject *object = nullptr;
    if (!ReadObject(path, "Product", document, object))
    {
        return;
    }
    product.Company = StringField(*object, "Company");
    product.ProductName = StringField(*object, "ProductName");
    product.DisplayName = StringField(*object, "DisplayName");
    product.Version = StringField(*object, "Version");
}

void DetectEnvironment(EngineState &state, StringView targetName)
{
    EngineEnvironment &environment = state.Environment;
    environment = {};
    environment.TargetName = targetName;
    environment.Platform = PlatformName;
    environment.Configuration = ConfigurationName;
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    const String executable = TrimSeparators(StringView(buffer, length < sizeof(buffer) ? length : 0));
    environment.ExecutableDirectory = ParentOf(executable, 1);
    environment.EngineVersion = MARLIN_STRINGIFY(MARLIN_VERSION_MAJOR) "." MARLIN_STRINGIFY(MARLIN_VERSION_MINOR);

    const String product = Join(environment.ExecutableDirectory, "Product.mtext");
    if (IsFile(product))
    {
        environment.Form = EngineForm::Packaged;
        ReadProduct(product, environment.Product);
        environment.EngineId = environment.Product.ProductName;
        return;
    }
    environment.EngineRoot = ParentOf(environment.ExecutableDirectory, 4);
    if (environment.EngineRoot.IsEmpty() || !IsDirectory(Join(environment.EngineRoot, "Binaries")))
    {
        MARLIN_LOG(LogCore, Warning, "{0} is not in Binaries/<Platform>/<Arch>/<Config>; the engine root is unknown",
                   executable);
        environment.EngineRoot.Clear();
        return;
    }
    const bool installed = IsFile(Join(environment.EngineRoot, "Build/InstalledBuild"));
    environment.Form = installed ? EngineForm::Installed : EngineForm::Source;
    environment.EngineId =
        installed ? environment.EngineVersion : Engine::ComputeSourceEngineId(environment.EngineRoot);
}

/// Mounts *root* at *point* with the platform layers of `<root>/Platforms` above it.
void MountLayered(StringView point, StringView root, bool writable)
{
    Vfs::Mount(point, MakeUnique<DirectoryMount>(root, writable), 0);
    const StringView layers = MARLIN_PLATFORM_LAYERS;
    int32 priority = LayerPriorityBase + 64;
    uint32 layerCount = 0;
    for (const StringView layer : layers.Split(';'))
    {
        StringBuilder<512> directory(root);
        directory.Append("/Platforms/").Append(layer);
        if (IsDirectory(directory.AsView()))
        {
            Vfs::Mount(point, MakeUnique<DirectoryMount>(directory.AsView(), writable), priority);
            ++layerCount;
        }
        --priority;
    }
    MARLIN_LOG(LogCore, Debug, "Mounted {0} at {1} ({2} platform layers{3})", point, root, layerCount,
               writable ? "" : ", read-only");
}

void MountDirectory(StringView point, StringView path)
{
    const String terminated(path);
    if (!PlatformFile::CreateDirectory(terminated.CStr()))
    {
        MARLIN_LOG(LogCore, Warning, "Cannot create {0}; the {1} mount may not work", path, point);
    }
    Vfs::Mount(point, MakeUnique<DirectoryMount>(path, true), 0);
    MARLIN_LOG(LogCore, Debug, "Mounted {0} at {1}", point, path);
}

void MountEngine(EngineState &state)
{
    const EngineEnvironment &environment = state.Environment;
    char process[16];
    const StringView processId(process, FormatUnsigned(PlatformProcess::CurrentId(), process));
    state.TempDirectory = Join(Join(FolderPath(KnownFolder::Temp), "Marlin"), processId);
    MountDirectory("/Temp", state.TempDirectory);
    if (environment.Form == EngineForm::Packaged)
    {
        return;
    }
    if (environment.EngineRoot.IsEmpty())
    {
        return;
    }
    const bool source = environment.Form == EngineForm::Source;
    MountLayered("/Engine", environment.EngineRoot, source);
    MountDirectory("/Cache", Join(FolderPath(KnownFolder::LocalAppData), "Marlin/Cache"));
    MountDirectory("/User/Global",
                   source ? Join(environment.EngineRoot, "Saved/User")
                          : Join(FolderPath(KnownFolder::LocalAppData), Join("Marlin", environment.EngineId)));
}

/// Where a package's chunk ranks: patches above everything they patch, numbered patches in order.
int32 ChunkPriority(StringView chunk)
{
    if (chunk == "Base")
    {
        return 0;
    }
    uint32 number = 0;
    if (chunk.StartsWith("Patch_") && TryParse(chunk.RemoveStart(6), number))
    {
        return 1000 + static_cast<int32>(number < 1000 ? number : 999);
    }
    return 100;
}

/// Mounts every package in Paks/ beside the executable (Documentation/Architecture/02 §5): each mount point
/// and platform layer of a package becomes a PakMount. Layers rank above packages, so a patch to a root file does not
/// shadow a platform layer's file, and Config reads each layer of each package in order. Encrypted packages get their
/// keys from the linked PakKeyProviders.
void MountPackages(EngineState &state)
{
    const EngineEnvironment &environment = state.Environment;
    const String folder = Join(environment.ExecutableDirectory, "Paks");
    DirectoryIterator iterator;
    DirectoryEntry entry;
    List<String> paths;
    if (iterator.Open(folder.CStr()))
    {
        while (iterator.Next(entry))
        {
            if (!entry.Info.IsDirectory && StringView(entry.Name).EndsWith(".mpak"))
            {
                paths.Add(Join(folder, entry.Name));
            }
        }
    }
    if (paths.IsEmpty())
    {
        MARLIN_LOG(LogCore, Error, "{0} holds no packages", folder);
        return;
    }
    const StringView layers = MARLIN_PLATFORM_LAYERS;
    List<StringView> layerOrder;
    for (const StringView layer : layers.Split(';'))
    {
        layerOrder.Add(layer);
    }
    PakCrypto crypto;
    crypto.Build({});
    for (const String &path : paths)
    {
        const SharedPtr<PakFile> pak = PakFile::Open(path.AsView(), state.Codecs, &crypto);
        if (!pak.IsValid())
        {
            MARLIN_LOG(LogCore, Error, "{0} is not a package this build can read, or its signature does not hold",
                       path);
            continue;
        }
        // A product that links a PakVerifyKeyProvider ships only signed packages: Shipping mounts nothing else, so a
        // package dropped beside the game cannot replace its content, config or scripts (06 §5.3). Development and
        // Profile builds still mount it, for testing side-loaded packages.
        if (crypto.RequiresSignatures() && !pak->IsSigned())
        {
            if (MARLIN_BUILD_SHIPPING)
            {
                MARLIN_LOG(LogCore, Error, "{0} is not signed with a key this game knows; it is not mounted", path);
                continue;
            }
            MARLIN_LOG(LogCore, Warning, "{0} is not signed with a key this game knows; Shipping would not mount it",
                       path);
        }
        const int32 priority = ChunkPriority(pak->GetChunk());
        List<String> mounted;
        for (const PakFileEntry &file : pak->GetFiles())
        {
            const StringView point = pak->GetString(file.MountPoint);
            const StringView layer = pak->GetString(file.Layer);
            const String key = Format("{0}|{1}", point, layer);
            if (mounted.Contains(key))
            {
                continue;
            }
            mounted.Add(key);
            int32 rank = 0;
            for (usize i = 0; i < layerOrder.Count(); ++i)
            {
                if (layerOrder[i] == layer)
                {
                    rank = static_cast<int32>(layerOrder.Count() - i);
                }
            }
            Vfs::Mount(point, MakeUnique<PakMount>(pak, point, layer), rank * 4096 + priority);
        }
        MARLIN_LOG(LogCore, Info, "Mounted {0} ({1})", path, pak->GetChunk());
        state.Packages.Add({pak, priority});
    }
    state.Packages.Sort(
        [](const Engine::MountedPackage &a, const Engine::MountedPackage &b) { return a.Priority > b.Priority; });
}

bool ResolveProject(EngineState &state, bool required)
{
    EngineEnvironment &environment = state.Environment;
    StringView argument;
    if (environment.Form == EngineForm::Packaged)
    {
        const ProductIdentity &product = environment.Product;
        const String folder = Join(product.Company, product.ProductName);
        MountDirectory("/User", Join(FolderPath(KnownFolder::LocalAppData), folder));
        MountDirectory("/User/SaveGames", Join(FolderPath(KnownFolder::SavedGames), folder));
        return true;
    }
    if (!CommandLine::TryGetValue("Project", argument))
    {
        if (required)
        {
            MARLIN_LOG(LogCore, Error, "{0} needs -Project=<path to .mproject>", environment.TargetName);
            return false;
        }
        MountDirectory("/User", environment.EngineRoot.IsEmpty()
                                    ? Join(FolderPath(KnownFolder::LocalAppData), "Marlin/User")
                                    : Join(environment.EngineRoot, "Saved/User"));
        return true;
    }
    environment.ProjectFile = FullPath(argument);
    MarlinTextDocument document;
    const MarlinTextObject *project = nullptr;
    if (!ReadObject(environment.ProjectFile, "Project", document, project))
    {
        MARLIN_LOG(LogCore, Error, "{0} is not a project file", environment.ProjectFile);
        return false;
    }
    environment.ProjectRoot = ParentOf(environment.ProjectFile, 1);
    StringView stem;
    StringView extension;
    StringView folder;
    StringView file;
    environment.ProjectFile.AsView().SplitLast('/', folder, file);
    environment.ProjectName = file.SplitLast('.', stem, extension) ? String(stem) : String(file);
    const String binding = StringField(*project, "Engine");
    if (!binding.IsEmpty() && binding != environment.EngineId && binding != environment.EngineVersion)
    {
        MARLIN_LOG(LogCore, Warning, "{0} is bound to engine {1}, but this is {2}", environment.ProjectName, binding,
                   environment.EngineId);
    }
    MountLayered("/Project", environment.ProjectRoot, true);
    StringView user;
    MountDirectory("/User", CommandLine::TryGetValue("UserDir", user) ? FullPath(user)
                                                                      : Join(environment.ProjectRoot, "Saved/User"));
    ReadProduct(Join(environment.ProjectRoot, "Config/Product.mtext"), environment.Product);
    return true;
}

/// -ExecCommands="a;b" runs console commands after the first frame, for automation.
void ExecuteStartupCommands()
{
    StringView commands;
    // Automation only: a shipped game runs no commands from its command line.
    if (MARLIN_BUILD_SHIPPING || !CommandLine::TryGetValue("ExecCommands", commands))
    {
        return;
    }
    for (const StringView command : commands.Split(';'))
    {
        StringBuilder<256> output;
        const bool succeeded = Console::Execute(command.Trim(), output);
        MARLIN_LOG(LogCore, Info, "{0}{1}{2}", command.Trim(), succeeded ? StringView(": ") : StringView(" failed: "),
                   output.AsView());
    }
}

/// Catches crashes from the first line of Startup, before the engine knows where reports go.
void InstallCrashHandler()
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(KnownFolder::LocalAppData, buffer, sizeof(buffer));
    if (length != 0 && length + sizeof("/Marlin/Crashes") <= sizeof(buffer))
    {
        MemCopy(buffer + length, "/Marlin/Crashes", sizeof("/Marlin/Crashes"));
        CrashReport::Install(StringView(buffer, length + sizeof("/Marlin/Crashes") - 1));
    }
}

void StartCrashReport(const EngineState &state, bool launchReporter)
{
    const EngineEnvironment &environment = state.Environment;
    const String directory = environment.Form == EngineForm::Source && !environment.EngineRoot.IsEmpty()
                                 ? Join(environment.EngineRoot, "Saved/Crashes")
                                 : Join(FolderPath(KnownFolder::LocalAppData), "Marlin/Crashes");
    CrashReportStartDesc desc;
    desc.TargetName = environment.TargetName;
    desc.EngineId = environment.EngineId;
    desc.EngineVersion = environment.EngineVersion;
    desc.Platform = environment.Platform;
    desc.Configuration = environment.Configuration;
    desc.ExecutableDirectory = environment.ExecutableDirectory;
    desc.ReportDirectory = directory;
    desc.LaunchReporter = launchReporter;
    CrashReport::Start(desc);
}

/// Once the project is known: its name and product, and reports in /User/Crashes.
void UpdateCrashReport(const EngineState &state)
{
    const EngineEnvironment &environment = state.Environment;
    CrashReport::SetProduct(environment.ProjectName, environment.Product.ProductName, environment.Product.Version);
    if (!Vfs::IsMounted("/User"))
    {
        return;
    }
    (void)Vfs::CreateDirectory("/User/Crashes");
    StringBuilder<512> directory;
    if (Vfs::GetPhysicalPath("/User/Crashes", directory))
    {
        CrashReport::SetReportDirectory(directory.AsView());
    }
}

/// Hands the Engine.CrashReport settings to the reporter once config is loaded.
void ShareConfig()
{
    // RECOVERY-DECISION D11: Engine.CrashReport defaults chosen; Enabled = false stops reporting.
    if (!Config::GetOr<bool>("Engine.CrashReport.Enabled", true))
    {
        CrashReport::Stop();
        return;
    }
    String host;
    (void)Config::TryGet("Engine.CrashReport.Server.Host", host);
    const auto port = Config::GetOr<uint16>("Engine.CrashReport.Server.Port", uint16{7492});
    // Players are asked before a report leaves their machine unless the product turns it off; developers are not.
    const bool askUser =
        Config::GetOr<bool>("Engine.CrashReport.AskUser", EngineState::Get().Environment.Form == EngineForm::Packaged);
    const auto hangSeconds = Config::GetOr<uint32>("Engine.CrashReport.HangSeconds", 0);
    const bool scrub = Config::GetOr<bool>("Engine.CrashReport.Privacy.Scrub", true);
    String kept;
    (void)Config::TryGet("Engine.CrashReport.Privacy.KeptArguments", kept);
    const auto maxReports = Config::GetOr<uint32>("Engine.CrashReport.MaxLocalReports", 20);
    const auto days = Config::GetOr<uint32>("Engine.CrashReport.RetentionDays", 30);
    CrashReport::SetUpload(host, port, askUser);
    CrashReport::SetHangSeconds(hangSeconds);
    CrashReport::SetPrivacy(scrub, kept);
    CrashReport::SetRetention(maxReports, days);

    // RECOVERY-DECISION D12: CrashConfig.mtext, attached to every report, holds the effective crash settings and
    // the config layers that set any of them.
    StringBuilder<2048> text;
    FormatTo(text, "CrashConfig\n{{\n    ServerHost = \"{0}\"\n    ServerPort = {1}\n    AskUser = {2}\n", host, port,
             askUser);
    FormatTo(text, "    HangSeconds = {0}\n    Scrub = {1}\n    KeptArguments = \"{2}\"\n", hangSeconds, scrub, kept);
    FormatTo(text, "    MaxLocalReports = {0}\n    RetentionDays = {1}\n    Layers = [", maxReports, days);
    bool first = true;
    Config::ForEachSection("Engine.CrashReport", [&](const MarlinTextObject & /*object*/, StringView source) {
        FormatTo(text, "{0}\"{1}\"", first ? StringView() : StringView(", "), source);
        first = false;
    });
    text.Append("]\n}\n");
    StringBuilder<1024> physical;
    if (Vfs::WriteAll("/Temp/CrashConfig.mtext", text.AsView()) &&
        Vfs::GetPhysicalPath("/Temp/CrashConfig.mtext", physical))
    {
        (void)CrashReport::AddAttachment(physical.AsView());
    }
}

void LoadConfig(const EngineState &state)
{
    ConfigDesc desc;
    desc.Directories.Add(String("/Engine/Config"));
    if (state.Environment.HasProject())
    {
        desc.Directories.Add(String("/Project/Config"));
    }
    desc.UserDirectories.Add(String("/User/Config"));
    // A shipped game takes from players only the settings its types mark UserConfigurable, and no -Config.
    desc.RestrictUser = MARLIN_BUILD_SHIPPING;
    if (!MARLIN_BUILD_SHIPPING)
    {
        CommandLine::ForEachValue("Config", [&](StringView value) { desc.Overrides.Add(String(value)); });
    }
    desc.Variables.Add({"Platform", PlatformName});
    desc.Variables.Add({"Configuration", ConfigurationName});
    desc.Variables.Add({"Target", state.Environment.TargetName.AsView()});
    Config::Initialize(desc);
    ShareConfig();
}
} // namespace

namespace Engine
{
const EngineEnvironment &GetEnvironment()
{
    return EngineState::Get().Environment;
}

SubsystemCollection *GetSubsystems()
{
    return EngineState::Get().Subsystems;
}

Span<const MountedPackage> GetPackages()
{
    return EngineState::Get().Packages.AsSpan();
}

const CompressionCodecSet &GetCompressionCodecs()
{
    return EngineState::Get().Codecs;
}

String ComputeSourceEngineId(StringView root)
{
    const String path = TrimSeparators(root);
    uint32 hash = 0x811C9DC5u;
    for (const char c : path)
    {
        const char folded = MARLIN_PLATFORM_WINDOWS ? Ascii::ToLower(c) : c;
        hash = (hash ^ static_cast<uint8>(folded)) * 0x01000193u;
    }
    char digits[16];
    const usize count = FormatUnsigned(hash, digits, 16);
    StringBuilder<32> id("Source-");
    id.Append(8 - count, '0').Append(StringView(digits, count));
    return id.ToString();
}

bool Startup(const LaunchDesc &desc, Span<const ModuleTableEntry> modules)
{
    const float64 startupStart = PlatformTime::Seconds();
    InstallCrashHandler();
    EngineState &state = EngineState::Get();
    MARLIN_ASSERT(!state.Started);
    state.Started = true;
    state.StartTime = startupStart;
    state.Frames = 0;
    state.Features = desc.Features;
    state.ExitRequested.Store(false);
    state.ExitCode.Store(0);
    Platform::Initialize();
    DetectEnvironment(state, desc.TargetName);
    const EngineEnvironment &environment = state.Environment;
    MARLIN_LOG(LogCore, Info, "{0} {1} starting: {2} {3}", desc.TargetName, environment.EngineVersion,
               environment.Platform, environment.Configuration);
    MARLIN_LOG(LogCore, Info, "Engine {0} ({1}); executable {2}", environment.EngineId, FormName(environment.Form),
               environment.ExecutableDirectory);
    if (HasAnyFlags(desc.Features, ProgramFeatures::Vfs))
    {
        MountEngine(state);
    }
    StartCrashReport(state, HasAnyFlags(desc.Features, ProgramFeatures::CrashReporter));
    state.Splash = desc.Splash;
    if (state.Splash != nullptr && state.Splash->Show != nullptr)
    {
        state.Splash->Show();
    }
    Reflection::RegisterModules(modules);
    state.Codecs.Build({});
    if (HasAnyFlags(desc.Features, ProgramFeatures::Vfs) && environment.Form == EngineForm::Packaged)
    {
        MountPackages(state);
    }
    if (HasAnyFlags(desc.Features, ProgramFeatures::Project) && !ResolveProject(state, desc.RequiresProject))
    {
        MARLIN_LOG(LogCore, Error, "{0} did not start: the project is missing or invalid", desc.TargetName);
        return false;
    }
    UpdateCrashReport(state);
    if (environment.HasProject())
    {
        MARLIN_LOG(LogCore, Info, "Project {0}: {1}", environment.ProjectName, environment.ProjectFile);
    }
    else if (!HasAnyFlags(desc.Features, ProgramFeatures::Project))
    {
        MARLIN_LOG(LogCore, Debug, "No project: {0} runs without content", desc.TargetName);
    }
    if (HasAnyFlags(desc.Features, ProgramFeatures::Config))
    {
        LoadConfig(state);
    }
    if (HasAnyFlags(desc.Features, ProgramFeatures::Subsystems))
    {
        const float64 subsystemsStart = PlatformTime::Seconds();
        state.Subsystems =
            ConstructAt(reinterpret_cast<SubsystemCollection *>(state.SubsystemStorage), TypeOf<EngineSubsystem>());
        state.Subsystems->Initialize();
        uint32 started = 0;
        for (Subsystem *subsystem : state.Subsystems->GetAll())
        {
            const TypeInfo &type = subsystem->GetType();
            StringBuilder<128> status("Starting ");
            status.Append(type.GetName());
            StartupProgress::SetStatus(status.AsView());
            const float64 start = PlatformTime::Seconds();
            static_cast<EngineSubsystem *>(subsystem)->Start();
            const float64 elapsed = (PlatformTime::Seconds() - start) * 1000.0;
            // Slow starts are startup latency a developer looks for; the rest is detail.
            if (elapsed >= 100.0)
            {
                MARLIN_LOG(LogCore, Info, "Started {0} in {1:F0} ms", type.GetName(), elapsed);
            }
            else
            {
                MARLIN_LOG(LogCore, Debug, "Started {0} in {1:F1} ms", type.GetName(), elapsed);
            }
            ++started;
        }
        MARLIN_LOG(LogCore, Info, "Started {0} subsystems in {1:F0} ms", started,
                   (PlatformTime::Seconds() - subsystemsStart) * 1000.0);
    }
    MARLIN_LOG(LogCore, Info, "{0} started in {1:F0} ms: engine {2}, root {3}, project {4}", desc.TargetName,
               (PlatformTime::Seconds() - startupStart) * 1000.0, environment.EngineId,
               environment.EngineRoot.IsEmpty() ? StringView("<packaged>") : environment.EngineRoot.AsView(),
               environment.HasProject() ? environment.ProjectName.AsView() : StringView("none"));
    return true;
}

void Shutdown()
{
    EngineState &state = EngineState::Get();
    if (!state.Started)
    {
        return;
    }
    const float64 shutdownStart = PlatformTime::Seconds();
    MARLIN_LOG(LogCore, Info, "{0} shutting down after {1} frames and {2:F1} s", state.Environment.TargetName,
               state.Frames, shutdownStart - state.StartTime);
    if (state.Splash != nullptr && state.Splash->Hide != nullptr)
    {
        state.Splash->Hide();
    }
    if (state.Subsystems != nullptr)
    {
        const Span<Subsystem *const> subsystems = state.Subsystems->GetAll();
        for (usize i = subsystems.Count(); i-- > 0;)
        {
            static_cast<EngineSubsystem *>(subsystems[i])->Stop();
        }
        DestroyAt(state.Subsystems);
        state.Subsystems = nullptr;
    }
    if (Config::IsInitialized())
    {
        Config::Shutdown();
    }
    Vfs::ShutdownAsync();
    state.Packages.Clear();
    Vfs::UnmountAll();
    if (!state.TempDirectory.IsEmpty())
    {
        (void)PlatformFile::DeleteDirectory(state.TempDirectory.CStr(), true);
        state.TempDirectory.Clear();
    }
    // Goes to the sinks still registered; the log file's sink may already be gone with its subsystem.
    MARLIN_LOG(LogCore, Debug, "Shutdown took {0:F0} ms", (PlatformTime::Seconds() - shutdownStart) * 1000.0);
    Log::Flush();
    CrashReport::Stop();
    Platform::Shutdown();
    state.Started = false;
}

int32 RunLoop()
{
    EngineState &state = EngineState::Get();
    const auto fixedRate = Config::GetOr<float64>("Engine.Loop.FixedRate", 60.0);
    const auto maxFrameRate = Config::GetOr<float64>("Engine.Loop.MaxFrameRate", 0.0);
    const auto maxDelta = Config::GetOr<float64>("Engine.Loop.MaxDelta", 0.25);
    const float64 fixedDelta = 1.0 / (fixedRate > 0 ? fixedRate : 60.0);
    const Span<Subsystem *const> subsystems =
        state.Subsystems != nullptr ? state.Subsystems->GetAll() : Span<Subsystem *const>();

    // -ExitAfterFrames=N ends the loop by itself, for smoke tests and automation.
    uint64 frameLimit = 0;
    (void)CommandLine::TryGetValue("ExitAfterFrames", frameLimit);

    FrameTime time;
    float64 accumulator = 0;
    const float64 start = PlatformTime::Seconds();
    float64 previous = start;
    FramePacer pacer;
    pacer.SetInterval(maxFrameRate > 0 ? 1.0 / maxFrameRate : 0.0);
    pacer.Reset(start);
    while (!state.ExitRequested.Load(MemoryOrder::Acquire))
    {
        const float64 now = PlatformTime::Seconds();
        time.Delta = now - previous < maxDelta ? now - previous : maxDelta;
        time.Elapsed = now - start;
        previous = now;
        Log::SetFrame(time.Frame);
        CrashReport::Heartbeat(time.Frame);
        for (Subsystem *subsystem : subsystems)
        {
            static_cast<EngineSubsystem *>(subsystem)->BeginFrame(time);
        }
        accumulator += time.Delta;
        while (accumulator >= fixedDelta)
        {
            for (Subsystem *subsystem : subsystems)
            {
                static_cast<EngineSubsystem *>(subsystem)->FixedUpdate(fixedDelta);
            }
            accumulator -= fixedDelta;
        }
        for (Subsystem *subsystem : subsystems)
        {
            static_cast<EngineSubsystem *>(subsystem)->Update(time);
        }
        for (Subsystem *subsystem : subsystems)
        {
            static_cast<EngineSubsystem *>(subsystem)->EndFrame(time);
        }
        if (time.Frame == 0)
        {
            ExecuteStartupCommands();
        }
        ++time.Frame;
        state.Frames = time.Frame;
        if (frameLimit != 0 && time.Frame >= frameLimit)
        {
            RequestExit(0);
        }
        pacer.Wait();
    }
    const float64 duration = PlatformTime::Seconds() - start;
    const float64 framesPerSecond = duration > 0.0 ? static_cast<float64>(time.Frame) / duration : 0.0;
    MARLIN_LOG(LogCore, Info, "Main loop ended after {0} frames in {1:F1} s ({2:F1} FPS average)", time.Frame, duration,
               framesPerSecond);
    return state.ExitCode.Load();
}

void RequestExit(int32 exitCode)
{
    EngineState &state = EngineState::Get();
    if (!state.ExitRequested.Load(MemoryOrder::Acquire))
    {
        MARLIN_LOG(LogCore, Debug, "Exit requested with code {0} at frame {1}", exitCode, state.Frames);
    }
    state.ExitCode.Store(exitCode);
    state.ExitRequested.Store(true, MemoryOrder::Release);
}

bool IsExitRequested()
{
    return EngineState::Get().ExitRequested.Load(MemoryOrder::Acquire);
}
} // namespace Engine
} // namespace Marlin
