#include "DataWriter.h"

#include "Json/JsonWriter.h"
#include "String/Format.h"
#include "Text.h"

namespace Marlin::Doc
{
namespace
{
void WriteLinks(JsonWriter &json, const List<Link> &links)
{
    json.Key("links").BeginObject();
    for (const Link &link : links)
    {
        json.Key(link.Target).String(link.Resolved);
    }
    json.EndObject();
}

void WriteStrings(JsonWriter &json, StringView key, const List<String> &values)
{
    json.Key(key).BeginArray();
    for (const String &value : values)
    {
        json.String(value);
    }
    json.EndArray();
}

void WriteItem(JsonWriter &json, const Item &item)
{
    json.Key(item.Id).BeginObject();
    json.Key("language").String(item.Language);
    json.Key("kind").String(item.Kind);
    json.Key("name").String(item.Name);
    json.Key("qualified_name").String(item.QualifiedName);
    json.Key("parent");
    item.Parent.IsEmpty() ? json.Null() : json.String(item.Parent);
    json.Key("visibility").String(item.Visibility);
    json.Key("span").BeginObject();
    json.Key("file").String(item.File).Key("line").Unsigned(item.Line).Key("column").Unsigned(item.Column);
    json.EndObject();
    json.Key("signature").BeginArray();
    for (const SignatureToken &token : item.SignatureTokens)
    {
        json.BeginObject().Key("text").String(token.Text);
        if (!token.Ref.IsEmpty())
        {
            json.Key("ref").String(token.Ref);
        }
        json.EndObject();
    }
    json.EndArray();
    if (item.HasParameters)
    {
        json.Key("parameters").String(item.Parameters);
    }
    WriteStrings(json, "attributes", item.Attributes);
    json.Key("reflection");
    if (item.ReflectionMacro.IsEmpty())
    {
        json.Null();
    }
    else
    {
        json.BeginObject().Key("macro").String(item.ReflectionMacro).Key("arguments").String(item.ReflectionArguments);
        json.EndObject();
    }
    json.Key("bases").BeginArray();
    for (usize i = 0; i < item.Bases.Count(); ++i)
    {
        json.BeginObject().Key("name").String(item.Bases[i]);
        if (i < item.BaseIds.Count() && !item.BaseIds[i].IsEmpty())
        {
            json.Key("ref").String(item.BaseIds[i]);
        }
        json.EndObject();
    }
    json.EndArray();
    if (!item.Extends.IsEmpty())
    {
        json.Key("extends").BeginObject().Key("name").String(item.Extends);
        if (!item.ExtendsId.IsEmpty())
        {
            json.Key("ref").String(item.ExtendsId);
        }
        json.EndObject();
    }
    WriteStrings(json, "children", item.Children);
    json.Key("projections").BeginArray().EndArray();
    json.Key("docs").String(item.Docs);
    WriteLinks(json, item.Links);
    json.EndObject();
}

String ModuleJson(const Module &module)
{
    StringBuilder<65536> output;
    JsonWriter json(output);
    json.BeginObject();
    json.Key("format_version").Integer(DataFormatVersion);
    json.Key("module").String(module.Name);
    json.Key("kind").String(module.Kind);
    json.Key("directory").String(module.Directory);
    json.Key("docs").String(module.Docs);
    WriteLinks(json, module.Links);
    json.Key("files").BeginArray();
    for (const FileDocs &file : module.Files)
    {
        json.BeginObject().Key("file").String(file.File).Key("docs").String(file.Docs);
        WriteLinks(json, file.Links);
        json.EndObject();
    }
    json.EndArray();
    json.Key("items").BeginObject();
    for (const Item &item : module.Items)
    {
        WriteItem(json, item);
    }
    json.EndObject();
    json.EndObject();
    output.Append('\n');
    return output.ToString();
}

String BookJson(const Book &book)
{
    StringBuilder<65536> output;
    JsonWriter json(output);
    json.BeginObject();
    json.Key("format_version").Integer(DataFormatVersion);
    json.Key("book").String(book.Name);
    json.Key("title").String(book.Title);
    json.Key("directory").String(book.Directory);
    json.Key("pages").BeginArray();
    for (const Page &page : book.Pages)
    {
        json.BeginObject();
        json.Key("id").String(page.Id).Key("title").String(page.Title).Key("path").String(page.Path);
        json.Key("level").Unsigned(page.Level);
        json.Key("headings").BeginArray();
        for (const Heading &heading : page.Headings)
        {
            json.BeginObject().Key("level").Unsigned(heading.Level).Key("text").String(heading.Text);
            json.Key("anchor").String(heading.Anchor).EndObject();
        }
        json.EndArray();
        json.Key("markdown").String(page.Markdown);
        WriteLinks(json, page.Links);
        json.EndObject();
    }
    json.EndArray();
    json.EndObject();
    output.Append('\n');
    return output.ToString();
}

void WriteRelations(JsonWriter &json, StringView key, const HashMap<String, List<String>> &relations)
{
    List<const KeyValuePair<String, List<String>> *> sorted;
    for (const auto &entry : relations)
    {
        sorted.Add(&entry);
    }
    sorted.Sort([](const auto *a, const auto *b) { return a->Key.AsView() < b->Key.AsView(); });
    json.Key(key).BeginObject();
    for (const auto *entry : sorted)
    {
        WriteStrings(json, entry->Key, entry->Value);
    }
    json.EndObject();
}

String IndexJson(const Project &project)
{
    StringBuilder<65536> output;
    JsonWriter json(output);
    json.BeginObject();
    json.Key("format_version").Integer(DataFormatVersion);
    json.Key("books").BeginArray();
    for (const Book &book : project.Books)
    {
        json.BeginObject().Key("book").String(book.Name).Key("title").String(book.Title);
        json.Key("file").String(Format("Books/{0}.json", book.Name)).EndObject();
    }
    json.EndArray();
    json.Key("modules").BeginArray();
    for (const Module &module : project.Modules)
    {
        json.BeginObject().Key("module").String(module.Name).Key("kind").String(module.Kind);
        json.Key("file").String(Format("Modules/{0}.json", module.Name));
        json.Key("summary").String(FirstParagraph(module.Docs)).EndObject();
    }
    json.EndArray();
    json.Key("items").BeginObject();
    for (const Module &module : project.Modules)
    {
        for (const Item &item : module.Items)
        {
            const ItemRef *owner = project.Items.Find(item.Id);
            if (owner == nullptr || &project.Modules[owner->Module] != &module)
            {
                continue;
            }
            json.Key(item.Id).BeginObject();
            json.Key("name").String(item.Name).Key("kind").String(item.Kind).Key("module").String(module.Name);
            json.Key("summary").String(FirstParagraph(item.Docs)).EndObject();
        }
    }
    json.EndObject();
    WriteRelations(json, "derived", project.Derived);
    WriteRelations(json, "extensions", project.Extensions);
    json.EndObject();
    output.Append('\n');
    return output.ToString();
}
} // namespace

bool WriteData(const Project &project, StringView folder, Reporter &reporter)
{
    bool ok = true;
    auto write = [&](const String &path, const String &text) {
        if (!WriteFile(path, text))
        {
            reporter.Error(Format("{0} cannot be written", path));
            ok = false;
        }
    };
    for (const Module &module : project.Modules)
    {
        write(Format("{0}/Modules/{1}.json", folder, module.Name), ModuleJson(module));
    }
    for (const Book &book : project.Books)
    {
        write(Format("{0}/Books/{1}.json", folder, book.Name), BookJson(book));
    }
    write(Format("{0}/index.json", folder), IndexJson(project));
    return ok;
}
} // namespace Marlin::Doc
