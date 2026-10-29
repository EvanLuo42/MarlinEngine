#include "ScriptVM/ScriptModuleData.h"

#include "Memory/MemoryOps.h"
#include "ScriptVM/ScriptBytecode.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
constexpr uint32 SectionCount = static_cast<uint32>(ScriptSection::Count);
// RECOVERY-DECISION SA1: the header is the magic, the version, the package string, the interface hash and a table of
// (offset, size) per section, all little-endian; every number in a section is a fixed-width u8, u32 or u64, and only
// line tables use varints.
constexpr uint32 HeaderBytes = 4 + 4 + 4 + 8 + SectionCount * 8;

constexpr StringView SectionNames[] = {"Strings", "Shapes",  "Types", "Functions", "Statics",
                                       "Imports", "Exports", "Debug", "Attributes"};

class ByteWriter
{
public:
    explicit ByteWriter(List<uint8> &bytes) : _bytes(&bytes)
    {
    }

    void U8(uint8 value)
    {
        _bytes->Add(value);
    }

    void U32(uint32 value)
    {
        for (uint32 i = 0; i < 4; ++i)
        {
            _bytes->Add(static_cast<uint8>(value >> (i * 8)));
        }
    }

    void U64(uint64 value)
    {
        U32(static_cast<uint32>(value));
        U32(static_cast<uint32>(value >> 32));
    }

    void Raw(const void *data, usize size)
    {
        if (size == 0)
        {
            return;
        }
        const usize at = _bytes->Count();
        _bytes->ResizeUninitialized(at + size);
        MemCopy(_bytes->Data() + at, data, size);
    }

    void Words(const List<uint32> &words)
    {
        U32(static_cast<uint32>(words.Count()));
        for (const uint32 word : words)
        {
            U32(word);
        }
    }

    void Bytes(const List<uint8> &bytes)
    {
        U32(static_cast<uint32>(bytes.Count()));
        Raw(bytes.Data(), bytes.Count());
    }

    void PatchU32(usize at, uint32 value)
    {
        for (uint32 i = 0; i < 4; ++i)
        {
            (*_bytes)[at + i] = static_cast<uint8>(value >> (i * 8));
        }
    }

    [[nodiscard]] usize Count() const
    {
        return _bytes->Count();
    }

private:
    List<uint8> *_bytes;
};

class ByteReader
{
public:
    explicit ByteReader(Span<const uint8> bytes) : _bytes(bytes)
    {
    }

    [[nodiscard]] bool Ok() const
    {
        return _ok;
    }

    [[nodiscard]] bool AtEnd() const
    {
        return _at == _bytes.Count();
    }

    uint8 U8()
    {
        if (!Need(1))
        {
            return 0;
        }
        return _bytes[_at++];
    }

    uint32 U32()
    {
        if (!Need(4))
        {
            return 0;
        }
        uint32 value = 0;
        for (uint32 i = 0; i < 4; ++i)
        {
            value |= static_cast<uint32>(_bytes[_at + i]) << (i * 8);
        }
        _at += 4;
        return value;
    }

    uint64 U64()
    {
        const uint64 low = U32();
        return low | (static_cast<uint64>(U32()) << 32);
    }

    void Raw(void *data, usize size)
    {
        if (size == 0 || !Need(size))
        {
            return;
        }
        MemCopy(data, _bytes.Data() + _at, size);
        _at += size;
    }

    /// A count of elements that take at least *elementBytes* each; a count the rest cannot hold fails.
    uint32 Count(usize elementBytes)
    {
        const uint32 count = U32();
        if (_ok && static_cast<uint64>(count) * elementBytes > _bytes.Count() - _at)
        {
            _ok = false;
            return 0;
        }
        return _ok ? count : 0;
    }

    void Words(List<uint32> &words)
    {
        const uint32 count = Count(4);
        words.Resize(count);
        for (uint32 i = 0; i < count; ++i)
        {
            words[i] = U32();
        }
    }

