#include "Containers/HashSet.h"
#include "Math/Random.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "String/Format.h"
#include "Templates/NumericLimits.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
constexpr Guid ParsedGuid = [] {
    Guid guid;
    (void)Guid::TryParse("{01234567-89AB-cdef-0123-456789abcdef}", guid);
    return guid;
}();
static_assert(ParsedGuid.A == 0x01234567 && ParsedGuid.B == 0x89ABCDEF && ParsedGuid.D == 0x89ABCDEF);
static_assert(DateTime(2024, 2, 29).DayOfYear() == 60 && DateTime(2024, 3, 1).Month() == 3);
static_assert(DateTime(1970, 1, 1).GetTicks() == DateTime::UnixEpochTicks);
static_assert(DateTime(9999, 12, 31, 23, 59, 59, 999).GetTicks() + 9999 == DateTime::MaxTicks);
static_assert(DateTime(2026, 10, 27).GetDayOfWeek() == DayOfWeek::Tuesday);
static_assert(DateTime(2024, 1, 31).AddMonths(1) == DateTime(2024, 2, 29));
static_assert(Random(42).NextUInt64() == Random(42).NextUInt64());
} // namespace

MARLIN_TEST(Misc, Guid)
{
    {
        MARLIN_EXPECT(Format("{0}", ParsedGuid) == "01234567-89ab-cdef-0123-456789abcdef");
        MARLIN_EXPECT(Format("{0:n}", ParsedGuid) == "0123456789abcdef0123456789abcdef");
        MARLIN_EXPECT(Format("{0:D}", ParsedGuid) == "01234567-89AB-CDEF-0123-456789ABCDEF");
        Guid parsed;
        MARLIN_EXPECT(Guid::TryParse("0123456789abcdef0123456789abcdef", parsed) && parsed == ParsedGuid);
        MARLIN_EXPECT(!Guid::TryParse("01234567-89ab-cdef-0123-456789abcdeg", parsed));
        HashSet<Guid> guids;
        for (int32 i = 0; i < 1000; ++i)
        {
            const Guid guid = Guid::NewGuid();
            MARLIN_EXPECT(guid.IsValid() && ((guid.B >> 12) & 0xF) == 4 && (guid.C >> 30) == 2);
            guids.Add(guid);
        }
        MARLIN_EXPECT(guids.Count() == 1000 && !Guid().IsValid());
        // RFC 9562 A.4: the name-based Guid of "www.example.com" in the DNS namespace.
        Guid dns;
        MARLIN_EXPECT(Guid::TryParse("6ba7b810-9dad-11d1-80b4-00c04fd430c8", dns));
        const Guid named = Guid::FromName(dns, "www.example.com");
        MARLIN_EXPECT(Format("{0}", named) == "2ed6657d-e927-568b-95e1-2665a8aea6a2");
    }
}

