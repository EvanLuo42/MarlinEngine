#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "String/Format.h"
#include "String/String.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
String KnownFolderPath(KnownFolder folder)
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(folder, buffer, sizeof(buffer));
    return length > 0 && length < sizeof(buffer) ? String(StringView(buffer, length)) : String();
}

String ParentOf(StringView path)
{
    StringView parent;
    StringView name;
    return path.SplitLast('/', parent, name) ? String(parent) : String();
}

void WriteFile(const String &path, StringView text)
{
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
    MARLIN_REQUIRE(file.IsValid() && PlatformFile::Write(file, text.Data(), text.Length()));
    PlatformFile::Close(file);
}
} // namespace

MARLIN_TEST(PlatformFile, SafeTreeRoots)
{
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(""));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot("/"));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot("."));

    for (const KnownFolder folder :
         {KnownFolder::Home, KnownFolder::LocalAppData, KnownFolder::RoamingAppData, KnownFolder::Documents,
          KnownFolder::SavedGames, KnownFolder::Temp, KnownFolder::Desktop, KnownFolder::Downloads,
          KnownFolder::Pictures, KnownFolder::Music, KnownFolder::Videos})
    {
        const String path = KnownFolderPath(folder);
        if (path.IsEmpty())
        {
            continue;
        }
        MARLIN_EXPECTF(!PlatformFile::IsSafeTreeRoot(path.CStr()), "{0}", path);
        MARLIN_EXPECTF(!PlatformFile::IsSafeTreeRoot(ParentOf(path).CStr()), "{0}", ParentOf(path));
    }

    const String home = KnownFolderPath(KnownFolder::Home);
    MARLIN_REQUIRE(!home.IsEmpty());
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/", home).CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/.", home).CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinMissing/..", home).CStr()));

    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    MARLIN_REQUIRE(length > 0 && length < sizeof(buffer));
    const String executableDirectory = ParentOf(StringView(buffer, length));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(executableDirectory.CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(ParentOf(executableDirectory).CStr()));
    MARLIN_EXPECT(PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinMissing", executableDirectory).CStr()));

    const String temp = KnownFolderPath(KnownFolder::Temp);
    MARLIN_REQUIRE(!temp.IsEmpty());
    MARLIN_EXPECT(PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinSafeTreeRoot", temp).CStr()));
}

MARLIN_TEST(PlatformFile, DeleteDirectory)
{
    const String temp = KnownFolderPath(KnownFolder::Temp);
    MARLIN_REQUIRE(!temp.IsEmpty());
    const String root = Format("{0}/MarlinDeleteDirectory.{1}", temp, PlatformProcess::CurrentId());
    (void)PlatformFile::DeleteDirectory(root.CStr(), true);
    MARLIN_REQUIRE(PlatformFile::CreateDirectory(Format("{0}/A/B", root).CStr()));
    WriteFile(Format("{0}/A/B/Deep.txt", root), "deep");
    WriteFile(Format("{0}/Top.txt", root), "top");
    MARLIN_REQUIRE(PlatformFile::SetReadOnly(Format("{0}/Top.txt", root).CStr(), true));

    MARLIN_EXPECT(!PlatformFile::DeleteDirectory(root.CStr(), false));
    MARLIN_EXPECT(!PlatformFile::DeleteDirectory(Format("{0}/Top.txt", root).CStr(), true));
    MARLIN_EXPECT(PlatformFile::DeleteDirectory(root.CStr(), true));
    MARLIN_EXPECT(!PlatformFile::DirectoryExists(root.CStr()));
    MARLIN_EXPECT(!PlatformFile::DeleteDirectory(root.CStr(), true));
}
} // namespace Marlin::Tests
