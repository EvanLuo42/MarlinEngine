#include "Testing/Testing.h"

#include "CrashReport/CrashPrivacy.h"
#include "CrashReport/CrashReportInfo.h"

namespace Marlin::Tests
{
MARLIN_TEST(CrashReport, InfoRoundTrip)
{
    CrashReportInfo info;
    info.Kind = CrashKind::Failure;
    info.ProcessId = 1234;
    info.ExceptionCode = 0xC000001Du;
    info.Address = 0x7FFC88CB481Aull;
    info.FailureMessage = "Assertion failed: \"x\" != nullptr\nsecond line";
    info.FailureFile = "C:\\Source\\File.cpp";
    info.FailureLine = 42;
    info.TargetName = "Game";
    info.Attachments.Add(String("GpuCrash.nv-gpudmp"));

    CrashReportInfo read;
    String errors;
    MARLIN_REQUIRE(CrashReport::ReadInfo(CrashReport::WriteInfo(info), read, errors));
    MARLIN_EXPECT(read.Kind == CrashKind::Failure);
    MARLIN_EXPECT(read.ProcessId == 1234 && read.ExceptionCode == 0xC000001Du && read.Address == info.Address);
    MARLIN_EXPECT(read.FailureMessage == info.FailureMessage);
    MARLIN_EXPECT(read.FailureFile == info.FailureFile && read.FailureLine == 42);
    MARLIN_EXPECT(read.TargetName == "Game");
    MARLIN_EXPECT(read.Attachments.Count() == 1 && read.Attachments[0] == "GpuCrash.nv-gpudmp");
}

MARLIN_TEST(CrashReport, ReadInfoRejectsOtherText)
{
    CrashReportInfo info;
    String errors;
    MARLIN_EXPECT(!CrashReport::ReadInfo("Something { Kind = Hang }", info, errors) && !errors.IsEmpty());
    MARLIN_EXPECT(!CrashReport::ReadInfo("not { marlin text", info, errors));
}

MARLIN_TEST(CrashPrivacy, ScrubsPathsAndWholeNames)
{
    const CrashScrubPattern patterns[] = {
        {.Text = String("C:/Users/Alice"), .Placeholder = "<Home>"},
        {.Text = String("C:\\Users\\Alice"), .Placeholder = "<Home>"},
        {.Text = String("Alice"), .Placeholder = "<User>", .WholeWord = true},
        {.Text = String("//DESKTOP-42"), .Placeholder = "//<Computer>", .WholeWord = true},
    };
    MARLIN_EXPECT(CrashPrivacy::Scrub("Saved to c:/users/alice/Saved/x.dmp", patterns) ==
                  "Saved to <Home>/Saved/x.dmp");
    MARLIN_EXPECT(CrashPrivacy::Scrub("C:\\Users\\Alice\\Game.exe", patterns) == "<Home>\\Game.exe");
    MARLIN_EXPECT(CrashPrivacy::Scrub("hello alice, Alicea stays", patterns) == "hello <User>, Alicea stays");
    MARLIN_EXPECT(CrashPrivacy::Scrub("share //DESKTOP-42/Builds", patterns) == "share //<Computer>/Builds");
    MARLIN_EXPECT(CrashPrivacy::Scrub("", patterns).IsEmpty());
}

MARLIN_TEST(CrashPrivacy, LocalPatternsNeverMatchShortNames)
{
    for (const CrashScrubPattern &pattern : CrashPrivacy::MakeLocalPatterns())
    {
        MARLIN_EXPECT(pattern.Text.Length() >= 3);
    }
}

MARLIN_TEST(CrashPrivacy, FilterCommandLineKeepsOnlyNamedValues)
{
    const StringView kept[] = {"Rhi", "map"};
    MARLIN_EXPECT(CrashPrivacy::FilterCommandLine("-Rhi=Vulkan -AuthToken=abc123 -Map Town -Windowed", kept) ==
                  "-Rhi=Vulkan -AuthToken=<removed> -Map Town -Windowed");
    MARLIN_EXPECT(CrashPrivacy::FilterCommandLine(
                      "C:/Users/Alice/Game.exe \"-Project=C:/Users/Alice/My Game.mproject\" -Secret value", kept) ==
                  "\"-Project=<removed> -Secret");
    MARLIN_EXPECT(CrashPrivacy::FilterCommandLine("-Config:Engine.X=1", kept) == "-Config=<removed>");
    MARLIN_EXPECT(CrashPrivacy::FilterCommandLine("", kept).IsEmpty());
}
} // namespace Marlin::Tests
