#pragma once

#include "Compression/CompressionCodec.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Engine/Subsystem.h"
#include "EnumFlags.h"
#include "Memory/SharedPtr.h"
#include "Reflection/Reflection.h"
#include "String/String.h"
#include "String/StringView.h"
#include "Vfs/PakFile.h"

namespace Marlin
{
/// How the engine is installed, which decides where everything is (Documentation/Architecture/02 §1).
enum class EngineForm : uint8
{
    /// Built from source: no marker, the default.
    Source,
    /// A read-only engine install: `Build/InstalledBuild` exists, written by `marlin package-engine`.
    Installed,
    /// A shipped product: Product.mtext sits beside the executable.
    Packaged,
};

/// From Product.mtext: the project's Config/Product.mtext in development, the packaged copy beside the executable.
struct ProductIdentity
{
    String Company;
    /// File-system safe; names the executable and the user data folder.
    String ProductName;
    String DisplayName;
    String Version;
};

struct EngineEnvironment
{
    EngineForm Form = EngineForm::Source;
    /// Such as Game, Editor or MarlinTests.
    String TargetName;
    String ExecutableDirectory;
    /// Four levels above the executable, Binaries/<Platform>/<Arch>/<Config>/; empty when packaged.
    String EngineRoot;
    /// `Source-<path hash>` for a source engine, the version for an install; what projects bind to.
    String EngineId;
    String EngineVersion;
    /// Such as Windows and Development: the values of the Platform and Configuration config conditions.
    StringView Platform;
    StringView Configuration;
    /// The .mproject, its folder and its name; empty without a project.
    String ProjectFile;
    String ProjectRoot;
    String ProjectName;
    ProductIdentity Product;

    [[nodiscard]] bool HasProject() const
    {
        return !ProjectRoot.IsEmpty();
    }
};

/// What a program starts beyond Core and reflection (Documentation/Architecture/01 §2).
enum class ProgramFeatures : uint8
{
    None = 0,
    /// Mounts /Engine, /User/Global, /Cache and /Temp.
    Vfs = 1 << 0,
    /// Resolves -Project= and mounts /Project and /User.
    Project = 1 << 1,
    /// Loads the config layers and applies them to config types.
    Config = 1 << 2,
    /// Creates the engine subsystems.
    Subsystems = 1 << 3,
    /// Starts a MarlinCrashReporter for the process; without it a crash is dumped from inside the process.
    CrashReporter = 1 << 4,
    All = Vfs | Project | Config | Subsystems | CrashReporter, // NOLINT(bugprone-signed-bitwise)
};
MARLIN_ENUM_FLAGS(ProgramFeatures)

/// A splash screen a target shows while it starts (Documentation/Architecture/09 §4.6). Its module links into the
/// executable, which hands it to the engine through LaunchDesc.
struct StartupSplash
{
    /// Called once the crash handler is in place (step 4); may decline, such as with -Headless.
    void (*Show)() = nullptr;
    /// Called by Shutdown in case startup failed with the splash still up; does nothing once it closed.
    void (*Hide)() = nullptr;
};

struct LaunchDesc
{
    StringView TargetName;
    ProgramFeatures Features = ProgramFeatures::All;
    /// Fails when no -Project= is given; otherwise the engine runs without /Project.
    bool RequiresProject = false;
    /// Null for none.
    const StartupSplash *Splash = nullptr;
};

namespace Engine
{
[[nodiscard]] ENGINE_API const EngineEnvironment &GetEnvironment();

/// A package the packaged engine mounted from Paks/: its asset catalog serves the asset table at Priority.
struct MountedPackage
{
    SharedPtr<const PakFile> Pak;
    int32 Priority = 0;
};

/// The packages mounted at startup, highest priority first; empty unless the engine runs packaged.
[[nodiscard]] ENGINE_API Span<const MountedPackage> GetPackages();
/// The CompressionCodec implementations linked into the process, built at startup.
[[nodiscard]] ENGINE_API const CompressionCodecSet &GetCompressionCodecs();

/// The engine-scope subsystems, or null before they start.
[[nodiscard]] ENGINE_API SubsystemCollection *GetSubsystems();

template <typename T> [[nodiscard]] T *GetSubsystem()
{
    const SubsystemCollection *subsystems = GetSubsystems();
    return subsystems != nullptr ? subsystems->Get<T>() : nullptr;
}

/// Runs frames until something requests exit; returns the exit code. Each frame calls the engine subsystems'
/// BeginFrame, FixedUpdate at the fixed rate, Update and EndFrame (Documentation/Architecture/01 §4).
ENGINE_API int32 RunLoop();
ENGINE_API void RequestExit(int32 exitCode = 0);
[[nodiscard]] ENGINE_API bool IsExitRequested();

/// `Source-` and the FNV-1a hash of *root*, '/'-separated, without a trailing '/' and lowercase on Windows; the
/// toolkit registers the same Id.
[[nodiscard]] ENGINE_API String ComputeSourceEngineId(StringView root);

/// Steps 1 to 9 of the startup; *modules* is the executable's module table.
ENGINE_API bool Startup(const LaunchDesc &desc, Span<const ModuleTableEntry> modules);
/// Everything Startup did, in reverse.
ENGINE_API void Shutdown();
} // namespace Engine

/// Starts the engine, runs the main loop and shuts down; the launch module's main returns what this does.
inline int32 LaunchMain(const LaunchDesc &desc);

/// For programs, which start only what they ask for.
namespace ProgramHost
{
inline bool Initialize(StringView name, ProgramFeatures features = ProgramFeatures::None);

inline void Shutdown()
{
    Engine::Shutdown();
}
} // namespace ProgramHost
} // namespace Marlin

// The module table is defined in the executable, so only code compiled into it may name it.
#include "Reflection/ModuleTable.h"

namespace Marlin
{
inline int32 LaunchMain(const LaunchDesc &desc)
{
    if (!Engine::Startup(desc, GetModuleTable()))
    {
        Engine::Shutdown();
        return 1;
    }
    const int32 exitCode = Engine::RunLoop();
    Engine::Shutdown();
    return exitCode;
}

inline bool ProgramHost::Initialize(StringView name, ProgramFeatures features)
{
    LaunchDesc desc;
    desc.TargetName = name;
    desc.Features = features;
    return Engine::Startup(desc, GetModuleTable());
}
} // namespace Marlin
