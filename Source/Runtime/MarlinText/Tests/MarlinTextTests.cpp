#include "Testing/Testing.h"

#include "MarlinText/MarlinTextDocument.h"
#include "MarlinText/MarlinTextSchema.h"
#include "MarlinText/MarlinTextSerializer.h"
#include "MarlinTextTestTypes.h"
#include "Reflection/Reflection.h"
#include "String/StringBuilder.h"

namespace Marlin::Tests
{
MARLIN_UPGRADE(TextNode, 1, 2)
{
    object.Rename("OldShape", "Shape");
}

namespace
{
constexpr StringView Sample = R"(!Version "1.0"

// A comment that must survive edits.
TextNode
{
    !Schema = 1
    OldShape = Round   // old name
    Maybe = 5
    Weights = { a = 1, "two words" = 2 }
    Child = TextLeaf { Depth = 3, Maybe = 9 }
    Points += [(1, [2, 3])]
}

TextNode : <Base#01832501-7918-3ee4-b177-2f200aab91b3>
{
    !Schema = 2
    Maybe = 6
    @if Platform == Windows
    {
        Weights = { win = 1 }
    }
    @else
    {
        Weights = { other = 1 }
    }
}
)";

void FillNode(TextNode &node)
{
    node.Maybe = 3;
    node.Shape = TextShape::Round;
    node.Weights.Add(Name("heavy weight"), 2.5f);
    node.Weights.Add(Name("b"), -1.0f);
    auto leaf = MakeUnique<TextLeaf>();
    leaf->Depth = 11;
    node.Child = Move(leaf);
    TextPoint point;
    point.X = 1.25f;
    point.Y[0] = 2;
    point.Y[1] = 3;
    node.Points.Add(point);
    node.Label = "say \"hi\"\n";
}

struct ParsedSample
{
    MarlinTextDocument Document;
    MarlinTextDiagnostics Diagnostics;

    ParsedSample()
    {
        MarlinTextParseOptions options;
        options.AllowConditions = true;
        MARLIN_REQUIRE(Document.Parse(Sample, Diagnostics, options));
    }
};
} // namespace

MARLIN_TEST(Serializer, RoundTrip)
{
    TextNode source;
    FillNode(source);
    StringBuilder<1024> text;
    const Guid uuid = Guid::NewGuid();
    MarlinTextSerializer::WriteObject(text, TypeOf<TextNode>(), &source, uuid, "Thing");
    MARLIN_EXPECT(text.AsView().Contains("Child = TextLeaf {") && text.AsView().Contains("!Schema = 2"));
    MARLIN_EXPECT(text.AsView().Contains("\"heavy weight\" = 2.5") &&
                  text.AsView().Contains("Points = [(1.25, [2, 3])]"));

    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    MARLIN_REQUIRE(document.Parse(text.AsView(), diagnostics) && document.GetObjectCount() == 1);
    const MarlinTextObject *object = document.GetObject(0, diagnostics);
    MARLIN_REQUIRE(object != nullptr);
    MARLIN_EXPECT(object->GetName() == "Thing" && object->GetUuid() == uuid && object->GetSchema() == 2);
    TextNode loaded;
    MARLIN_EXPECT(MarlinTextSerializer::ReadObject(*object, TypeOf<TextNode>(), &loaded, diagnostics));
    MARLIN_EXPECTF(!diagnostics.HasErrors(), "{0}", diagnostics.ToString());
    MARLIN_EXPECT(Reflection::Equals(TypeOf<TextNode>(), &loaded, &source));
    MARLIN_EXPECT(loaded.Label == source.Label && &loaded.Child->GetType() == &TypeOf<TextLeaf>());
}

MARLIN_TEST(Serializer, CoreValueTypes)
{
    TextPlacement placed;
    placed.Where.Translation = Vector3(1, 2.5f, -3);
    placed.Where.Scale = Vector3(2);
    placed.Id = Guid::NewGuid();
    placed.When = DateTime(2024, 7, 4, 13, 5, 9, 250);
    placed.Tint = Color32{10, 20, 30, 40};
    placed.Cache = 99;
    StringBuilder<1024> text;
    MarlinTextSerializer::WriteObject(text, TypeOf<TextPlacement>(), &placed);
    MARLIN_EXPECT(text.AsView().Contains("Translation = (1, 2.5, -3)") &&
                  text.AsView().Contains("Tint = (10, 20, 30, 40)"));
    MARLIN_EXPECT(!text.AsView().Contains("Cache") && !text.AsView().Contains("Rotation"));

    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    MARLIN_REQUIRE(document.Parse(text.AsView(), diagnostics));
    TextPlacement loaded;
    MARLIN_EXPECT(MarlinTextSerializer::ReadObject(*document.GetObject(0, diagnostics), TypeOf<TextPlacement>(),
                                                   &loaded, diagnostics));
    MARLIN_EXPECT(loaded.Where.Translation == placed.Where.Translation && loaded.Where.Scale == placed.Where.Scale);
    MARLIN_EXPECT(loaded.Id == placed.Id && loaded.When == placed.When && loaded.Cache == 0);
    MARLIN_EXPECT(loaded.Tint.R == 10 && loaded.Tint.A == 40);
}

