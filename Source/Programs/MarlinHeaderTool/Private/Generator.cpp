#include "Generator.h"

#include <string>
#include <string_view>
#include <vector>

namespace Marlin::HeaderTool
{
namespace
{
std::string Quote(const std::string &text)
{
    std::string result = "\"";
    for (const char c : text)
    {
        switch (c)
        {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\t':
            result += "\\t";
            break;
        case '\r':
            result += "\\r";
            break;
        default:
            result += c;
            break;
        }
    }
    return result + "\"";
}

std::string Qualified(const std::vector<std::string> &namespaces, const std::string &name)
{
    std::string result;
    for (const std::string &part : namespaces)
    {
        result += "::" + part;
    }
    return result + "::" + name;
}

std::string FullName(const std::vector<std::string> &namespaces, const std::string &name)
{
    std::string result;
    for (const std::string &part : namespaces)
    {
        result += part + "::";
    }
    return result + name;
}

std::string JoinNamespaces(const std::vector<std::string> &namespaces)
{
    std::string result;
    for (const std::string &part : namespaces)
    {
        result += (result.empty() ? "" : "::") + part;
    }
    return result;
}

const std::string *FindMetadata(const std::vector<MetadataItem> &items, const std::string &key)
{
    for (const MetadataItem &item : items)
    {
        if (item.Key == key)
        {
            return &item.Value;
        }
    }
    return nullptr;
}

/// Declares the entries of *items* as *name* and returns the Metadata expression for them.
std::string MetadataOf(std::string &out, const std::string &name, const std::vector<MetadataItem> &items)
{
    if (items.empty())
    {
        return "Metadata()";
    }
    out += "    static constexpr MetadataEntry " + name + "[] = {";
    for (size_t i = 0; i < items.size(); ++i)
    {
        out += (i == 0 ? "" : ", ") + std::string("{") + Quote(items[i].Key) + ", " + Quote(items[i].Value) + "}";
    }
    out += "};\n";
    return "Metadata(" + name + ")";
}

std::string_view KindName(RecordKind kind)
{
    switch (kind)
    {
    case RecordKind::Class:
        return "Class";
    case RecordKind::Interface:
        return "Interface";
    default:
        return "Struct";
    }
}

void OpenNamespace(std::string &out, const std::vector<std::string> &namespaces)
{
    if (!namespaces.empty())
    {
        out += "namespace " + JoinNamespaces(namespaces) + "\n{\n";
    }
}

void CloseNamespace(std::string &out, const std::vector<std::string> &namespaces)
{
    if (!namespaces.empty())
    {
        out += "} // namespace " + JoinNamespaces(namespaces) + "\n";
    }
}

void GenerateRecord(std::string &out, const ModuleInfo &module, const RecordDecl &record)
{
    const std::string type = Qualified(record.Namespaces, record.Name);
    out += "template <> struct TypeRegistration<" + type + ">\n{\n";
    out += "    using T = " + type + ";\n";
    if (record.Kind == RecordKind::Class)
    {
        out += "    static_assert(__has_virtual_destructor(T), \"MCLASS " + record.Name +
               " needs a virtual destructor\");\n";
    }
    const std::string typeMeta = MetadataOf(out, "TypeMeta", record.Metadata);

    if (!record.Fields.empty())
    {
        std::vector<std::string> metas;
        metas.reserve(record.Fields.size());
        for (const FieldDecl &field : record.Fields)
        {
            metas.push_back(MetadataOf(out, "FieldMeta_" + field.Name, field.Metadata));
        }
        out += "    static constinit inline FieldInfo Fields[] = {\n";
        for (size_t i = 0; i < record.Fields.size(); ++i)
        {
            const FieldDecl &field = record.Fields[i];
            const std::string getter = "&TypeOf<decltype(T::" + field.Name + ")>";
            const std::string location = field.IsStatic
                                             ? "const_cast<void *>(static_cast<const void *>(&T::" + field.Name + "))"
                                             : "static_cast<uint32>(__builtin_offsetof(T, " + field.Name + "))";
            out += "        FieldInfo(";
            out += Quote(field.Name);
            out += ", " + getter;
            out += ", " + location;
            out += ", " + metas[i];
            out += "),\n";
        }
        out += "    };\n";
    }

    if (!record.Functions.empty())
    {
        std::vector<std::string> metas;
        for (const FunctionDecl &function : record.Functions)
        {
            const std::string pointer = "&T::" + function.Name;
            metas.push_back(MetadataOf(out, "FunctionMeta_" + function.Name, function.Metadata));
            out += "    static_assert(FunctionTraitsOf<" + pointer +
                   ">::ArgumentCount == " + std::to_string(function.Parameters.size()) + ", \"MarlinHeaderTool read " +
                   std::to_string(function.Parameters.size()) + " parameters of " + record.Name + "::" + function.Name +
                   "\");\n";
            if (!function.Parameters.empty())
            {
                out += "    static constexpr ParameterInfo Parameters_" + function.Name + "[] = {";
                for (size_t i = 0; i < function.Parameters.size(); ++i)
                {
                    const ParameterDecl &parameter = function.Parameters[i];
                    out += (i == 0 ? "" : ", ") + std::string("ParameterOf<") + pointer + ", " + std::to_string(i) +
                           ">(" + Quote(parameter.Name) + ", " + Quote(parameter.DefaultValue) + ")";
                }
                out += "};\n";
            }
        }
        out += "    static constinit inline FunctionInfo Functions[] = {\n";
        for (size_t i = 0; i < record.Functions.size(); ++i)
        {
            const FunctionDecl &function = record.Functions[i];
            const std::string pointer = "&T::" + function.Name;
            const std::string parameters = function.Parameters.empty()
                                               ? "Span<const ParameterInfo>()"
                                               : "Span<const ParameterInfo>(Parameters_" + function.Name + ")";
            out += "        FunctionInfo(";
            out += Quote(function.Name);
            out += ", ReturnTypeOf<" + pointer + ">(), ";
            out += parameters;
            out += ", &Invoker<" + pointer + ">::Invoke";
            out += ", FunctionTraitsOf<" + pointer + ">::IsStatic";
            out += ", FunctionTraitsOf<" + pointer + ">::IsConst, ";
            out += metas[i];
            out += "),\n";
        }
        out += "    };\n";
    }

    if (!record.Bases.empty())
    {
        out += "    static constexpr InterfaceEntry Interfaces[] = {";
        for (size_t i = 0; i < record.Bases.size(); ++i)
        {
            out += (i == 0 ? "" : ", ") + std::string("InterfaceOf<T, T::MarlinBase") + std::to_string(i) + ">()";
        }
        out += "};\n";
    }

    const std::string *version = FindMetadata(record.Metadata, "Version");
    out += "    static constinit inline TypeInfo Type{TypeDescription{\n";
    out += "        .Name = " + Quote(record.Name) + ",\n";
    out += "        .FullName = " + Quote(FullName(record.Namespaces, record.Name)) + ",\n";
    out += "        .Module = " + Quote(module.Name) + ",\n";
    out += "        .Kind = TypeKind::";
    out += KindName(record.Kind);
    out += ",\n";
    out += "        .Size = sizeof(T),\n";
    out += "        .Alignment = alignof(T),\n";
    out += "        .Version = " + (version != nullptr ? *version : std::string("1")) + ",\n";
    out += "        .Abstract = IsAbstract<T>,\n";
    out += "        .Operations = &OperationsFor<T>::Value,\n";
    if (!record.Bases.empty())
    {
        out += "        .Parent = ParentOf<T, T::MarlinBase0>(),\n";
        out += "        .ToParent = ToParentOf<T, T::MarlinBase0>(),\n";
        out += "        .Interfaces = Interfaces,\n";
    }
    if (!record.Fields.empty())
    {
        out += "        .Fields = Fields,\n";
    }
    if (!record.Functions.empty())
    {
        out += "        .Functions = Functions,\n";
    }
    if (!record.Metadata.empty())
    {
        out += "        .Meta = " + typeMeta + ",\n";
    }
    out += "    }};\n};\n\n";
}

void GenerateEnum(std::string &out, const ModuleInfo &module, const EnumDecl &decl)
{
    const std::string type = Qualified(decl.Namespaces, decl.Name);
    out += "template <> struct TypeRegistration<" + type + ">\n{\n";
    out += "    using T = " + type + ";\n";
    const std::string typeMeta = MetadataOf(out, "TypeMeta", decl.Metadata);
    std::vector<std::string> metas;
    metas.reserve(decl.Enumerators.size());
    for (const EnumeratorDecl &enumerator : decl.Enumerators)
    {
        metas.push_back(MetadataOf(out, "EnumeratorMeta_" + enumerator.Name, enumerator.Metadata));
    }
    if (!decl.Enumerators.empty())
    {
        out += "    static constexpr EnumeratorInfo Enumerators[] = {\n";
        for (size_t i = 0; i < decl.Enumerators.size(); ++i)
        {
            const std::string &name = decl.Enumerators[i].Name;
            out += "        {" + Quote(name) + ", static_cast<int64>(T::" + name + "), " + metas[i] + "},\n";
        }
        out += "    };\n";
    }
    out += "    static constinit inline TypeInfo Type{TypeDescription{\n";
    out += "        .Name = " + Quote(decl.Name) + ",\n";
    out += "        .FullName = " + Quote(FullName(decl.Namespaces, decl.Name)) + ",\n";
    out += "        .Module = " + Quote(module.Name) + ",\n";
    out += "        .Kind = TypeKind::Enum,\n";
    out += "        .Size = sizeof(T),\n";
    out += "        .Alignment = alignof(T),\n";
    out += "        .Signed = IsSigned<UnderlyingType<T>>,\n";
    out += "        .Operations = &OperationsFor<T>::Value,\n";
    if (!decl.Enumerators.empty())
    {
        out += "        .Enumerators = Enumerators,\n";
    }
    if (!decl.Metadata.empty())
    {
        out += "        .Meta = " + typeMeta + ",\n";
    }
    out += "    }};\n};\n\n";
}

void AppendMacroLine(std::string &out, const std::string &line)
{
    out += " \\\n    " + line;
}
} // namespace

std::string GenerateHeader(const ModuleInfo &module, const GeneratedFile &file)
{
    std::string out = "// Generated by MarlinHeaderTool from " + file.IncludePath + ". Do not edit.\n";
    out += "#pragma once\n\n#include \"Reflection/Markup.h\"\n";

    for (const EnumDecl &decl : file.Parsed.Enums)
    {
        out += '\n';
        OpenNamespace(out, decl.Namespaces);
        out += std::string(decl.IsScoped ? "enum class " : "enum ") + decl.Name +
               (decl.Underlying.empty() ? "" : " : " + decl.Underlying) + ";\n";
        out += module.Api + " const ::Marlin::TypeInfo &MarlinStaticType(" + decl.Name + " *tag);\n";
        CloseNamespace(out, decl.Namespaces);
    }

    for (const RecordDecl &record : file.Parsed.Records)
    {
        out += "\n#define MARLIN_GENERATED_" + file.FileId + "_" + std::to_string(record.BodyLine);
        AppendMacroLine(out, "public:");
        AppendMacroLine(out, "using MarlinReflectedType = " + record.Name + ";");
        if (record.Kind == RecordKind::Interface)
        {
            AppendMacroLine(out, "using MarlinInterfaceType = " + record.Name + ";");
        }
        if (!record.Bases.empty())
        {
            AppendMacroLine(out, "using Super = " + record.Bases[0] + ";");
        }
        AppendMacroLine(out, (record.Api.empty() ? module.Api + " " : std::string()) +
                                 "static const ::Marlin::TypeInfo &StaticType();");
        if (record.Kind == RecordKind::Class)
        {
            AppendMacroLine(out, "MARLIN_DYNAMIC_TYPE()");
        }
        AppendMacroLine(out, "private:");
        for (size_t i = 0; i < record.Bases.size(); ++i)
        {
            AppendMacroLine(out, "using MarlinBase" + std::to_string(i) + " = " + record.Bases[i] + ";");
        }
        AppendMacroLine(out, "friend struct ::Marlin::Private::TypeRegistration<" + record.Name + ">;");
        AppendMacroLine(out, record.IsClassKey ? "private:" : "public:");
        out += '\n';
    }

    out += "\n#undef MARLIN_FILE_ID\n#define MARLIN_FILE_ID " + file.FileId + "\n";
    return out;
}

std::string GenerateSource(const ModuleInfo &module, const GeneratedFile &file)
{
    std::string out = "// Generated by MarlinHeaderTool from " + file.IncludePath + ". Do not edit.\n";
    out += "#include \"" + file.IncludePath + "\"\n\n#include \"Reflection/Generated.h\"\n\n";
    out += "#pragma clang diagnostic ignored \"-Winvalid-offsetof\"\n\n";
    out += "namespace Marlin::Private\n{\n";
    for (const RecordDecl &record : file.Parsed.Records)
    {
        GenerateRecord(out, module, record);
    }
    for (const EnumDecl &decl : file.Parsed.Enums)
    {
        GenerateEnum(out, module, decl);
    }
    out += "} // namespace Marlin::Private\n\n";

    for (const RecordDecl &record : file.Parsed.Records)
    {
        const std::string type = Qualified(record.Namespaces, record.Name);
        out += "const ::Marlin::TypeInfo &" + type + "::StaticType()\n{\n";
        out += "    return ::Marlin::Private::TypeRegistration<" + type + ">::Type;\n}\n\n";
    }
    for (const EnumDecl &decl : file.Parsed.Enums)
    {
        const std::string type = Qualified(decl.Namespaces, decl.Name);
        out += "const ::Marlin::TypeInfo &" + Qualified(decl.Namespaces, "MarlinStaticType") + "(" + type +
               " * /*tag*/)\n{\n";
        out += "    return ::Marlin::Private::TypeRegistration<" + type + ">::Type;\n}\n\n";
    }

    out += "void MarlinRegister_" + file.FileId + "(::Marlin::Private::ModuleRegistrar &registrar)\n{\n";
    for (const RecordDecl &record : file.Parsed.Records)
    {
        out += "    registrar.Add(::Marlin::Private::TypeRegistration<" + Qualified(record.Namespaces, record.Name) +
               ">::Type);\n";
    }
    for (const EnumDecl &decl : file.Parsed.Enums)
    {
        out += "    registrar.Add(::Marlin::Private::TypeRegistration<" + Qualified(decl.Namespaces, decl.Name) +
               ">::Type);\n";
    }
    out += "}\n";
    return out;
}

std::string GenerateModule(const ModuleInfo &module, const std::vector<const GeneratedFile *> &files,
                           const std::vector<UpgradeDecl> &upgrades, const std::vector<TestDecl> &tests)
{
    std::string out = "// Generated by MarlinHeaderTool for module " + module.Name + ". Do not edit.\n";
    out += "#include \"Diagnostics/Log.h\"\n";
    out += "#include \"Reflection/Generated.h\"\n";
    if (!upgrades.empty())
    {
        out += "#include \"MarlinText/MarlinTextSchema.h\"\n";
    }
    if (!tests.empty())
    {
        out += "#include \"Testing/Testing.h\"\n";
    }
    out += '\n';
    for (const GeneratedFile *file : files)
    {
        out += "void MarlinRegister_" + file->FileId + "(::Marlin::Private::ModuleRegistrar &registrar);\n";
    }
    for (const UpgradeDecl &upgrade : upgrades)
    {
        out += '\n';
        OpenNamespace(out, upgrade.Namespaces);
        out += "void MarlinUpgrade_" + upgrade.Type + "_" + std::to_string(upgrade.From) + "_" +
               std::to_string(upgrade.To) + "(::Marlin::MarlinTextObject &object);\n";
        CloseNamespace(out, upgrade.Namespaces);
    }
    for (const TestDecl &test : tests)
    {
        out += '\n';
        OpenNamespace(out, test.Namespaces);
        out += test.Source ? "void MarlinTestSource_" + test.Name + "();\n"
                           : "void MarlinTestRun_" + test.Suite + "_" + test.Name + "();\n";
        CloseNamespace(out, test.Namespaces);
    }
    out += "\nMARLIN_DEFINE_LOG_CATEGORY_MARKERS();\n";
    out += "\nextern \"C\" " + module.Api + " void Register_" + module.Name + "()\n{\n";
    out += "    MARLIN_REGISTER_LOG_CATEGORIES();\n";
    out += "    ::Marlin::Private::ModuleRegistrar registrar(" + Quote(module.Name) + ");\n";
    for (const GeneratedFile *file : files)
    {
        out += "    MarlinRegister_" + file->FileId + "(registrar);\n";
    }
    out += "    registrar.Commit();\n";
    for (const UpgradeDecl &upgrade : upgrades)
    {
        out += "    ::Marlin::MarlinTextSchema::RegisterUpgrade(" + Quote(upgrade.Type) + ", " +
               std::to_string(upgrade.From) + ", " + std::to_string(upgrade.To) + ", &" +
               Qualified(upgrade.Namespaces, "MarlinUpgrade_" + upgrade.Type + "_" + std::to_string(upgrade.From) +
                                                 "_" + std::to_string(upgrade.To)) +
               ");\n";
    }
    // Tests of module X live in XTests and are named after X.
    const std::string testedModule = module.Name.ends_with("Tests") && module.Name.size() > 5
                                         ? module.Name.substr(0, module.Name.size() - 5)
                                         : module.Name;
    for (const TestDecl &test : tests)
    {
        if (test.Source)
        {
            out += "    ::Marlin::Testing::RegisterTestSource(&" +
                   Qualified(test.Namespaces, "MarlinTestSource_" + test.Name) + ");\n";
            continue;
        }
        std::string tags;
        for (const std::string &tag : test.Tags)
        {
            tags += (tags.empty() ? "" : ",") + tag;
        }
        out += "    ::Marlin::Testing::RegisterTest({";
        out += ".Module = " + Quote(testedModule);
        out += ", .Suite = " + Quote(test.Suite);
        out += ", .Name = " + Quote(test.Name);
        out += ", .Tags = " + Quote(tags);
        out += ", .Run = &" + Qualified(test.Namespaces, "MarlinTestRun_" + test.Suite + "_" + test.Name);
        out += ", .File = " + Quote(test.File);
        out += ", .Line = " + std::to_string(test.Line);
        out += "});\n";
    }
    out += "}\n";
    return out;
}
} // namespace Marlin::HeaderTool
