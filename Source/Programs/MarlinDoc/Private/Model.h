#pragma once

#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "String/String.h"

namespace Marlin::Doc
{
/// A piece of a signature; *Ref* is the id of the item it names, or empty.
struct SignatureToken
{
    String Text;
    String Ref;
};

/// A link target of a doc comment or a page, as written, and what it resolved to: an item id or a `manual:` address.
struct Link
{
    String Target;
    String Resolved;
};

/// One declaration of C++ or of a Language/ library (Documentation/Architecture/16 §4).
struct Item
{
    /// `<language>:<qualified name>`, with the parameter types appended for an overload.
    String Id;
    /// `cpp`, `mscript`, `mshader` or `mcommon`.
    String Language;
    /// `class`, `struct`, `function`, `method`, `field`…: open, never an enum.
    String Kind;
    String Name;
    /// As the language writes it: `Marlin::Engine::GetSubsystems`, `Marlin.Random.Next`.
    String QualifiedName;
    /// The id of the enclosing type or namespace, or empty.
    String Parent;
    /// `public`, `protected` or `internal`.
    String Visibility = "public";
    /// Relative to the engine root, '/'-separated.
    String File;
    uint32 Line = 0;
    uint32 Column = 0;
    /// The declaration as written up to its body, on one line.
    String Signature;
    List<SignatureToken> SignatureTokens;
    /// The parameter types, comma-separated, for functions; what tells overloads apart.
    String Parameters;
    bool HasParameters = false;
    /// Attributes as written, such as `[Compute(64, 1, 1)]`.
    List<String> Attributes;
    /// The reflection marker before a C++ declaration, such as `MPROPERTY`, and its arguments as written.
    String ReflectionMacro;
    String ReflectionArguments;
    /// The Markdown of the `///` comment.
    String Docs;
    List<Link> Links;
    /// The base types and interfaces as written, and the ids they resolved to.
    List<String> Bases;
    List<String> BaseIds;
    /// For an extension, the type it extends as written, and its id.
    String Extends;
    String ExtendsId;
    /// The ids of the members, in source order.
    List<String> Children;
};

/// The `//!` docs of a file.
struct FileDocs
{
    String File;
    String Docs;
    List<Link> Links;
};

/// A C++ module or a Language/ library.
struct Module
{
    String Name;
    /// `Runtime`, `Developer`, `Editor`, `Program` or `Language`.
    String Kind;
    String Directory;
    /// The module's Docs.md.
    String Docs;
    List<Link> Links;
    List<FileDocs> Files;
    List<Item> Items;
};

struct Heading
{
    uint32 Level = 0;
    String Text;
    String Anchor;
};

/// A page of a book, from its Summary.md.
struct Page
{
    /// `<Book>/<path without .md>`, what `manual:` links name.
    String Id;
    String Title;
    /// Relative to the book folder.
    String Path;
    /// The nesting in Summary.md, 0 at the top.
    uint32 Level = 0;
    String Markdown;
    List<Link> Links;
    List<Heading> Headings;
};

/// A folder of Documentation/ with a Summary.md (Documentation/Architecture/16 §2).
struct Book
{
    String Name;
    String Title;
    int32 Order = 0;
    bool HasOrder = false;
    String Directory;
    List<Page> Pages;
};

struct ItemRef
{
    uint32 Module = 0;
    uint32 Item = 0;
};

struct Project
{
    String Root;
    List<Module> Modules;
    List<Book> Books;
    /// Every item by id; the first module's wins when two declare the same id, such as a namespace.
    HashMap<String, ItemRef> Items;
    /// Derived types and interface implementations by base id, and extensions by extended type id.
    HashMap<String, List<String>> Derived;
    HashMap<String, List<String>> Extensions;

    [[nodiscard]] const Item *Find(StringView id) const
    {
        const ItemRef *found = Items.Find(id);
        return found != nullptr ? &Modules[found->Module].Items[found->Item] : nullptr;
    }
};

/// Counts and prints what MarlinDoc reports: `file(line): warning code: message`.
class Reporter
{
public:
    void Warn(StringView file, uint32 line, StringView code, StringView message);
    void Error(StringView message);

    [[nodiscard]] uint32 GetWarnings() const
    {
        return _warnings;
    }

    [[nodiscard]] uint32 GetErrors() const
    {
        return _errors;
    }

private:
    uint32 _warnings = 0;
    uint32 _errors = 0;
};
} // namespace Marlin::Doc
