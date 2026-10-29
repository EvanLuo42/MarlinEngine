#include "Testing/Testing.h"

#include "Json/JsonSerializer.h"
#include "Json/JsonValue.h"
#include "Json/JsonWriter.h"
#include "JsonTestTypes.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/NumericLimits.h"

namespace Marlin::Tests
{
namespace
{
JsonValue ParseOrFail(StringView text)
{
    JsonValue value;
    JsonError error;
    MARLIN_REQUIRE(Json::Parse(text, value, error));
    return value;
}
} // namespace

MARLIN_TEST(Json, ParseAndWrite)
{
    const JsonValue value = ParseOrFail(R"( {"a": [1, -2, 3.5, 1e3, 18446744073709551615, -9223372036854775808],
        "b": {"s": "x\"\\\/\n\u00e9\ud83d\ude00", "t": true, "f": false, "n": null}} )");
    MARLIN_REQUIRE(value.IsObject() && value.Count() == 2);
    const Span<const JsonValue> numbers = value.Find("a")->GetElements();
    int64 integer = 0;
    uint64 big = 0;
    MARLIN_EXPECT(numbers[1].TryGetInteger(integer) && integer == -2);
    MARLIN_EXPECT(!numbers[2].TryGetInteger(integer) && numbers[2].AsNumber() == 3.5);
    MARLIN_EXPECT(numbers[3].TryGetInteger(integer) && integer == 1000);
    MARLIN_EXPECT(numbers[4].TryGetUnsigned(big) && big == 18446744073709551615u);
    MARLIN_EXPECT(numbers[5].TryGetInteger(integer) && integer == NumericLimits<int64>::Min());
    const JsonValue &b = *value.Find("b");
    MARLIN_EXPECT(b.Find("s")->AsString() == "x\"\\/\n\xC3\xA9\xF0\x9F\x98\x80");
    MARLIN_EXPECT(b.Find("t")->AsBool() && !b.Find("f")->AsBool(true) && b.Find("n")->IsNull());

    const String text = value.ToString();
    MARLIN_EXPECT(text.AsView().Contains("18446744073709551615") && text.AsView().Contains("-9223372036854775808"));
    MARLIN_EXPECT(ParseOrFail(text) == value);

    StringBuilder<128> output;
    JsonWriter writer(output);
    writer.BeginObject().Key("k").BeginArray().Integer(1).Null().Number(0.1).Number(1.0 / 0.0).EndArray();
    writer.Key("e").BeginObject().EndObject().Key("c").String("\x01").EndObject();
    MARLIN_EXPECT(output.AsView() == R"({"k":[1,null,0.1,null],"e":{},"c":"\u0001"})");
}

MARLIN_TEST(Json, ParseErrors)
{
    const auto fails = [](StringView text, uint32 line, uint32 column) {
        JsonValue value;
        JsonError error;
        return !Json::Parse(text, value, error) && error.Line == line && error.Column == column;
    };
    MARLIN_EXPECT(fails("", 1, 1));
    MARLIN_EXPECT(fails("[1,]", 1, 4));
    MARLIN_EXPECT(fails("{\"a\" 1}", 1, 6));
    MARLIN_EXPECT(fails("[\n01]", 2, 2));
    MARLIN_EXPECT(fails("\"\\ud800\"", 1, 8));
    MARLIN_EXPECT(fails("tru", 1, 1));
    MARLIN_EXPECT(fails("1 2", 1, 3));
    MARLIN_EXPECT(fails("\"a\tb\"", 1, 4));
}