MARLIN_TEST(Serializer, UpgradesAndArrayOperations)
{
    ParsedSample sample;
    MARLIN_REQUIRE(sample.Document.GetVersion() == "1.0" && sample.Document.GetObjectCount() == 2);
    const MarlinTextObject *first = sample.Document.GetObject(0, sample.Diagnostics);
    MARLIN_REQUIRE(first != nullptr);
    TextNode loaded;
    MARLIN_EXPECT(MarlinTextSerializer::ReadObject(*first, TypeOf<TextNode>(), &loaded, sample.Diagnostics));
    MARLIN_EXPECT(loaded.Shape == TextShape::Round && loaded.Maybe.ValueOr(0) == 5 && loaded.Points.Count() == 1);
    MARLIN_EXPECT(loaded.Weights.Find(Name("two words")) != nullptr);
    MARLIN_REQUIRE(loaded.Child);
    MARLIN_EXPECT(&loaded.Child->GetType() == &TypeOf<TextLeaf>() &&
                  static_cast<TextLeaf *>(loaded.Child.Get())->Depth == 3 && loaded.Child->Maybe.ValueOr(0) == 9);
    MARLIN_EXPECT(!sample.Diagnostics.HasErrors());
}

MARLIN_TEST(Serializer, OverridesAndConditions)
{
    ParsedSample sample;
    const MarlinTextConditions::Variable variables[] = {{"Platform", "Windows"}};
    const MarlinTextConditions conditions(variables);
    MarlinTextReadOptions options;
    options.Conditions = &conditions;
    options.ResolveParent = [&](const MarlinTextValue &link) -> const MarlinTextObject * {
        MarlinTextDiagnostics ignored;
        return link.LinkName == "Base" && link.LinkGuid.IsValid() ? sample.Document.GetObject(0, ignored) : nullptr;
    };
    const MarlinTextObject *second = sample.Document.GetObject(1, sample.Diagnostics);
    MARLIN_REQUIRE(second != nullptr);
    TextNode overridden;
    MARLIN_EXPECT(
        MarlinTextSerializer::ReadObject(*second, TypeOf<TextNode>(), &overridden, sample.Diagnostics, options));
    MARLIN_EXPECT(overridden.Maybe.ValueOr(0) == 6 && overridden.Shape == TextShape::Round);
    MARLIN_EXPECT(overridden.Weights.Count() == 1 && overridden.Weights.Find(Name("win")) != nullptr);

    TextNode unresolved;
    MarlinTextDiagnostics errors;
    MARLIN_EXPECT(!MarlinTextSerializer::ReadObject(*second, TypeOf<TextNode>(), &unresolved, errors));
    MARLIN_EXPECT(errors.HasErrors());
}

MARLIN_TEST(Serializer, UpdateKeepsTheRestOfTheText)
{
    ParsedSample sample;
    TextNode loaded;
    MARLIN_REQUIRE(MarlinTextSerializer::ReadObject(*sample.Document.GetObject(0, sample.Diagnostics),
                                                    TypeOf<TextNode>(), &loaded, sample.Diagnostics));
    loaded.Maybe = 8;
    MARLIN_REQUIRE(
        MarlinTextSerializer::UpdateObject(sample.Document, 0, TypeOf<TextNode>(), &loaded, sample.Diagnostics));
    const StringView text = sample.Document.GetText();
    MARLIN_EXPECT(text.Contains("// A comment that must survive edits.") && text.Contains("Maybe = 8"));
    MARLIN_EXPECT(text.Contains("Shape = Round") && !text.Contains("OldShape") && !text.Contains("// old name"));
    MARLIN_EXPECT(text.Contains("!Schema = 2\n    Maybe = 8") && text.Contains("Points += [(1, [2, 3])]"));

    TextNode reloaded;
    MARLIN_EXPECT(MarlinTextSerializer::ReadObject(*sample.Document.GetObject(0, sample.Diagnostics),
                                                   TypeOf<TextNode>(), &reloaded, sample.Diagnostics));
    MARLIN_EXPECT(Reflection::Equals(TypeOf<TextNode>(), &reloaded, &loaded));

    reloaded.Points.Clear();
    MARLIN_EXPECT(
        MarlinTextSerializer::UpdateObject(sample.Document, 0, TypeOf<TextNode>(), &reloaded, sample.Diagnostics));
    MARLIN_EXPECT(sample.Document.GetText().Contains("Points = []") &&
                  !sample.Document.GetText().Contains("Points +="));
    MARLIN_EXPECT(sample.Document.GetText().Contains("@else"));
}