    void Bytes(List<uint8> &bytes)
    {
        const uint32 count = Count(1);
        bytes.ResizeUninitialized(count);
        Raw(bytes.Data(), count);
    }

private:
    bool Need(usize bytes)
    {
        if (!_ok || bytes > _bytes.Count() - _at)
        {
            _ok = false;
            return false;
        }
        return true;
    }

    Span<const uint8> _bytes;
    usize _at = 0;
    bool _ok = true;
};

void WriteSection(ScriptSection section, const ScriptModuleData &module, ByteWriter &out)
{
    switch (section)
    {
    case ScriptSection::Strings:
        out.U32(static_cast<uint32>(module.Strings.Count()));
        for (const String &text : module.Strings)
        {
            out.U32(static_cast<uint32>(text.AsView().Length()));
            out.Raw(text.AsView().Data(), text.AsView().Length());
        }
        break;
    case ScriptSection::Shapes:
        out.U32(static_cast<uint32>(module.Shapes.Count()));
        for (const ScriptShape &shape : module.Shapes)
        {
            out.U8(static_cast<uint8>(shape.Kind));
            out.U8(shape.Width);
            out.U8(shape.Signed ? 1 : 0);
            out.U8(static_cast<uint8>(shape.Space));
            out.U32(shape.Count);
            out.U32(shape.Rows);
            out.U32(shape.Size);
            out.U32(shape.Alignment);
            out.U32(shape.Stride);
            out.U32(shape.Element);
            out.U32(shape.Name);
            out.U32(static_cast<uint32>(shape.Fields.Count()));
            for (const ScriptShapeField &field : shape.Fields)
            {
                out.U32(field.Name);
                out.U32(field.Offset);
                out.U32(field.Shape);
            }
            out.Words(shape.Parameters);
        }
        break;
    case ScriptSection::Types:
        out.U32(static_cast<uint32>(module.Types.Count()));
        for (const ScriptTypeRecord &type : module.Types)
        {
            out.U8(static_cast<uint8>(type.Kind));
            out.U8(type.Flags);
            out.U32(type.Name);
            out.U32(type.Base);
            out.U32(type.Layout);
            out.Words(type.VirtualSlots);
            out.U32(static_cast<uint32>(type.Interfaces.Count()));
            for (const ScriptInterfaceTable &table : type.Interfaces)
            {
                out.U32(table.Interface);
                out.Words(table.Methods);
            }
            out.Words(type.Implements);
        }
        break;
    case ScriptSection::Functions:
        out.U32(static_cast<uint32>(module.Functions.Count()));
        for (const ScriptFunctionRecord &function : module.Functions)
        {
            out.U32(function.Name);
            out.U8(static_cast<uint8>(function.Kind));
            out.U8(function.Flags);
            out.U32(function.Signature);
            out.U32(function.NativeName);
            out.U32(function.ParameterSlots);
            out.U32(function.FrameSlots);
            out.U32(function.ZeroSlots);
            out.U32(function.ReturnSlots);
            out.U32(function.ResultShape);
            out.U64(function.ReadOnlyParameters);
            out.Words(function.Code);
            out.U32(static_cast<uint32>(function.Constants.Count()));
            for (const ScriptSlot &constant : function.Constants)
            {
                out.Raw(constant.Bytes, sizeof(constant.Bytes));
            }
            out.Words(function.Blocks);
            out.U32(static_cast<uint32>(function.Switches.Count()));
            for (const ScriptSwitchTable &table : function.Switches)
            {
                out.U8(table.Is64 ? 1 : 0);
                out.U32(table.Default);
                out.U32(static_cast<uint32>(table.Cases.Count()));
                for (const ScriptSwitchCase &entry : table.Cases)
                {
                    out.U64(static_cast<uint64>(entry.Value));
                    out.U32(entry.Target);
                }
            }
            out.U32(static_cast<uint32>(function.InterfaceSites.Count()));
            for (const ScriptInterfaceSite &site : function.InterfaceSites)
            {
                out.U32(site.Interface);
                out.U32(site.Method);
            }
            out.U32(function.MapBytes);
            out.Bytes(function.Maps);
            out.U32(static_cast<uint32>(function.Safepoints.Count()));
            for (const ScriptSafepoint &safepoint : function.Safepoints)
            {
                out.U32(safepoint.Pc);
                out.U32(safepoint.Map);
            }
            out.Bytes(function.LineTable);
        }
        break;
    case ScriptSection::Statics:
        out.U32(module.StaticSize);
        out.U32(module.StaticAlignment);
        out.U32(static_cast<uint32>(module.Statics.Count()));
        for (const ScriptStaticField &field : module.Statics)
        {
            out.U32(field.Name);
            out.U32(field.Offset);
            out.U32(field.Shape);
        }
        out.Words(module.StaticInitializers);
        break;
    case ScriptSection::Imports:
        out.U32(static_cast<uint32>(module.Imports.Count()));
        for (const ScriptImport &import : module.Imports)
        {
            out.U8(static_cast<uint8>(import.Kind));
            out.U32(import.Index);
            out.U32(import.Name);
            out.U64(import.Hash);
        }
        break;
    case ScriptSection::Exports:
        out.U32(static_cast<uint32>(module.Exports.Count()));
        for (const ScriptExport &entry : module.Exports)
        {
            out.U8(static_cast<uint8>(entry.Kind));
            out.U32(entry.Index);
            out.U64(entry.Hash);
        }
        break;
    case ScriptSection::Debug:
        out.U32(static_cast<uint32>(module.Files.Count()));
        for (const ScriptSourceFile &file : module.Files)
        {
            out.U32(file.Path);
            out.U64(file.ContentHash);
        }
        break;
    case ScriptSection::Attributes:
        out.U32(static_cast<uint32>(module.Attributes.Count()));
        for (const ScriptAttribute &attribute : module.Attributes)
        {
            out.U8(static_cast<uint8>(attribute.Target));
            out.U32(attribute.Index);
            out.U32(attribute.Name);
            out.U32(static_cast<uint32>(attribute.Arguments.Count()));
            for (const ScriptAttributeArgument &argument : attribute.Arguments)
            {
                out.U32(argument.Name);
                out.U32(argument.Value);
            }
        }
        break;
    case ScriptSection::Count:
        break;
    }
}