MARLIN_TEST(Json, Reflection)
{
    JsonTestSettings settings;
    settings.Name = "Main";
    settings.Size = {1920, 1080};
    settings.Mode = JsonTestMode::Borderless;
    settings.Id = 0xFFFFFFFFFFFFFFFFull;
    settings.Tags.Add("hdr");
    settings.Scales.FindOrAdd("ui") = 1.5f;
    settings.ByIndex.FindOrAdd(3) = "three";
    settings.Owner = Guid{1, 2, 3, 4};
    settings.Fallback = 7;
    settings.Cached = 99;

    StringBuilder<1024> text;
    JsonWriter writer(text);
    Json::Write(writer, settings);
    MARLIN_EXPECT(text.AsView().Contains(R"("Size":[1920,1080])"));
    MARLIN_EXPECT(text.AsView().Contains(R"("Mode":"Borderless")"));
    MARLIN_EXPECT(text.AsView().Contains(R"("Id":18446744073709551615)"));
    MARLIN_EXPECT(text.AsView().Contains(R"("Scales":{"ui":1.5})"));
    MARLIN_EXPECT(text.AsView().Contains(R"("ByIndex":[[3,"three"]])"));
    MARLIN_EXPECT(text.AsView().Contains(R"("Owner":"00000001-0000-0002-0000-000300000004")"));
    MARLIN_EXPECT(!text.AsView().Contains("Cached"));

    JsonTestSettings read;
    StringBuilder<256> error;
    MARLIN_REQUIRE(Json::Read(ParseOrFail(text.AsView()), read, error));
    MARLIN_EXPECT(read.Name == "Main" && read.Size.Width == 1920 && read.Mode == JsonTestMode::Borderless);
    MARLIN_EXPECT(read.Id == settings.Id && read.Tags.Count() == 1 && *read.Scales.Find("ui") == 1.5f);
    MARLIN_EXPECT(*read.ByIndex.Find(3) == "three" && read.Owner == settings.Owner);
    MARLIN_EXPECT(read.Fallback.HasValue() && read.Fallback.Value() == 7 && read.Cached == 0);

    JsonTestSettings bad;
    MARLIN_EXPECT(!Json::Read(ParseOrFail(R"({"Size":[1,"x"]})"), bad, error));
    MARLIN_EXPECT(error.AsView().EndsWith("Size[1]: expected an int32"));
    error.Clear();
    MARLIN_EXPECT(!Json::Read(ParseOrFail(R"({"Mode":"Tiny"})"), bad, error));
    MARLIN_EXPECT(error.AsView().Contains("'Tiny' is not a JsonTestMode"));
    error.Clear();
    MARLIN_EXPECT(!Json::Read(ParseOrFail(R"({"Width":1})"), bad, error));
    MARLIN_EXPECT(error.AsView().Contains("has no field Width"));
}

MARLIN_TEST(Json, Schema)
{
    StringBuilder<4096> text;
    JsonWriter writer(text);
    JsonSchemaWriter::WriteDocument(writer, TypeOf<JsonTestSettings>());
    const JsonValue schema = ParseOrFail(text.AsView());
    const JsonValue *definitions = schema.Find("$defs");
    MARLIN_REQUIRE(definitions != nullptr);
    const JsonValue *settings = definitions->Find("Marlin::Tests::JsonTestSettings");
    MARLIN_REQUIRE(settings != nullptr);
    const JsonValue *properties = settings->Find("properties");
    MARLIN_REQUIRE(properties != nullptr && properties->Find("Cached") == nullptr);
    MARLIN_EXPECT(properties->Find("Tags")->Find("type")->AsString() == "array");
    MARLIN_EXPECT(properties->Find("Name")->Find("description")->AsString() == "The display name.");
    const JsonValue *mode = definitions->Find("Marlin::Tests::JsonTestMode");
    MARLIN_REQUIRE(mode != nullptr && mode->Find("enum")->Count() == 3);
    MARLIN_EXPECT(definitions->Find("Marlin::Tests::JsonTestSize")->Find("type")->AsString() == "array");
}

MARLIN_TEST(Json, RepeatedKeysReplaceInPlace)
{
    // A repeated key keeps its first place and takes the last value, in small objects and in large ones.
    StringBuilder<1024> text;
    text.Append("{\"a\": 1, \"b\": 2, \"a\": 3");
    for (uint32 i = 0; i < 40; ++i)
    {
        FormatTo(text, ", \"k{0}\": {1}", i % 30, i);
    }
    text.Append(", \"b\": 4}");
    const JsonValue value = ParseOrFail(text.AsView());
    MARLIN_REQUIRE(value.IsObject() && value.Count() == 32);
    const Span<const JsonMember> members = value.GetMembers();
    MARLIN_EXPECT(members[0].Key == "a" && members[0].Value.AsNumber() == 3);
    MARLIN_EXPECT(members[1].Key == "b" && members[1].Value.AsNumber() == 4);
    for (uint32 i = 0; i < 30; ++i)
    {
        const JsonValue *found = value.Find(Format("k{0}", i).AsView());
        MARLIN_REQUIRE(found != nullptr);
        MARLIN_EXPECT(members[2 + i].Value.AsNumber() == found->AsNumber());
        MARLIN_EXPECT(found->AsNumber() == (i < 10 ? i + 30 : i));
    }
}
} // namespace Marlin::Tests
