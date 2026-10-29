#include "AssetPipeline/AssetIndex.h"
#include "AssetPipeline/AssetScanner.h"
#include "Concurrency/Jobs.h"
#include "Config/Config.h"
#include "Engine/Engine.h"
#include "HAL/PlatformFile.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Memory/MemoryOps.h"
#include "Misc/CommandLine.h"
#include "Packaging/Packager.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"
#include "Vfs/PakCipher.h"
#include "Vfs/Vfs.h"

namespace
{
using namespace Marlin;

String ParentOf(StringView path)
{
    StringView parent;
    StringView name;
    return path.SplitLast('/', parent, name) ? String(parent) : String();
}

bool FileExists(const String &path)
{
    return PlatformFile::FileExists(path.CStr());
}

/// Copies *from* over *to*, unless *to* already has its size and modification time (Copy keeps the time).
bool CopyFile(const String &from, const String &to)
{
    FileInfo source;
    FileInfo target;
    if (PlatformFile::GetInfo(from.CStr(), source) && PlatformFile::GetInfo(to.CStr(), target) &&
        source.Size == target.Size && source.ModifiedTime == target.ModifiedTime)
    {
        return true;
    }
    return PlatformFile::Copy(from.CStr(), to.CStr(), true);
}

/// Reads the native file *native* as text.
bool ReadText(StringView native, String &text)
{
    const String path(native);
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Read, FileMode::OpenExisting);
    if (!file.IsValid())
    {
        return false;
    }
    uint64 size = 0;
    usize read = 0;
    List<uint8> bytes;
    const bool ok = PlatformFile::GetSize(file, size) && (bytes.ResizeUninitialized(static_cast<usize>(size)), true) &&
                    PlatformFile::Read(file, bytes.Data(), bytes.Count(), read) && read == bytes.Count();
    PlatformFile::Close(file);
    text = StringView(reinterpret_cast<const char *>(bytes.Data()), bytes.Count());
    return ok;
}

/// Reads the package key from the native file *native*: its bytes as hex digits, two per byte; whitespace between
/// them is ignored. The file lives only on the build machine (Documentation/Architecture/06 §5.2).
// RECOVERY-DECISION SP-D4: the key file is hex text, not raw bytes, so it survives copy and paste into a secret store.
bool ReadKey(StringView native, List<uint8> &key)
{
    String text;
    if (!ReadText(native, text))
    {
        return false;
    }
    key.Clear();
    uint32 digits = 0;
    uint8 byte = 0;
    for (const char c : text.AsView())
    {
        uint32 value = 0;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            continue;
        }
        if (!TryParse(StringView(&c, 1), value, 16))
        {
            return false;
        }
        byte = static_cast<uint8>((byte << 4) | value);
        if (++digits % 2 == 0)
        {
            key.Add(byte);
            byte = 0;
        }
    }
    return digits % 2 == 0 && !key.IsEmpty();
}

/// Adds the Config folder of *root*, mounted at *mountPoint*, and of each of its platform layers.
void AddConfig(PackageDesc &desc, StringView mountPoint, const String &root, bool layered)
{
    desc.Files.Add({String(mountPoint), String(), root, String("Config")});
    if (!layered)
    {
        return;
    }
    const StringView layers = MARLIN_PLATFORM_LAYERS;
    for (const StringView layer : layers.Split(';'))
    {
        const String directory = Format("{0}/Platforms/{1}", root, layer);
        if (PlatformFile::DirectoryExists(Format("{0}/Config", directory).CStr()))
        {
            desc.Files.Add({String(mountPoint), String(layer), directory, String("Config")});
        }
    }
}

/// The highest patch number among the packages in *folder*, collecting their paths into *packages*.
uint32 ReadPrevious(const String &folder, List<String> &packages)
{
    uint32 highest = 0;
    DirectoryIterator iterator;
    DirectoryEntry entry;
    if (!iterator.Open(folder.CStr()))
    {
        return 0;
    }
    while (iterator.Next(entry))
    {
        const StringView name(entry.Name);
        if (entry.Info.IsDirectory || !name.EndsWith(".mpak"))
        {
            continue;
        }
        packages.Add(Format("{0}/{1}", folder, name));
        uint32 number = 0;
        if (name.StartsWith("Patch_") && TryParse(name.Substring(6, 4), number) && number > highest)
        {
            highest = number;
        }
    }
    return highest;
}