void ReadSection(ScriptSection section, ByteReader &in, ScriptModuleData &module)
{
    switch (section)
    {
    case ScriptSection::Strings: {
        const uint32 count = in.Count(4);
        module.Strings.Reserve(count);
        for (uint32 i = 0; i < count && in.Ok(); ++i)
        {
            const uint32 length = in.Count(1);
            List<char> text;
            text.ResizeUninitialized(length);
            in.Raw(text.Data(), length);
            module.Strings.Add(String(StringView(text.Data(), in.Ok() ? length : 0)));
        }
        break;
    }
    case ScriptSection::Shapes: {
        const uint32 count = in.Count(40);
        module.Shapes.Resize(count);
        for (ScriptShape &shape : module.Shapes)
        {
            shape.Kind = static_cast<ScriptShapeKind>(in.U8());
            shape.Width = in.U8();
            shape.Signed = in.U8() != 0;
            shape.Space = static_cast<ScriptPointerSpace>(in.U8());
            shape.Count = in.U32();
            shape.Rows = in.U32();
            shape.Size = in.U32();
            shape.Alignment = in.U32();
            shape.Stride = in.U32();
            shape.Element = in.U32();
            shape.Name = in.U32();
            shape.Fields.Resize(in.Count(12));
            for (ScriptShapeField &field : shape.Fields)
            {
                field.Name = in.U32();
                field.Offset = in.U32();
                field.Shape = in.U32();
            }
            in.Words(shape.Parameters);
        }
        break;
    }
    case ScriptSection::Types: {
        const uint32 count = in.Count(26);
        module.Types.Resize(count);
        for (ScriptTypeRecord &type : module.Types)
        {
            type.Kind = static_cast<ScriptTypeKind>(in.U8());
            type.Flags = in.U8();
            type.Name = in.U32();
            type.Base = in.U32();
            type.Layout = in.U32();
            in.Words(type.VirtualSlots);
            type.Interfaces.Resize(in.Count(8));
            for (ScriptInterfaceTable &table : type.Interfaces)
            {
                table.Interface = in.U32();
                in.Words(table.Methods);
            }
            in.Words(type.Implements);
        }
        break;
    }
    case ScriptSection::Functions: {
        const uint32 count = in.Count(74);
        module.Functions.Resize(count);
        for (ScriptFunctionRecord &function : module.Functions)
        {
            function.Name = in.U32();
            function.Kind = static_cast<ScriptFunctionKind>(in.U8());
            function.Flags = in.U8();
            function.Signature = in.U32();
            function.NativeName = in.U32();
            function.ParameterSlots = in.U32();
            function.FrameSlots = in.U32();
            function.ZeroSlots = in.U32();
            function.ReturnSlots = in.U32();
            function.ResultShape = in.U32();
            function.ReadOnlyParameters = in.U64();
            in.Words(function.Code);
            function.Constants.Resize(in.Count(sizeof(ScriptSlot)));
            for (ScriptSlot &constant : function.Constants)
            {
                in.Raw(constant.Bytes, sizeof(constant.Bytes));
            }
            in.Words(function.Blocks);
            function.Switches.Resize(in.Count(9));
            for (ScriptSwitchTable &table : function.Switches)
            {
                table.Is64 = in.U8() != 0;
                table.Default = in.U32();
                table.Cases.Resize(in.Count(12));
                for (ScriptSwitchCase &entry : table.Cases)
                {
                    entry.Value = static_cast<int64>(in.U64());
                    entry.Target = in.U32();
                }
            }
            function.InterfaceSites.Resize(in.Count(8));
            for (ScriptInterfaceSite &site : function.InterfaceSites)
            {
                site.Interface = in.U32();
                site.Method = in.U32();
            }
            function.MapBytes = in.U32();
            in.Bytes(function.Maps);
            function.Safepoints.Resize(in.Count(8));
            for (ScriptSafepoint &safepoint : function.Safepoints)
            {
                safepoint.Pc = in.U32();
                safepoint.Map = in.U32();
            }
            in.Bytes(function.LineTable);
        }
        break;
    }
    case ScriptSection::Statics: {
        module.StaticSize = in.U32();
        module.StaticAlignment = in.U32();
        module.Statics.Resize(in.Count(12));
        for (ScriptStaticField &field : module.Statics)
        {
            field.Name = in.U32();
            field.Offset = in.U32();
            field.Shape = in.U32();
        }
        in.Words(module.StaticInitializers);
        break;
    }
    case ScriptSection::Imports: {
        module.Imports.Resize(in.Count(17));
        for (ScriptImport &import : module.Imports)
        {
            import.Kind = static_cast<ScriptImportKind>(in.U8());
            import.Index = in.U32();
            import.Name = in.U32();
            import.Hash = in.U64();
        }
        break;
    }
    case ScriptSection::Exports: {
        module.Exports.Resize(in.Count(13));
        for (ScriptExport &entry : module.Exports)
        {
            entry.Kind = static_cast<ScriptExportKind>(in.U8());
            entry.Index = in.U32();
            entry.Hash = in.U64();
        }
        break;
    }
    case ScriptSection::Debug: {
        module.Files.Resize(in.Count(12));
        for (ScriptSourceFile &file : module.Files)
        {
            file.Path = in.U32();
            file.ContentHash = in.U64();
        }
        break;
    }
    case ScriptSection::Attributes: {
        module.Attributes.Resize(in.Count(13));
        for (ScriptAttribute &attribute : module.Attributes)
        {
            attribute.Target = static_cast<ScriptAttributeTarget>(in.U8());
            attribute.Index = in.U32();
            attribute.Name = in.U32();
            attribute.Arguments.Resize(in.Count(8));
            for (ScriptAttributeArgument &argument : attribute.Arguments)
            {
                argument.Name = in.U32();
                argument.Value = in.U32();
            }
        }
        break;
    }
    case ScriptSection::Count:
        break;
    }
}

