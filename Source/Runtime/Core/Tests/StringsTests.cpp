#include "Containers/HashMap.h"
#include "Math/MathUtility.h"
#include "String/Format.h"
#include "String/Name.h"
#include "String/Number.h"
#include "String/String.h"
#include "String/StringBuilder.h"
#include "String/StringView.h"
#include "String/Utf.h"
#include "Templates/Bit.h"
#include "Templates/NumericLimits.h"
#include "Testing/Testing.h"

namespace Marlin::Tests
{
namespace
{
static_assert(StringView("hello").Length() == 5);
static_assert(StringView("hello world").IndexOf("world") == 6);
static_assert(StringView("a.b.c").LastIndexOf('.') == 3);
static_assert(StringView("  x ").Trim() == "x");
static_assert(StringView("Texture_Albedo").MatchesWildcard("Texture_*"));
static_assert(!StringView("Mesh").MatchesWildcard("Tex*"));
static_assert(StringView("abc").GetHash() == Hasher<const char *>::Get("abc"));
static_assert(StringView("abc") < StringView("abd"));
static_assert(StringView("ABC").EqualsIgnoreCase("abc"));

constexpr usize CountParts()
{
    usize count = 0;
    for (StringView part : StringView("a,,b,").Split(','))
    {
        count += part.Length() + 1;
    }
    return count;
}
static_assert(CountParts() == 6);

String Text(float64 value, FloatStyle style = FloatStyle::Shortest, int32 precision = 0)
{
    StringBuilder<> builder;
    AppendFloat(builder, value, style, precision);
    return builder.ToString();
}

String Text32(float32 value)
{
    StringBuilder<> builder;
    AppendFloat(builder, value);
    return builder.ToString();
}

uint64 _state = 0x9E3779B97F4A7C15ull;

uint64 NextRandom()
{
    _state ^= _state << 13;
    _state ^= _state >> 7;
    _state ^= _state << 17;
    return _state;
}

struct Point
{
    int32 X;
    int32 Y;