String TextField(const MarlinTextObject &object, StringView key)
{
    const MarlinTextField *field = object.Find(key);
    return field != nullptr && field->Value.Kind == MarlinTextValueKind::String ? String(field->Value.Text) : String();
}

/// Writes *bytes* to native file *native* as hex text, the form ReadKey reads.
bool WriteHex(const String &native, Span<const uint8> bytes)
{
    constexpr char Digits[] = "0123456789abcdef";
    StringBuilder<512> text;
    for (const uint8 byte : bytes)
    {
        text.Append(Digits[byte >> 4]).Append(Digits[byte & 15]);
    }
    text.Append('\n');
    const FileHandle file = PlatformFile::Open(native.CStr(), FileAccess::Write, FileMode::CreateNew);
    if (!file.IsValid())
    {
        return false;
    }
    const bool ok = PlatformFile::Write(file, text.Data(), text.Length());
    PlatformFile::Close(file);
    return ok;
}

/// `-GenerateSigningKey=<file> [-Scheme=EcdsaP256]`: writes a new private key to *file* and its public key to
/// `<file>.pub`, both as hex. Neither file may exist yet. The private key stays on the build machine; the public key
/// goes into the product's PakVerifyKeyProvider.
int32 GenerateSigningKey(StringView path)
{
    StringView name = "EcdsaP256";
    (void)CommandLine::TryGetValue("Scheme", name);
    PakCrypto crypto;
    crypto.Build({});
    const PakSignatureScheme *scheme = crypto.FindScheme(name);
    List<uint8> privateKey;
    List<uint8> publicKey;
    if (scheme == nullptr || !scheme->Generate(privateKey, publicKey))
    {
        MARLIN_LOG(LogCore, Error, "No linked PakSignatureScheme called {0} can make a key", name);
        return 1;
    }
    const String privatePath(path);
    const String publicPath = Format("{0}.pub", path);
    if (!WriteHex(privatePath, privateKey.AsSpan()) || !WriteHex(publicPath, publicKey.AsSpan()))
    {
        MARLIN_LOG(LogCore, Error, "{0} or {1} cannot be written; neither may exist yet", privatePath, publicPath);
        return 1;
    }
    MemZero(privateKey.Data(), privateKey.Count());
    MARLIN_LOG(LogCore, Info, "Wrote the {0} private key to {1} and its public key to {2}", scheme->GetName(),
               privatePath, publicPath);
    return 0;
}
} // namespace