bool CheckIndex(uint32 index, usize count, bool optional = false)
{
    return (optional && index == ~0u) || index < count;
}

/// Checks that every index names an entry of its table, and the enums are in range.
bool Validate(const ScriptModuleData &module, String &error)
{
    const usize strings = module.Strings.Count();
    const usize shapes = module.Shapes.Count();
    const usize types = module.Types.Count();
    const usize functions = module.Functions.Count();
    auto fail = [&](StringView what, usize index) {
        error = Format("{0} {1} refers past the end of a table", what, index);
        return false;
    };
    if (!CheckIndex(module.Package, strings))
    {
        return fail("the package name", 0);
    }
    for (usize i = 0; i < shapes; ++i)
    {
        const ScriptShape &shape = module.Shapes[i];
        if (shape.Kind > ScriptShapeKind::Function || shape.Space > ScriptPointerSpace::Static)
        {
            return fail("shape", i);
        }
        const bool hasElement = shape.Kind == ScriptShapeKind::Vector || shape.Kind == ScriptShapeKind::Matrix ||
                                shape.Kind == ScriptShapeKind::Array || shape.Kind == ScriptShapeKind::Pointer ||
                                shape.Kind == ScriptShapeKind::Function;
        if ((hasElement && !CheckIndex(shape.Element, shapes)) ||
            (shape.Kind == ScriptShapeKind::Reference && !CheckIndex(shape.Element, types, true)) ||
            !CheckIndex(shape.Name, strings, true))
        {
            return fail("shape", i);
        }
        for (const ScriptShapeField &field : shape.Fields)
        {
            if (!CheckIndex(field.Name, strings, true) || !CheckIndex(field.Shape, shapes))
            {
                return fail("shape", i);
            }
        }
        for (const uint32 parameter : shape.Parameters)
        {
            if (!CheckIndex(parameter, shapes))
            {
                return fail("shape", i);
            }
        }
    }
    for (usize i = 0; i < types; ++i)
    {
        const ScriptTypeRecord &type = module.Types[i];
        bool ok = type.Kind <= ScriptTypeKind::Value && CheckIndex(type.Name, strings) &&
                  CheckIndex(type.Base, types, true) && CheckIndex(type.Layout, shapes, true);
        for (const uint32 slot : type.VirtualSlots)
        {
            ok = ok && CheckIndex(slot, functions);
        }
        for (const ScriptInterfaceTable &table : type.Interfaces)
        {
            ok = ok && CheckIndex(table.Interface, types);
            for (const uint32 method : table.Methods)
            {
                ok = ok && CheckIndex(method, functions);
            }
        }
        for (const uint32 inherited : type.Implements)
        {
            ok = ok && CheckIndex(inherited, types);
        }
        if (!ok)
        {
            return fail("type", i);
        }
    }
    for (usize i = 0; i < functions; ++i)
    {
        const ScriptFunctionRecord &function = module.Functions[i];
        const usize code = function.Code.Count();
        bool ok = function.Kind <= ScriptFunctionKind::Kernel && CheckIndex(function.Name, strings) &&
                  CheckIndex(function.Signature, shapes) && CheckIndex(function.NativeName, strings, true) &&
                  CheckIndex(function.ResultShape, shapes, true) &&
                  ((function.Flags & ScriptFunctionFlags::Native) == 0 || function.NativeName != ~0u);
        for (const uint32 block : function.Blocks)
        {
            ok = ok && block <= code;
        }
        for (const ScriptSwitchTable &table : function.Switches)
        {
            ok = ok && table.Default < code;
            for (const ScriptSwitchCase &entry : table.Cases)
            {
                ok = ok && entry.Target < code;
            }
        }
        for (const ScriptInterfaceSite &site : function.InterfaceSites)
        {
            ok = ok && CheckIndex(site.Interface, types);
        }
        const usize mapSize = static_cast<usize>(function.MapBytes) * 2;
        ok = ok && (mapSize == 0 ? function.Maps.IsEmpty() : function.Maps.Count() % mapSize == 0) &&
             uint64{function.ParameterSlots} + function.ZeroSlots <= uint64{function.FrameSlots};
        const usize maps = mapSize == 0 ? 0 : function.Maps.Count() / mapSize;
        for (const ScriptSafepoint &safepoint : function.Safepoints)
        {
            ok = ok && safepoint.Pc < code && safepoint.Map < maps;
        }
        if (!ok)
        {
            return fail("function", i);
        }
    }
    for (usize i = 0; i < module.Statics.Count(); ++i)
    {
        const ScriptStaticField &field = module.Statics[i];
        if (!CheckIndex(field.Name, strings) || !CheckIndex(field.Shape, shapes) ||
            static_cast<uint64>(field.Offset) + module.Shapes[field.Shape].Size > module.StaticSize)
        {
            return fail("static field", i);
        }
    }
    if (module.StaticAlignment == 0 || (module.StaticAlignment & (module.StaticAlignment - 1)) != 0)
    {
        return fail("static field", module.Statics.Count());
    }
    for (usize i = 0; i < module.StaticInitializers.Count(); ++i)
    {
        if (!CheckIndex(module.StaticInitializers[i], functions))
        {
            return fail("static initializer", i);
        }
    }
    for (usize i = 0; i < module.Imports.Count(); ++i)
    {
        const ScriptImport &import = module.Imports[i];
        const usize targets = import.Kind == ScriptImportKind::Type ? types : functions;
        if (import.Kind > ScriptImportKind::Type || !CheckIndex(import.Index, targets) ||
            !CheckIndex(import.Name, strings))
        {
            return fail("import", i);
        }
    }
    for (usize i = 0; i < module.Exports.Count(); ++i)
    {
        const ScriptExport &entry = module.Exports[i];
        const usize targets = entry.Kind == ScriptExportKind::Type ? types : functions;
        if (entry.Kind > ScriptExportKind::Type || !CheckIndex(entry.Index, targets))
        {
            return fail("export", i);
        }
    }
    for (usize i = 0; i < module.Files.Count(); ++i)
    {
        if (!CheckIndex(module.Files[i].Path, strings))
        {
            return fail("source file", i);
        }
    }
    for (usize i = 0; i < module.Attributes.Count(); ++i)
    {
        const ScriptAttribute &attribute = module.Attributes[i];
        bool ok = attribute.Target <= ScriptAttributeTarget::Function && CheckIndex(attribute.Name, strings);
        if (ok && attribute.Target == ScriptAttributeTarget::Shape)
        {
            ok = CheckIndex(attribute.Index, shapes) && module.Shapes[attribute.Index].Kind == ScriptShapeKind::Struct;
        }
        else if (ok)
        {
            ok = CheckIndex(attribute.Index, functions);
        }
        for (const ScriptAttributeArgument &argument : attribute.Arguments)
        {
            ok = ok && CheckIndex(argument.Name, strings) && CheckIndex(argument.Value, strings);
        }
        if (!ok)
        {
            return fail("attribute", i);
        }
    }
    return true;
}