MARLIN_TEST(Document, Edits)
{
    ParsedSample sample;
    MARLIN_EXPECT(sample.Document.SetField(1, "Label", "\"x\"", sample.Diagnostics));
    MARLIN_EXPECT(sample.Document.GetText().Contains("    Maybe = 6\n") &&
                  sample.Document.GetText().Contains("Label = \"x\""));
    MARLIN_EXPECT(!sample.Document.SetField(1, "Label", "not a value {", sample.Diagnostics));
    MARLIN_EXPECT(sample.Document.RemoveField(0, "Child", sample.Diagnostics));
    MARLIN_EXPECT(!sample.Document.GetText().Contains("TextLeaf"));
    MARLIN_EXPECT(sample.Document.AddObject("TextLeaf { Depth = 1 }", sample.Diagnostics));
    MARLIN_EXPECT(sample.Document.GetObjectCount() == 3);
    MARLIN_EXPECT(sample.Document.RemoveObject(1, sample.Diagnostics) && sample.Document.GetObjectCount() == 2);
    MARLIN_EXPECT(!sample.Document.GetText().Contains("@if"));
    const MarlinTextObject *leaf = sample.Document.GetObject(1, sample.Diagnostics);
    MARLIN_EXPECT(leaf != nullptr && leaf->GetTypeName() == "TextLeaf");
}

MARLIN_TEST(Document, Values)
{
    MarlinTextDocument document;
    MarlinTextDiagnostics diagnostics;
    MARLIN_REQUIRE(document.Parse(R"(Values {
        Hex = 0x1F, Negative = -3, Float = 1.5e3, Infinity = -inf
        Text = "tab\tquote\" é"
        Flags = A | B
        Link = </Project/Rock:Rock#01832501-7918-3ee4-b177-2f200aab91b3>
        Binary = `source:Rock.png#01832501-7918-3ee4-b177-2f200aab91b3`
        Nothing = null
    })",
                                  diagnostics));
    const MarlinTextObject *object = document.GetObject(0, diagnostics);
    MARLIN_REQUIREF(object != nullptr, "{0}", diagnostics.ToString());
    MARLIN_EXPECT(object->Find("Hex")->Value.Integer == 31 && object->Find("Negative")->Value.Integer == -3);
    MARLIN_EXPECT(object->Find("Float")->Value.Float == 1500 && object->Find("Infinity")->Value.Float < -1e300);
    MARLIN_EXPECT(object->Find("Text")->Value.Text == "tab\tquote\" \xC3\xA9");
    MARLIN_EXPECT(object->Find("Flags")->Value.Kind == MarlinTextValueKind::Identifier &&
                  object->Find("Flags")->Value.Text == "A | B");
    const MarlinTextValue &link = object->Find("Link")->Value;
    MARLIN_EXPECT(link.Kind == MarlinTextValueKind::ObjectLink && link.Text == "/Project/Rock" &&
                  link.LinkName == "Rock");
    MARLIN_EXPECT(object->Find("Binary")->Value.Text == "source:Rock.png" &&
                  object->Find("Binary")->Value.LinkGuid.IsValid());
    MARLIN_EXPECT(object->Find("Nothing")->Value.Kind == MarlinTextValueKind::Null);
}

MARLIN_TEST(Document, Errors)
{
    MarlinTextDiagnostics errors;
    MarlinTextDocument document;
    MARLIN_EXPECT(!document.Parse("TextNode { @if A == B { } }", errors) || document.GetObject(0, errors) == nullptr);
    MARLIN_EXPECT(errors.HasErrors() && errors.ToString("bad.mtext").Contains("bad.mtext(1): error: @if"));

    MarlinTextDiagnostics more;
    MARLIN_REQUIRE(document.Parse("TextNode { Maybe = \"five\" Unknown = 1 }\nTextNode { !Schema = 9 }", more));
    TextNode node;
    MARLIN_EXPECT(!MarlinTextSerializer::ReadObject(*document.GetObject(0, more), TypeOf<TextNode>(), &node, more));
    MARLIN_EXPECT(more.GetAll().Count() == 2 && !more.GetAll()[1].IsError);
    MARLIN_EXPECT(!MarlinTextSerializer::ReadObject(*document.GetObject(1, more), TypeOf<TextNode>(), &node, more));
    MARLIN_EXPECT(!document.Parse("TextNode { Maybe = 1", more));
}
} // namespace Marlin::Tests