/// `MarlinPackager -Project=<.mproject> [-Output=<folder>] [-Previous=<folder of the last release>] [-Server]
/// [-Configuration=Shipping] [-LoadOrder=<file>] [-Key=<file>] [-SigningKey=<file>]`: packages the project for this
/// platform (Documentation/Architecture/06 §5). -LoadOrder takes what a run with AssetSubsystem's RecordLoadOrder
/// wrote, one Guid per line, and lays those objects out first; -Key is the key file of an encrypted release and
/// -SigningKey the private key of a signed one. `MarlinPackager -GenerateSigningKey=<file>` makes a signing key pair.
int main(int /*argc*/, const char *const * /*argv*/)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinPackager",
                                 ProgramFeatures::Vfs | ProgramFeatures::Project | ProgramFeatures::Config))
    {
        return 1;
    }
    // Without subsystems nothing starts the workers, and PakWriter would compress and the packager read on this
    // thread alone.
    const bool startedJobs = !JobSystem::IsRunning() && JobSystem::Start();
    const EngineEnvironment &environment = Engine::GetEnvironment();
    int32 exitCode = 1;
    do
    {
        StringView generate;
        if (CommandLine::TryGetValue("GenerateSigningKey", generate))
        {
            exitCode = GenerateSigningKey(generate);
            break;
        }
        if (!environment.HasProject())
        {
            MARLIN_LOG(LogCore, Error, "MarlinPackager needs -Project=<path to .mproject>");
            break;
        }
        const String productPath = Format("{0}/Config/Product.mtext", environment.ProjectRoot);
        String productText;
        MarlinTextDocument document;
        MarlinTextDiagnostics diagnostics;
        const MarlinTextObject *product = nullptr;
        if (Vfs::ReadAll("/Project/Config/Product.mtext", productText) &&
            document.Parse(productText.AsView(), diagnostics))
        {
            for (usize i = 0; i < document.GetObjectCount() && product == nullptr; ++i)
            {
                const MarlinTextObject *object = document.GetObject(i, diagnostics);
                product = object != nullptr && object->GetTypeName() == "Product" ? object : nullptr;
            }
        }
        if (product == nullptr)
        {
            MARLIN_LOG(LogCore, Error, "{0} has no Product", productPath);
            break;
        }

        PackageDesc desc;
        if (const MarlinTextField *startup = product->Find("StartupLevel");
            startup != nullptr && startup->Value.Kind == MarlinTextValueKind::ObjectLink)
        {
            desc.Roots.Add(startup->Value.LinkGuid);
        }
        if (const MarlinTextField *always = product->Find("AlwaysCook");
            always != nullptr && always->Value.Kind == MarlinTextValueKind::Array)
        {
            for (const MarlinTextValue &element : always->Value.Elements)
            {
                if (element.Kind == MarlinTextValueKind::ObjectLink)
                {
                    desc.Roots.Add(element.LinkGuid);
                }
                else if (element.Kind == MarlinTextValueKind::String)
                {
                    desc.RootFolders.Add(String(element.Text));
                }
            }
        }
        if (desc.Roots.IsEmpty() && desc.RootFolders.IsEmpty())
        {
            MARLIN_LOG(LogCore, Error, "{0} names neither a StartupLevel nor AlwaysCook", productPath);
            break;
        }
        (void)Config::Apply("Engine.Packaging", TypeOf<PackagingSettings>(), &desc.Settings);
        // A packaged game loads its global shaders from the packages, compiled for this platform's profiles.
        desc.GlobalShaders = true;
        // And its scripts' bytecode modules (MarlinScript §11.1): Shipping carries the VM, never the compiler.
        desc.ScriptModules = true;

        StringView loadOrder;
        if (CommandLine::TryGetValue("LoadOrder", loadOrder))
        {
            String text;
            if (!ReadText(loadOrder, text))
            {
                MARLIN_LOG(LogCore, Error, "{0} cannot be read", loadOrder);
                break;
            }
            bool valid = true;
            for (const StringView line : text.AsView().Split('\n'))
            {
                const StringView trimmed = line.Trim();
                Guid uuid;
                if (trimmed.IsEmpty())
                {
                    continue;
                }
                if (!Guid::TryParse(trimmed, uuid))
                {
                    MARLIN_LOG(LogCore, Error, "{0}: {1} is not a Guid", loadOrder, trimmed);
                    valid = false;
                    break;
                }
                desc.LoadOrder.Add(uuid);
            }
            if (!valid)
            {
                break;
            }
        }
        if (!desc.Settings.Encryption.Cipher.IsEmpty())
        {
            StringView keyFile;
            if (!CommandLine::TryGetValue("Key", keyFile) || !ReadKey(keyFile, desc.EncryptionKey))
            {
                MARLIN_LOG(LogCore, Error,
                           "Engine.Packaging.Encryption names {0}: MarlinPackager needs -Key=<key file>",
                           desc.Settings.Encryption.Cipher);
                break;
            }
        }
        if (!desc.Settings.Signing.Scheme.IsEmpty())
        {
            StringView keyFile;
            if (!CommandLine::TryGetValue("SigningKey", keyFile) || !ReadKey(keyFile, desc.SigningKey))
            {
                MARLIN_LOG(LogCore, Error,
                           "Engine.Packaging.Signing names {0}: MarlinPackager needs -SigningKey=<private key file>",
                           desc.Settings.Signing.Scheme);
                break;
            }
        }

        AssetIndex index;
        // From the indexer's snapshot, the scan reads only what changed since.
        (void)LoadProjectIndex(index);
        AssetScanner scanner(index, GetProjectContentFolders());
        List<String> changed;
        scanner.ScanAll(changed);
        desc.Index = &index;
        desc.Target.Platform = environment.Platform;
        desc.Target.Kind = CommandLine::HasSwitch("Server") ? TargetKind::Server : TargetKind::Client;

        AddConfig(desc, "/Engine", environment.EngineRoot, true);
        AddConfig(desc, "/Project", environment.ProjectRoot, false);

        StringView output;
        const String outputFolder =
            CommandLine::TryGetValue("Output", output)
                ? String(output)
                : Format("{0}/Saved/Packages/{1}", environment.ProjectRoot, environment.Platform);
        desc.OutputDirectory = Format("{0}/Paks", outputFolder);
        desc.StateDirectory = Format("{0}/Intermediate/Packaging", environment.ProjectRoot);
        StringView previous;
        if (CommandLine::TryGetValue("Previous", previous))
        {
            desc.PatchNumber = ReadPrevious(Format("{0}/Paks", previous), desc.Previous) + 1;
            if (desc.Previous.IsEmpty())
            {
                MARLIN_LOG(LogCore, Error, "{0}/Paks holds no packages to patch", previous);
                break;
            }
        }
        (void)PlatformFile::CreateDirectory(outputFolder.CStr());

        PackageResult result;
        StringBuilder<4096> errors;
        if (!BuildPackages(desc, result, errors))
        {
            MARLIN_LOG(LogCore, Error, "Packaging failed:\n{0}", errors.AsView());
            break;
        }
        // RECOVERY-DECISION SP-D5: packages an earlier run left in the output folder are reported, never deleted.
        {
            DirectoryIterator packages;
            DirectoryEntry entry;
            if (packages.Open(desc.OutputDirectory.CStr()))
            {
                while (packages.Next(entry))
                {
                    const String path = Format("{0}/{1}", desc.OutputDirectory, entry.Name);
                    if (!entry.Info.IsDirectory && StringView(entry.Name).EndsWith(".mpak") &&
                        !result.Packages.Contains(path))
                    {
                        MARLIN_LOG(LogCore, Warning, "{0} is not part of this release; remove it before shipping",
                                   path);
                    }
                }
            }
        }

        const String productName = TextField(*product, "ProductName");
        StringBuilder<1024> packaged;
        packaged.Append("!Version \"1.0\"\n\nProduct\n{\n");
        for (const StringView key :
             {StringView("Company"), StringView("ProductName"), StringView("DisplayName"), StringView("Version")})
        {
            packaged.Append(Format("    {0} = \"{1}\"\n", key, TextField(*product, key)));
        }
        // The packaged game opens this level: it is one of the roots, so it is in the packages (02 §4).
        if (const MarlinTextField *startup = product->Find("StartupLevel");
            startup != nullptr && startup->Value.Kind == MarlinTextValueKind::ObjectLink)
        {
            packaged.Append(Format("    StartupLevel = {0}\n", startup->Value.Source));
        }
        packaged.Append("}\n");
        const String packagedPath = Format("{0}/Product.mtext", outputFolder);
        const FileHandle file = PlatformFile::Open(packagedPath.CStr(), FileAccess::Write, FileMode::CreateAlways);
        if (!file.IsValid() || !PlatformFile::Write(file, packaged.Data(), packaged.Length()))
        {
            MARLIN_LOG(LogCore, Error, "{0} cannot be written", packagedPath);
            if (file.IsValid())
            {
                PlatformFile::Close(file);
            }
            break;
        }
        PlatformFile::Close(file);

        StringView configuration = "Shipping";
        (void)CommandLine::TryGetValue("Configuration", configuration);
        const String binaries = Format("{0}/{1}", ParentOf(environment.ExecutableDirectory.AsView()), configuration);
        const String game = Format("{0}/Game.exe", binaries);
        if (!FileExists(game))
        {
            MARLIN_LOG(LogCore, Error, "{0} is missing: build Game for {1} first", game, configuration);
            break;
        }
        bool copied = CopyFile(game, Format("{0}/{1}.exe", outputFolder,
                                            productName.IsEmpty() ? StringView("Game") : productName.AsView()));
        DirectoryIterator libraries;
        DirectoryEntry entry;
        if (libraries.Open(binaries.CStr()))
        {
            while (libraries.Next(entry))
            {
                if (!entry.Info.IsDirectory && StringView(entry.Name).EndsWith(".dll"))
                {
                    copied = CopyFile(Format("{0}/{1}", binaries, entry.Name),
                                      Format("{0}/{1}", outputFolder, entry.Name)) &&
                             copied;
                }
            }
        }
        if (!copied)
        {
            MARLIN_LOG(LogCore, Error, "The executable could not be copied to {0}", outputFolder);
            break;
        }
        MARLIN_LOG(LogCore, Info,
                   "Packaged {0} objects, {1} global shaders, {2} script modules and {3} files into {4} packages in "
                   "{5}{6}",
                   result.Objects, result.Shaders, result.ScriptModules, result.Files, result.Packages.Count(),
                   outputFolder,
                   desc.Previous.IsEmpty() ? String()
                                           : Format(" ({0} unchanged since the last release, {1} removed)",
                                                    result.Unchanged, result.Deleted));
        exitCode = 0;
    } while (false);
    if (startedJobs)
    {
        JobSystem::Stop();
    }
    ProgramHost::Shutdown();
    return exitCode;
}
