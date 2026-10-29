#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Marlin::HeaderTool
{
struct MetadataItem
{
    std::string Key;
    std::string Value;
};

struct FieldDecl
{
    std::string Name;
    std::vector<MetadataItem> Metadata;
    bool IsStatic = false;
    int Line = 0;
};

struct ParameterDecl
{
    std::string Name;
    std::string DefaultValue;
};

struct FunctionDecl
{
    std::string Name;
    std::vector<ParameterDecl> Parameters;
    std::vector<MetadataItem> Metadata;
    int Line = 0;
};

enum class RecordKind : std::uint8_t
{
    Struct,
    Class,
    Interface,
};

struct RecordDecl
{
    RecordKind Kind = RecordKind::Struct;
    /// `struct` or `class`, which sets the access the generated body restores.
    bool IsClassKey = false;
    std::string Name;
    std::vector<std::string> Namespaces;
    /// The API macro written before the name, if any.
    std::string Api;
    std::vector<std::string> Bases;
    std::vector<MetadataItem> Metadata;
    std::vector<FieldDecl> Fields;
    std::vector<FunctionDecl> Functions;
    int Line = 0;
    int BodyLine = 0;
};

struct EnumeratorDecl
{
    std::string Name;
    std::vector<MetadataItem> Metadata;
};

struct EnumDecl
{
    std::string Name;
    std::vector<std::string> Namespaces;
    bool IsScoped = false;
    std::string Underlying;
    std::vector<EnumeratorDecl> Enumerators;
    std::vector<MetadataItem> Metadata;
    int Line = 0;
};

/// MARLIN_UPGRADE(Type, From, To) in a source file.
struct UpgradeDecl
{
    std::string Type;
    int From = 0;
    int To = 0;
    std::vector<std::string> Namespaces;
    int Line = 0;
};

/// MARLIN_TEST(Suite, Name, Tags...) or MARLIN_TEST_F(Fixture, Suite, Name, Tags...) in a source file.
struct TestDecl
{
    /// MARLIN_TEST_SOURCE(Name): a function that registers tests at run time; Suite is empty.
    bool Source = false;
    std::string Suite;
    std::string Name;
    std::vector<std::string> Tags;
    std::vector<std::string> Namespaces;
    std::string File;
    int Line = 0;
};

struct ParsedFile
{
    std::vector<RecordDecl> Records;
    std::vector<EnumDecl> Enums;
    std::vector<UpgradeDecl> Upgrades;
    std::vector<TestDecl> Tests;
    /// The quoted includes in order, with their lines.
    std::vector<std::pair<std::string, int>> Includes;

    [[nodiscard]] bool IsReflected() const
    {
        return !Records.empty() || !Enums.empty();
    }
};

struct Diagnostic
{
    std::string File;
    int Line = 0;
    std::string Message;
};
} // namespace Marlin::HeaderTool