uint32 ReadVarint(Span<const uint8> bytes, usize &at, bool &ok)
{
    uint32 value = 0;
    for (uint32 shift = 0; shift < 35; shift += 7)
    {
        if (at >= bytes.Count())
        {
            ok = false;
            return 0;
        }
        const uint8 byte = bytes[at++];
        value |= static_cast<uint32>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0)
        {
            return value;
        }
    }
    ok = false;
    return 0;
}

void WriteVarint(List<uint8> &bytes, uint32 value)
{
    while (value >= 0x80)
    {
        bytes.Add(static_cast<uint8>(value | 0x80));
        value >>= 7;
    }
    bytes.Add(static_cast<uint8>(value));
}
} // namespace

void WriteScriptModule(const ScriptModuleData &module, List<uint8> &bytes)
{
    bytes.Clear();
    ByteWriter out(bytes);
    out.U32(ScriptModuleMagic);
    out.U32(ScriptFormatVersion);
    out.U32(module.Package);
    out.U64(module.InterfaceHash);
    const usize table = out.Count();
    for (uint32 i = 0; i < SectionCount * 2; ++i)
    {
        out.U32(0);
    }
    for (uint32 i = 0; i < SectionCount; ++i)
    {
        const usize start = out.Count();
        WriteSection(static_cast<ScriptSection>(i), module, out);
        out.PatchU32(table + i * 8, static_cast<uint32>(start));
        out.PatchU32(table + i * 8 + 4, static_cast<uint32>(out.Count() - start));
    }
}