MARLIN_TEST(Misc, DateTime)
{

    {
        const DateTime moment(2024, 7, 4, 13, 5, 9, 250);
        MARLIN_EXPECT(moment.Year() == 2024 && moment.Month() == 7 && moment.Day() == 4 && moment.Hour() == 13 &&
                      moment.Minute() == 5 && moment.Second() == 9 && moment.Millisecond() == 250);
        MARLIN_EXPECT(Format("{0}", moment) == "2024-07-04T13:05:09.2500000");
        MARLIN_EXPECT(Format("{0:s}", moment) == "2024-07-04T13:05:09");
        MARLIN_EXPECT(Format("{0:dd/MM/yy H:m 'at' fff}", moment) == "04/07/24 13:5 at 250");
        DateTime parsed;
        MARLIN_EXPECT(DateTime::TryParse("2024-07-04T13:05:09.25Z", parsed) && parsed == moment);
        MARLIN_EXPECT(DateTime::TryParse("2024-07-04", parsed) && parsed == moment.Date());
        MARLIN_EXPECT(!DateTime::TryParse("2023-02-29", parsed) && !DateTime::TryParse("2024-07-04T25:00", parsed));
        MARLIN_EXPECT(DateTime::FromUnixSeconds(1700000000).ToUnixSeconds() == 1700000000);
        MARLIN_EXPECT(DateTime::FromUnixSeconds(-1).ToUnixSeconds() == -1);
        int32 failuresInLoop = 0;
        for (int64 day = 0; day < 3652059; day += 97)
        {
            const DateTime date(day * TimeSpan::TicksPerDay);
            int32 year = 0;
            int32 month = 0;
            int32 dayOfMonth = 0;
            date.GetDate(year, month, dayOfMonth);
            failuresInLoop += DateTime(year, month, dayOfMonth) != date;
        }
        MARLIN_EXPECT(failuresInLoop == 0);
        const DateTime now = DateTime::UtcNow();
        MARLIN_EXPECT(now.Year() >= 2024 && now.Year() < 2100);
        MARLIN_EXPECT((DateTime::Now() - now).Duration() < TimeSpan(1, 0, 0, 0));

        const TimeSpan span(1, 2, 3, 4, 5);
        MARLIN_EXPECT(span.Days() == 1 && span.Hours() == 2 && span.Minutes() == 3 && span.Seconds() == 4 &&
                      span.Milliseconds() == 5);
        MARLIN_EXPECT(Format("{0}", span) == "1.02:03:04.0050000" &&
                      Format("{0}", -TimeSpan::FromSeconds(90)) == "-00:01:30");
        TimeSpan parsedSpan;
        MARLIN_EXPECT(TimeSpan::TryParse("1.02:03:04.005", parsedSpan) && parsedSpan == span);
        MARLIN_EXPECT(TimeSpan::TryParse("-00:01:30", parsedSpan) && parsedSpan == TimeSpan::FromMinutes(-1.5));
        MARLIN_EXPECT(TimeSpan::FromMilliseconds(1.5).GetTicks() == 15000 && span * 2 == span + span);
    }
}

MARLIN_TEST(Misc, CommandLine)
{

    {
        List<String> arguments;
        CommandLine::Split(R"(Game.exe -Map=Arena "-Name=Two Words" a\"b "c""d" e\\\"f -Flag)", arguments);
        MARLIN_EXPECT(arguments.Count() == 7 && arguments[2] == "-Name=Two Words" && arguments[3] == "a\"b" &&
                      arguments[4] == "c\"d" && arguments[5] == "e\\\"f");
        CommandLine::Set(R"(-map=Arena -Config:Engine.Fps=60 -Config:Engine.VSync=1 -Threads=8 -Verbose input.txt)");
        StringView value;
        MARLIN_EXPECT(CommandLine::TryGetValue("Map", value) && value == "Arena");
        MARLIN_EXPECT(CommandLine::HasSwitch("verbose") && !CommandLine::HasSwitch("Verb") &&
                      !CommandLine::HasSwitch("x"));
        int32 threads = 0;
        MARLIN_EXPECT(CommandLine::TryGetValue("Threads", threads) && threads == 8);
        int32 configs = 0;
        CommandLine::ForEachValue("Config", [&](StringView text) { configs += text.StartsWith("Engine.") ? 1 : 0; });
        MARLIN_EXPECT(configs == 2 && CommandLine::GetArguments().Count() == 6 &&
                      CommandLine::GetArguments()[5] == "input.txt");
    }
}

MARLIN_TEST(Misc, Random)
{

    {
        Random random(1234);
        int32 counts[6] = {};
        float64 sum = 0;
        for (int32 i = 0; i < 600000; ++i)
        {
            ++counts[random.NextInRange(0, 5)];
            const float64 unit = random.NextDouble();
            MARLIN_EXPECT(unit >= 0 && unit < 1);
            sum += random.NextGaussian();
        }
        for (const int32 count : counts)
        {
            MARLIN_EXPECT(count > 99000 && count < 101000);
        }
        MARLIN_EXPECT(sum / 600000 > -0.01 && sum / 600000 < 0.01);
        MARLIN_EXPECT(random.NextInRange(-5, -5) == -5 &&
                      random.NextInRange(NumericLimits<int64>::Min(), NumericLimits<int64>::Max()) != 0);
        int32 values[10] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        random.Shuffle(Span<int32>(values));
        int32 total = 0;
        for (const int32 v : values)
        {
            total += v;
        }
        MARLIN_EXPECT(total == 45);
        Random a(7);
        Random b(7);
        b.Jump();
        MARLIN_EXPECT(a.NextUInt64() != b.NextUInt64());
        MARLIN_EXPECT(ThreadRandom().NextUInt64() != ThreadRandom().NextUInt64());
    }
}
} // namespace Marlin::Tests
