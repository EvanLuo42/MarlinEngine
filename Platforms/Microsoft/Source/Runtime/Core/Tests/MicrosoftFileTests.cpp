#include "HAL/MicrosoftHeaders.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "String/Format.h"
#include "String/String.h"
#include "Testing/Testing.h"

#include <winioctl.h>

namespace Marlin::Tests
{
namespace
{
/// The mount point form of REPARSE_DATA_BUFFER, which only the driver headers declare.
struct JunctionData
{
    DWORD Tag;
    WORD DataLength;
    WORD Reserved;
    WORD SubstituteOffset;
    WORD SubstituteLength;
    WORD PrintOffset;
    WORD PrintLength;
    wchar_t Names[MAX_PATH + 8];
};

/// Creates the directory *link* as a junction to *target*; junctions need no privilege, unlike symbolic links.
bool CreateJunction(const String &link, const String &target)
{
    if (!PlatformFile::CreateDirectory(link.CStr()))
    {
        return false;
    }
    const Microsoft::WidePath wideTarget(target.CStr());
    wchar_t full[MAX_PATH];
    const DWORD length = wideTarget.Get() != nullptr ? GetFullPathNameW(wideTarget.Get(), MAX_PATH, full, nullptr) : 0;
    if (length == 0 || length >= MAX_PATH)
    {
        return false;
    }
    JunctionData data = {};
    data.Tag = IO_REPARSE_TAG_MOUNT_POINT;
    const wchar_t prefix[] = L"\\??\\";
    WORD count = 0;
    for (const wchar_t *c = prefix; *c != L'\0'; ++c)
    {
        data.Names[count++] = *c;
    }
    for (DWORD i = 0; i < length; ++i)
    {
        data.Names[count++] = full[i];
    }
    // The substitute name, its terminator, then an empty print name and its terminator.
    data.SubstituteLength = static_cast<WORD>(count * sizeof(wchar_t));
    data.PrintOffset = static_cast<WORD>((count + 1) * sizeof(wchar_t));
    data.DataLength = static_cast<WORD>(4 * sizeof(WORD) + (count + 2) * sizeof(wchar_t));

    const Microsoft::WidePath wideLink(link.CStr());
    const HANDLE handle = CreateFileW(wideLink.Get(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD returned = 0;
    const bool created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data, data.DataLength + 8, nullptr, 0,
                                         &returned, nullptr) != 0;
    CloseHandle(handle);
    return created;
}

/// Removes a junction itself, never what it leads to.
void RemoveJunction(const String &link)
{
    const Microsoft::WidePath wideLink(link.CStr());
    (void)RemoveDirectoryW(wideLink.Get());
}

String ParentOf(StringView path)
{
    StringView parent;
    StringView name;
    return path.SplitLast('/', parent, name) ? String(parent) : String();
}

String KnownFolderPath(KnownFolder folder)
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(folder, buffer, sizeof(buffer));
    return length > 0 && length < sizeof(buffer) ? String(StringView(buffer, length)) : String();
}

String TempFolder()
{
    return KnownFolderPath(KnownFolder::Temp);
}
} // namespace

MARLIN_TEST(PlatformFile, SafeTreeRootsGuardProfiles)
{
    const String home = KnownFolderPath(KnownFolder::Home);
    MARLIN_REQUIRE(!home.IsEmpty());
    const String profiles = ParentOf(home);
    // The profile's own folders, such as a source or OneDrive folder, whether or not they exist.
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinMissing", home).CStr()));
    MARLIN_EXPECT(PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinMissing/Deeper", home).CStr()));
    // Another profile beside this one, and its own folders.
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinOther", profiles).CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinOther/Desktop", profiles).CStr()));
    MARLIN_EXPECT(PlatformFile::IsSafeTreeRoot(Format("{0}/MarlinOther/Desktop/Deeper", profiles).CStr()));
}

MARLIN_TEST(PlatformFile, DeleteDirectoryKeepsLinkTargets)
{
    const String temp = TempFolder();
    MARLIN_REQUIRE(!temp.IsEmpty());
    const String root = Format("{0}/MarlinLinkTest.{1}", temp, PlatformProcess::CurrentId());
    const String victim = Format("{0}/Victim", root);
    const String keep = Format("{0}/Keep.txt", victim);
    const String tree = Format("{0}/Tree", root);
    const String inner = Format("{0}/Link", tree);
    const String outer = Format("{0}/Outer", root);
    (void)PlatformFile::DeleteDirectory(root.CStr(), true);
    MARLIN_REQUIRE(PlatformFile::CreateDirectory(victim.CStr()) && PlatformFile::CreateDirectory(tree.CStr()));
    const FileHandle file = PlatformFile::Open(keep.CStr(), FileAccess::Write, FileMode::CreateAlways);
    MARLIN_REQUIRE(file.IsValid());
    PlatformFile::Close(file);
    MARLIN_REQUIRE(CreateJunction(inner, victim) && CreateJunction(outer, victim));

    // A junction inside the tree goes with the tree; what it leads to stays.
    MARLIN_EXPECT(PlatformFile::DeleteDirectory(tree.CStr(), true));
    MARLIN_EXPECT(!PlatformFile::DirectoryExists(tree.CStr()) && PlatformFile::FileExists(keep.CStr()));
    // A junction named directly is removed, not walked.
    MARLIN_EXPECT(PlatformFile::DeleteDirectory(outer.CStr(), true));
    MARLIN_EXPECT(!PlatformFile::DirectoryExists(outer.CStr()) && PlatformFile::FileExists(keep.CStr()));

    MARLIN_EXPECT(PlatformFile::DeleteDirectory(root.CStr(), true) && !PlatformFile::DirectoryExists(root.CStr()));
}

MARLIN_TEST(PlatformFile, SafeTreeRootsFollowLinks)
{
    const String temp = TempFolder();
    MARLIN_REQUIRE(!temp.IsEmpty());
    char buffer[1024];
    const usize length = PlatformProcess::ExecutablePath(buffer, sizeof(buffer));
    MARLIN_REQUIRE(length > 0 && length < sizeof(buffer));
    // The executable's directory stands in for the user's folders: protected the same way, and rebuilt if lost.
    const String executableDirectory = ParentOf(StringView(buffer, length));
    const String executableParent = ParentOf(executableDirectory);
    StringView parent;
    StringView name;
    MARLIN_REQUIRE(executableDirectory.AsView().SplitLast('/', parent, name));

    const String root = Format("{0}/MarlinLinkGuard.{1}", temp, PlatformProcess::CurrentId());
    const String direct = Format("{0}/Direct", root);
    const String above = Format("{0}/Above", root);
    (void)PlatformFile::DeleteDirectory(root.CStr(), true);
    MARLIN_REQUIRE(PlatformFile::CreateDirectory(root.CStr()));
    MARLIN_REQUIRE(CreateJunction(direct, executableDirectory) && CreateJunction(above, executableParent));

    MARLIN_EXPECT(PlatformFile::IsSafeTreeRoot(root.CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(direct.CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(above.CStr()));
    MARLIN_EXPECT(!PlatformFile::IsSafeTreeRoot(Format("{0}/{1}", above, name).CStr()));

    RemoveJunction(direct);
    RemoveJunction(above);
    MARLIN_EXPECT(PlatformFile::DeleteDirectory(root.CStr(), true));
}
} // namespace Marlin::Tests