bool ReadScriptModule(Span<const uint8> bytes, ScriptModuleData &module, String &error)
{
    module = {};
    ByteReader header(bytes);
    if (bytes.Count() < HeaderBytes || header.U32() != ScriptModuleMagic)
    {
        error = String("not a script module");
        return false;
    }
    const uint32 version = header.U32();
    if (version != ScriptFormatVersion)
    {
        error = Format("format version {0}, but this VM reads {1}", version, ScriptFormatVersion);
        return false;
    }
    module.Package = header.U32();
    module.InterfaceHash = header.U64();
    for (uint32 i = 0; i < SectionCount; ++i)
    {
        const uint32 offset = header.U32();
        const uint32 size = header.U32();
        if (offset < HeaderBytes || static_cast<uint64>(offset) + size > bytes.Count())
        {
            error = Format("section {0} lies outside the module", SectionNames[i]);
            return false;
        }
        ByteReader in(bytes.Slice(offset, size));
        ReadSection(static_cast<ScriptSection>(i), in, module);
        if (!in.Ok() || !in.AtEnd())
        {
            error = Format("section {0} is malformed", SectionNames[i]);
            return false;
        }
    }
    return Validate(module, error);
}

// RECOVERY-DECISION SA2: a line-table row is four varints: the word delta, the file, the line delta (zigzag) and
// the column.
void AppendScriptLineEntry(List<uint8> &table, const ScriptLineEntry &previous, const ScriptLineEntry &entry)
{
    WriteVarint(table, entry.Pc - previous.Pc);
    WriteVarint(table, entry.File);
    const int32 line = static_cast<int32>(entry.Line) - static_cast<int32>(previous.Line);
    WriteVarint(table, (static_cast<uint32>(line) << 1) ^ static_cast<uint32>(line >> 31));
    WriteVarint(table, entry.Column);
}

bool DecodeScriptLineTable(Span<const uint8> table, List<ScriptLineEntry> &entries)
{
    entries.Clear();
    ScriptLineEntry previous;
    usize at = 0;
    bool ok = true;
    while (at < table.Count())
    {
        ScriptLineEntry entry;
        entry.Pc = previous.Pc + ReadVarint(table, at, ok);
        entry.File = ReadVarint(table, at, ok);
        const uint32 zigzag = ReadVarint(table, at, ok);
        const int32 line = static_cast<int32>(zigzag >> 1) ^ -static_cast<int32>(zigzag & 1);
        entry.Line = static_cast<uint32>(static_cast<int32>(previous.Line) + line);
        entry.Column = ReadVarint(table, at, ok);
        if (!ok)
        {
            return false;
        }
        entries.Add(entry);
        previous = entry;
    }
    return true;
}
} // namespace Marlin
