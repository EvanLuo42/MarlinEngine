#include "World/WorldManagerSubsystem.h"

#include "Asset/Assets.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFile.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Misc/CommandLine.h"
#include "String/Format.h"

namespace Marlin
{
bool WorldManagerSubsystem::LoadStartupLevel = false;
String WorldManagerSubsystem::StartupProfile;

namespace
{
/// Reads the file at physical *path*; false if it cannot be read.
bool ReadText(const String &path, String &text)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    bool ok = PlatformFile::GetSize(file, size);
    if (ok)
    {
        char *data = text.ResizeUninitialized(static_cast<usize>(size));
        ok = PlatformFile::Read(file, data, text.Length(), read) && read == text.Length();
    }
    PlatformFile::Close(file);
    return ok;
}

/// StartupLevel of the Product object in Product.mtext: beside the executable when packaged, else in the project.
Guid ReadProductStartupLevel()
{
    const EngineEnvironment &environment = Engine::GetEnvironment();
    String path;
    if (environment.Form == EngineForm::Packaged)
    {
        path = Format("{0}/Product.mtext", environment.ExecutableDirectory);
    }
    else if (environment.HasProject())
    {
        path = Format("{0}/Config/Product.mtext", environment.ProjectRoot);
    }
    String text;
    if (path.IsEmpty() || !ReadText(path, text))
    {
        return {};
    }
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    if (!document.Parse(text, diagnostics))
    {
        return {};
    }
    for (usize i = 0; i < document.GetObjectCount(); ++i)
    {
        const MarlinTextObject *object = document.GetObject(i, diagnostics);
        const MarlinTextField *field =
            object != nullptr && object->GetTypeName() == "Product" ? object->Find("StartupLevel") : nullptr;
        // An object link, `</Project/Content/Levels/Frontend:Frontend#UUID>` (Documentation/Architecture/02 §4).
        if (field != nullptr && field->Value.Kind == MarlinTextValueKind::ObjectLink && field->Value.LinkGuid.IsValid())
        {
            return field->Value.LinkGuid;
        }
    }
    return {};
}
} // namespace

Guid WorldManagerSubsystem::FindStartupLevel()
{
    Guid level;
    StringView argument;
    if (CommandLine::TryGetValue("Level", argument) && Guid::TryParse(argument, level))
    {
        return level;
    }
    level = ReadProductStartupLevel();
    if (!level.IsValid())
    {
        MARLIN_LOG(LogWorld, Warning, "No startup level: pass -Level=<UUID> or set Product.StartupLevel");
    }
    return level;
}

void WorldManagerSubsystem::Initialize(SubsystemContext &context)
{
    _engineSubsystems = context.Collection;
}

void WorldManagerSubsystem::Start()
{
    // Step 10 of startup (Documentation/Architecture/01 §2): the whole engine is up.
    if (!LoadStartupLevel)
    {
        return;
    }
    const Guid level = FindStartupLevel();
    if (level.IsValid())
    {
        (void)OpenLevel(level, StartupProfile.AsView());
    }
}

void WorldManagerSubsystem::Deinitialize()
{
    _pending.Clear();
    while (!_worlds.IsEmpty())
    {
        _worlds.RemoveAt(_worlds.Count() - 1);
    }
    _profiles.Clear();
}

World &WorldManagerSubsystem::CreateWorld(const WorldProfile &profile, StringView name)
{
    return *_worlds.Add(MakeUnique<World>(profile, name, _engineSubsystems));
}

void WorldManagerSubsystem::DestroyWorld(World &world)
{
    for (usize i = 0; i < _worlds.Count(); ++i)
    {
        if (_worlds[i].Get() == &world)
        {
            _worlds.RemoveAt(i);
            return;
        }
    }
}

const WorldProfile *WorldManagerSubsystem::FindProfile(StringView name)
{
    for (const UniquePtr<WorldProfile> &profile : _profiles)
    {
        if (profile->Name.AsView() == name)
        {
            return profile.Get();
        }
    }
    auto profile = MakeUnique<WorldProfile>();
    if (!WorldProfile::Load(name, *profile))
    {
        return nullptr;
    }
    return _profiles.Add(Move(profile)).Get();
}

bool WorldManagerSubsystem::OpenLevel(const Guid &level, StringView profile)
{
    const WorldProfile *found = FindProfile(profile);
    if (found == nullptr)
    {
        MARLIN_LOG(LogWorld, Error, "Cannot open level {0}: no world profile {1}", level, profile);
        return false;
    }
    MARLIN_LOG(LogWorld, Debug, "Opening level {0} with world profile {1}", level, profile);
    _pending.Add({Assets::Load<WorldPartition>(level, IoPriority::Critical), found});
    return true;
}

void WorldManagerSubsystem::OpenPendingLevels()
{
    for (usize i = 0; i < _pending.Count();)
    {
        PendingLevel &pending = _pending[i];
        const AssetState state = pending.Level.GetState();
        if (state == AssetState::Unloaded || state == AssetState::Loading)
        {
            ++i;
            continue;
        }
        if (!pending.Level.IsLoaded())
        {
            MARLIN_LOG(LogWorld, Error, "Level {0} failed to load", pending.Level.GetGuid());
        }
        else
        {
            World &world = CreateWorld(*pending.Profile, pending.Level->Name.AsView());
            MARLIN_LOG(LogWorld, Info, "Opened world {0} from level {1} (profile {2})", world.GetName(),
                       pending.Level.GetGuid(), pending.Profile->Name.AsView());
            if (auto *partition = world.GetSubsystem<WorldPartitionSubsystem>())
            {
                partition->SetPartition(pending.Level);
            }
        }
        _pending.RemoveAt(i);
    }
}

void WorldManagerSubsystem::BeginFrame(const FrameTime &time)
{
    OpenPendingLevels();
    for (const UniquePtr<World> &world : _worlds)
    {
        world->BeginFrame(time);
    }
}

void WorldManagerSubsystem::FixedUpdate(float64 delta)
{
    for (const UniquePtr<World> &world : _worlds)
    {
        world->FixedUpdate(delta);
    }
}

void WorldManagerSubsystem::Update(const FrameTime &time)
{
    for (const UniquePtr<World> &world : _worlds)
    {
        world->Update(time);
    }
}

void WorldManagerSubsystem::EndFrame(const FrameTime &time)
{
    for (const UniquePtr<World> &world : _worlds)
    {
        world->EndFrame(time);
    }
}
} // namespace Marlin
