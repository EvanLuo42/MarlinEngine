#include "Testing/Testing.h"

#include "CrashAnalysis/CrashAnalysis.h"
#include "CrashAnalysis/Minidump.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformFile.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformSystem.h"
#include "HAL/PlatformThread.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
namespace
{
// RECOVERY-DECISION D19: test folders live under %TEMP% (recovery safety rule), not beside the executable.
/// A fresh `%TEMP%/MarlinTests/CrashAnalysisTests`; only that folder is ever deleted.
String TestDirectory()
{
    char buffer[1024];
    const usize length = PlatformSystem::GetKnownFolder(KnownFolder::Temp, buffer, sizeof(buffer));
    MARLIN_ASSERT(length > 3 && length < sizeof(buffer));
    const String base = Format("{0}/MarlinTests/", StringView(buffer, length));
    const String directory = Format("{0}CrashAnalysisTests", base);
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
    (void)PlatformFile::CreateDirectory(directory.CStr());
    return directory;
}

CrashFrame Frame(StringView module, StringView function, uint64 offset = 0)
{
    CrashFrame frame;
    frame.Module = String(module);
    frame.Function = String(function);
    frame.Offset = offset;
    return frame;
}
} // namespace

MARLIN_TEST(CrashAnalysis, WalksTheStackOfThisProcess)
{
    const String directory = TestDirectory();
    const String dump = Format("{0}/Self.dmp", directory);
    MARLIN_REQUIRE(PlatformCrash::WriteMinidump(dump.CStr(), PlatformProcess::CurrentId(), nullptr));

    CrashAnalysisDesc desc;
    desc.Minidump = dump;
    desc.FocusThread = PlatformThread::CurrentId();
    CrashAnalysisResult result;
    String error;
    MARLIN_REQUIRE(CrashAnalyzer::Analyze(desc, result, error));
    MARLIN_EXPECT(!result.HasException);
    MARLIN_EXPECT(result.FocusThread == desc.FocusThread);
    MARLIN_EXPECT(!result.SignatureText.IsEmpty());
    bool found = false;
    for (const CrashThreadStack &stack : result.Threads)
    {
        if (!stack.Focus)
        {
            continue;
        }
        for (const CrashFrame &frame : stack.Frames)
        {
            found |= frame.Function.Contains("WalksTheStackOfThisProcess");
        }
    }
    StringBuilder<4096> text;
    CrashAnalyzer::WriteText(text, result);
    MARLIN_EXPECTF(found, "the walk lost the test's own frames:\n{0}", text.AsView());
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(CrashAnalysis, RejectsFilesThatAreNotMinidumps)
{
    const String directory = TestDirectory();
    const String path = Format("{0}/NotADump.dmp", directory);
    const StringView text = "MDMX this is not a minidump, but long enough to have a header";
    const FileHandle file = PlatformFile::Open(path.CStr(), FileAccess::Write, FileMode::CreateAlways);
    MARLIN_REQUIRE(file.IsValid());
    MARLIN_REQUIRE(PlatformFile::Write(file, text.Data(), text.Length()));
    PlatformFile::Close(file);

    Minidump dump;
    String error;
    MARLIN_EXPECT(!dump.Open(path, error) && !error.IsEmpty());
    MARLIN_EXPECT(!dump.Open(Format("{0}/Missing.dmp", TestDirectory()), error));
    (void)PlatformFile::DeleteDirectory(directory.CStr(), true);
}

MARLIN_TEST(CrashAnalysis, SignatureSkipsCrashMachinery)
{
    const CrashFrame frames[] = {
        Frame("Marlin-Core.dll", "Marlin::Private::AssertFailed"),
        Frame("Game.exe", "Marlin::Game::Fire", 0x10),
        Frame("Game.exe", "Marlin::Game::Update", 0x20),
        Frame("Marlin-Engine.dll", "Marlin::Engine::RunLoop", 0x30),
        Frame("Game.exe", "main", 0x40),
        Frame("KERNEL32.DLL", "BaseThreadInitThunk", 0x50),
        Frame("ntdll.dll", "RtlUserThreadStart", 0x60),
    };
    String text;
    const Hash128 signature = CrashAnalyzer::ComputeSignature(frames, text);
    MARLIN_EXPECT(!text.IsEmpty() && !text.Contains("AssertFailed"));

    String same;
    String other;
    MARLIN_EXPECT(CrashAnalyzer::ComputeSignature(Span<const CrashFrame>(frames).Slice(1), same) == signature);
    MARLIN_EXPECT(same == text);
    MARLIN_EXPECT(CrashAnalyzer::ComputeSignature(Span<const CrashFrame>(frames).Slice(5), other) != signature);
    MARLIN_EXPECT(CrashAnalyzer::FormatSignature(signature).Length() == 32);
}

MARLIN_TEST(CrashAnalysis, SymbolStorePaths)
{
    MinidumpModule module;
    module.Path = "C:/Game/Binaries/Game.exe";
    module.TimeStamp = 0x0ABCDEF1u;
    module.Size = 0x7000;
    module.DebugFile = "Game.pdb";
    const uint8 id[] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    module.DebugId.AddRange(Span<const uint8>(id));
    module.DebugAge = 3;
    MARLIN_EXPECT(CrashAnalyzer::GetImagePath("S:/Store", module) == "S:/Store/Game.exe/0ABCDEF17000/Game.exe");
    MARLIN_EXPECT(CrashAnalyzer::GetDebugFilePath("S:/Store", module) ==
                  "S:/Store/Game.pdb/123456789ABCDEF00123456789ABCDEF3/Game.pdb");
    module.DebugFile.Clear();
    MARLIN_EXPECT(CrashAnalyzer::GetDebugFilePath("S:/Store", module).IsEmpty());
}
} // namespace Marlin::Tests