    void FormatTo(StringWriter &output, StringView /*spec*/) const
    {
        Marlin::FormatTo(output, "({0}, {1})", X, Y);
    }
};
} // namespace

MARLIN_TEST(Strings, String)
{
    {
        String empty;
        MARLIN_EXPECT(empty.IsEmpty() && empty.CStr()[0] == '\0' && empty.Capacity() == String::InlineCapacity);
        String inlined("12345678901234567890123");
        MARLIN_EXPECT(inlined.Length() == 23 && inlined.Capacity() == 23 && inlined.CStr()[23] == '\0');
        String heap("123456789012345678901234");
        MARLIN_EXPECT(heap.Length() == 24 && heap.Capacity() >= 24 && heap == "123456789012345678901234");
        inlined.Append('x');
        MARLIN_EXPECT(inlined == "12345678901234567890123x" && inlined.Length() == 24);

        String self("abc");
        for (int32 i = 0; i < 5; ++i)
        {
            self.Append(self);
        }
        MARLIN_EXPECT(self.Length() == 96 && self.StartsWith("abcabc") && self.EndsWith("cabc"));

        String text("hello world");
        text.Insert(5, ",");
        MARLIN_EXPECT(text == "hello, world");
        text.Remove(5, 1);
        MARLIN_EXPECT(text == "hello world");
        MARLIN_EXPECT(text.Replace("o", "0") == 2 && text == "hell0 w0rld");
        text.Replace('0', 'o');
        text.ToUpper();
        MARLIN_EXPECT(text == "HELLO WORLD");
        String padded("  trim me \t");
        padded.Trim();
        MARLIN_EXPECT(padded == "trim me");
        padded.Resize(10, '!');
        MARLIN_EXPECT(padded == "trim me!!!");
        String moved = Move(heap);
        MARLIN_EXPECT(heap.IsEmpty() && moved.Length() == 24);
        MARLIN_EXPECT(String("a") + "b" + String("c") == "abc");
        MARLIN_EXPECT(String("abc") < String("abd") && String("b") > StringView("a"));
        MARLIN_EXPECT(text.GetHash() == StringView("HELLO WORLD").GetHash());
        String large(1000, 'z');
        large.ShrinkToFit();
        MARLIN_EXPECT(large.Length() == 1000 && large[999] == 'z');

        HashMap<String, int32> map;
        map.Add("alpha", 1);
        map.Add(String("beta"), 2);
        const char *key = "beta";
        MARLIN_EXPECT(map.Find("alpha") != nullptr && *map.Find("alpha") == 1);
        MARLIN_EXPECT(map.Find(key) != nullptr && *map.Find(key) == 2);
        MARLIN_EXPECT(map.Find(StringView("gamma")) == nullptr);
    }
}

MARLIN_TEST(Strings, StringBuilder)
{

    {
        StringBuilder<8> builder;
        for (int32 i = 0; i < 100; ++i)
        {
            builder.Append("ab");
        }
        MARLIN_EXPECT(builder.Length() == 200 && builder.AsView().EndsWith("abab") &&
                      StringView(builder.CStr()).Length() == 200);
        builder.Clear();
        builder.Append(3, '-');
        MARLIN_EXPECT(builder.AsView() == "---");
    }
}

MARLIN_TEST(Strings, IntegerParsing)
{

    {
        int32 value = 0;
        MARLIN_EXPECT(TryParse("-2147483648", value) && value == -2147483647 - 1);
        MARLIN_EXPECT(!TryParse("2147483648", value));
        uint8 byte = 0;
        MARLIN_EXPECT(TryParse("255", byte) && byte == 255 && !TryParse("256", byte) && !TryParse("-1", byte));
        MARLIN_EXPECT(Parse<uint32>("ff").HasValue() == false);
        uint32 hex = 0;
        MARLIN_EXPECT(TryParse("ff", hex, 16) && hex == 255);
        MARLIN_EXPECT(!TryParse("", value) && !TryParse("+", value) && !TryParse("1 ", value));
        char buffer[MaxIntegerLength];
        MARLIN_EXPECT(StringView(buffer, FormatSigned(-9223372036854775807ll - 1, buffer)) == "-9223372036854775808");
        MARLIN_EXPECT(StringView(buffer, FormatUnsigned(255, buffer, 2)) == "11111111");
    }
}

MARLIN_TEST(Strings, FloatText)
{

    {
        MARLIN_EXPECT(Text(0.1) == "0.1");
        MARLIN_EXPECT(Text(1.0) == "1");
        MARLIN_EXPECT(Text(-0.0) == "-0");
        MARLIN_EXPECT(Text(123.456) == "123.456");
        MARLIN_EXPECT(Text(1e15) == "1E+15");
        MARLIN_EXPECT(Text(123456789012345.0) == "123456789012345");
        MARLIN_EXPECT(Text(1e21) == "1E+21");
        MARLIN_EXPECT(Text(0.0001) == "0.0001");
        MARLIN_EXPECT(Text(0.00001) == "1E-05");
        MARLIN_EXPECT(Text(5e-324) == "5E-324");
        MARLIN_EXPECT(Text(1.7976931348623157e308) == "1.7976931348623157E+308");
        MARLIN_EXPECT(Text(2.2250738585072014e-308) == "2.2250738585072014E-308");
        MARLIN_EXPECT(Text(0.3) == "0.3");
        MARLIN_EXPECT(Text(2.0 / 3.0) == "0.6666666666666666");
        MARLIN_EXPECT(Text(NumericLimits<float64>::QuietNaN()) == "NaN" &&
                      Text(-NumericLimits<float64>::Infinity()) == "-Infinity");
        MARLIN_EXPECT(Text(123.456, FloatStyle::Fixed, 2) == "123.46");
        MARLIN_EXPECT(Text(0.125, FloatStyle::Fixed, 2) == "0.13");
        MARLIN_EXPECT(Text(2.5, FloatStyle::Fixed, 0) == "3");
        MARLIN_EXPECT(Text(0.004, FloatStyle::Fixed, 2) == "0.00");
        MARLIN_EXPECT(Text(0.005, FloatStyle::Fixed, 2) == "0.01");
        MARLIN_EXPECT(Text(0.0, FloatStyle::Fixed, 3) == "0.000");
        MARLIN_EXPECT(Text(999.996, FloatStyle::Fixed, 2) == "1000.00");
        MARLIN_EXPECT(Text(1e20, FloatStyle::Fixed, 1) == "100000000000000000000.0");
        MARLIN_EXPECT(Text(1234.5678, FloatStyle::Exponent, 2) == "1.23E+003");
        MARLIN_EXPECT(Text(1234.5678, FloatStyle::General, 6) == "1234.57");
        MARLIN_EXPECT(Text(0.1, FloatStyle::Fixed, 20) == "0.10000000000000000555");
        MARLIN_EXPECT(Text32(0.1f) == "0.1");
        MARLIN_EXPECT(Text32(16777216.0f) == "16777216");
        MARLIN_EXPECT(Text32(3.4028235e38f) == "3.4028235E+38");
        MARLIN_EXPECT(Text32(1e-45f) == "1E-45");

        float64 parsed = 0;
        MARLIN_EXPECT(TryParse("0.1", parsed) && parsed == 0.1);
        MARLIN_EXPECT(TryParse("-1.5e3", parsed) && parsed == -1500.0);
        MARLIN_EXPECT(TryParse("2.2250738585072011e-308", parsed) && BitCast<uint64>(parsed) == 0x000FFFFFFFFFFFFFull);
        MARLIN_EXPECT(TryParse("4.9406564584124654e-324", parsed) && BitCast<uint64>(parsed) == 1);
        MARLIN_EXPECT(TryParse("2.4703282292062327e-324", parsed) && parsed == 0.0);
        MARLIN_EXPECT(TryParse("2.4703282292062328e-324", parsed) && BitCast<uint64>(parsed) == 1);
        MARLIN_EXPECT(TryParse("1e400", parsed) && parsed == NumericLimits<float64>::Infinity());
        MARLIN_EXPECT(TryParse("9007199254740993", parsed) && parsed == 9007199254740992.0);
        MARLIN_EXPECT(TryParse("9007199254740993.0000000000000000000001", parsed) && parsed == 9007199254740994.0);
        MARLIN_EXPECT(TryParse(".5", parsed) && parsed == 0.5 && TryParse("5.", parsed) && parsed == 5.0);
        MARLIN_EXPECT(TryParse("Infinity", parsed) && parsed == NumericLimits<float64>::Infinity());
        MARLIN_EXPECT(TryParse("nan", parsed) && parsed != parsed);
        MARLIN_EXPECT(!TryParse("", parsed) && !TryParse(".", parsed) && !TryParse("1e", parsed) &&
                      !TryParse("1x", parsed));
        float32 parsed32 = 0;
        MARLIN_EXPECT(TryParse("1.17549435e-38", parsed32) && parsed32 == 1.17549435e-38f);
        MARLIN_EXPECT(TryParse("16777217", parsed32) && parsed32 == 16777216.0f);
        MARLIN_EXPECT(TryParse("0.1", parsed32) && parsed32 == 0.1f);

        int32 mismatches = 0;
        for (int32 i = 0; i < 20000; ++i)
        {
            const uint64 bits = NextRandom();
            const auto value = BitCast<float64>(bits);
            if (Math::IsNaN(value) || Math::Abs(value) == NumericLimits<float64>::Infinity())
            {
                continue;
            }
            float64 back = 0;
            mismatches += !TryParse(Text(value), back) || BitCast<uint64>(back) != bits;
            const auto bits32 = static_cast<uint32>(bits);
            const auto value32 = BitCast<float32>(bits32);
            if (Math::IsNaN(value32) || Math::Abs(value32) == NumericLimits<float32>::Infinity())
            {
                continue;
            }
            float32 back32 = 0;
            mismatches += !TryParse(Text32(value32), back32) || BitCast<uint32>(back32) != bits32;
        }
        MARLIN_EXPECT(mismatches == 0);

        int32 fixedMismatches = 0;
        for (int32 i = 0; i < 2000; ++i)
        {
            const float64 value = static_cast<float64>(NextRandom() % 100000000) / 1000.0;
            float64 back = 0;
            fixedMismatches += !TryParse(Text(value, FloatStyle::Exponent, 16), back) || back != value;
        }
        MARLIN_EXPECT(fixedMismatches == 0);
    }
}

MARLIN_TEST(Strings, Format)
{

    {
        MARLIN_EXPECT(Format("{0} {1:F2}", "pi", 3.14159) == "pi 3.14");
        MARLIN_EXPECT(Format("{1}{0}{1}", 'a', 'b') == "bab");
        MARLIN_EXPECT(Format("{{{0}}}", 7) == "{7}");
        MARLIN_EXPECT(Format("[{0,5}][{0,-5}]", 42) == "[   42][42   ]");
        MARLIN_EXPECT(Format("{0:X} {1:x4} {2:B}", -1, 255u, uint8{5}) == "FFFFFFFF 00ff 101");
        MARLIN_EXPECT(Format("{0:D5} {1:N}", -42, 1234567.891) == "-00042 1,234,567.89");
        MARLIN_EXPECT(Format("{0:N0} {1:P1}", 1234567, 0.256) == "1,234,567 25.6%");
        MARLIN_EXPECT(Format("{0:E2}", 1234.5678) == "1.23E+003");
        MARLIN_EXPECT(Format("{0:G3}", 0.000012345) == "1.23E-05");
        MARLIN_EXPECT(Format("{0} {1}", true, nullptr) == "true 0x0000000000000000");
        MARLIN_EXPECT(Format("{0} {1}", String("str"), StringView("view")) == "str view");
        MARLIN_EXPECT(Format("{0}", Point{1, -2}) == "(1, -2)");
        MARLIN_EXPECT(Format("{0}", Name("Formatted")) == "Formatted");
        MARLIN_EXPECT(Format("{0}", 1.5f) == "1.5");
        MARLIN_EXPECT(Format("{0:F1}", 7) == "7.0");
        MARLIN_EXPECT(Format(RuntimeFormat("{0} {9} {x} }"), 1) == "1 {9} {x} }");
        StringBuilder<16> builder;
        FormatTo(builder, "{0}-{1}", "abc", 12);
        MARLIN_EXPECT(builder.AsView() == "abc-12");
    }

    {
        char encoded[4] = {};
        MARLIN_EXPECT(Utf8::Encode(0x1F600, encoded) == 4);
        const char *cursor = encoded;
        MARLIN_EXPECT(Utf8::Decode(cursor, encoded + 4) == 0x1F600 && cursor == encoded + 4);
        const StringView mixed = "a\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x98\x80";
        MARLIN_EXPECT(Utf8::IsValid(mixed) && Utf8::CountCodePoints(mixed) == 4);
        char16_t wide[8] = {};
        MARLIN_EXPECT(Utf8::ToUtf16(mixed, wide, 8) == 5 && wide[3] == 0xD83D && wide[4] == 0xDE00);
        MARLIN_EXPECT(Utf8::FromUtf16(wide, 5) == mixed);
        MARLIN_EXPECT(!Utf8::IsValid("\xC0\xAF") && !Utf8::IsValid("\xED\xA0\x80") && !Utf8::IsValid("\xE4\xB8"));
        const char16_t lone[] = {0xD800, u'x'};
        MARLIN_EXPECT(Utf8::FromUtf16(lone, 2) == "\xEF\xBF\xBDx");
        char32_t sum = 0;
        for (const char32_t c : Utf8::CodePoints(mixed))
        {
            sum += c;
        }
        MARLIN_EXPECT(sum == U'a' + 0xE9 + 0x4E2D + 0x1F600);
    }

    {
        const Name a("Transform");
        const Name b(String("Transform"));
        const Name c("transform");
        MARLIN_EXPECT(a == b && a != c && !a.IsNone() && Name().IsNone() && Name("").IsNone());
        MARLIN_EXPECT(a.ToView() == "Transform" && Name::Find("Transform") == a && Name::Find("NeverAdded").IsNone());
        MARLIN_EXPECT(Name::FromIndex(a.GetIndex()) == a);
        Name last;
        for (int32 i = 0; i < 10000; ++i)
        {
            last = Name(Format("Generated{0}", i));
        }
        MARLIN_EXPECT(last.ToView() == "Generated9999" && Name("Generated5000").ToView() == "Generated5000");
        HashMap<Name, int32> byName;
        byName.Add(a, 1);
        MARLIN_EXPECT(byName.Find(b) != nullptr);
    }
}
} // namespace Marlin::Tests
