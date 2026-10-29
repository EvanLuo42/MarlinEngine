#include "ScriptBackend/ScriptDisassembler.h"

#include "ScriptVM/ScriptBytecode.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "Templates/Bit.h"

namespace Marlin
{
namespace
{
/// Writes a module as text: the header, files, types, statics, linkage, then every function.
class Disassembler
{
public:
    Disassembler(const ScriptModuleData &module, StringWriter &output) : _module(&module), _output(&output)
    {
    }

    void Run()
    {
        const ScriptModuleData &module = *_module;
        FormatTo(*_output, "module {0} interface {1:X16}\n", Text(module.Package), module.InterfaceHash);
        for (usize i = 0; i < module.Files.Count(); ++i)
        {
            FormatTo(*_output, "file {0} \"{1}\" {2:X16}\n", i, Text(module.Files[i].Path),
                     module.Files[i].ContentHash);
        }
        for (uint32 i = 0; i < module.Types.Count(); ++i)
        {
            Type(i);
        }
        if (module.StaticSize != 0 || !module.Statics.IsEmpty())
        {
            FormatTo(*_output, "statics {0} bytes, align {1}\n", module.StaticSize, module.StaticAlignment);
            for (const ScriptStaticField &field : module.Statics)
            {
                FormatTo(*_output, "  +{0} {1}: ", field.Offset, Text(field.Name));
                Shape(field.Shape);
                _output->Append('\n');
            }
        }
        for (const uint32 initializer : module.StaticInitializers)
        {
            FormatTo(*_output, "initializer {0}\n", FunctionName(initializer));
        }
        for (const ScriptImport &import : module.Imports)
        {
            const StringView kind = import.Kind == ScriptImportKind::Type     ? StringView("type")
                                    : import.Kind == ScriptImportKind::Native ? StringView("native")
                                                                              : StringView("function");
            FormatTo(*_output, "import {0} #{1} \"{2}\" {3:X16}\n", kind, import.Index, Text(import.Name),
                     import.Hash);
        }
        for (const ScriptExport &entry : module.Exports)
        {
            const bool type = entry.Kind == ScriptExportKind::Type;
            FormatTo(*_output, "export {0} #{1} {2:X16}\n", type ? StringView("type") : StringView("function"),
                     entry.Index, entry.Hash);
        }
        for (const ScriptAttribute &attribute : module.Attributes)
        {
            if (attribute.Target == ScriptAttributeTarget::Function)
            {
                FormatTo(*_output, "attribute {0} on {1}", Text(attribute.Name), FunctionName(attribute.Index));
            }
            else
            {
                FormatTo(*_output, "attribute {0} on ", Text(attribute.Name));
                Shape(attribute.Index);
            }
            for (const ScriptAttributeArgument &argument : attribute.Arguments)
            {
                FormatTo(*_output, " {0}=\"{1}\"", Text(argument.Name), Text(argument.Value));
            }
            _output->Append('\n');
        }
        for (uint32 i = 0; i < module.Functions.Count(); ++i)
        {
            Function(i);
        }
    }

private:
    [[nodiscard]] StringView Text(uint32 index) const
    {
        return index < _module->Strings.Count() ? _module->Strings[index].AsView() : StringView("?");
    }

    [[nodiscard]] StringView TypeName(uint32 index) const
    {
        return index < _module->Types.Count() ? Text(_module->Types[index].Name) : StringView("?");
    }

    [[nodiscard]] StringView FunctionName(uint32 index) const
    {
        return index < _module->Functions.Count() ? Text(_module->Functions[index].Name) : StringView("?");
    }

    void Shape(uint32 index, uint32 depth = 0)
    {
        if (index >= _module->Shapes.Count() || depth > 8)
        {
            _output->Append('?');
            return;
        }
        const ScriptShape &shape = _module->Shapes[index];
        switch (shape.Kind)
        {
        case ScriptShapeKind::Void:
            _output->Append("void");
            break;
        case ScriptShapeKind::Bool:
            _output->Append("bool");
            break;
        case ScriptShapeKind::Int:
            FormatTo(*_output, "{0}{1}", shape.Signed ? 'i' : 'u', shape.Width);
            break;
        case ScriptShapeKind::Float:
            FormatTo(*_output, "f{0}", shape.Width);
            break;
        case ScriptShapeKind::Vector:
            Shape(shape.Element, depth + 1);
            FormatTo(*_output, "x{0}", shape.Count);
            break;
        case ScriptShapeKind::Matrix: {
            const uint32 scalar =
                shape.Element < _module->Shapes.Count() ? _module->Shapes[shape.Element].Element : ~0u;
            Shape(scalar, depth + 1);
            FormatTo(*_output, "x{0}x{1}", shape.Rows, shape.Count);
            break;
        }
        case ScriptShapeKind::Array:
            _output->Append('[');
            Shape(shape.Element, depth + 1);
            FormatTo(*_output, "; {0}, stride {1} align {2}]", shape.Count, shape.Stride, shape.Alignment);
            break;
        case ScriptShapeKind::Struct:
            _output->Append('%').Append(shape.Name != ~0u ? Text(shape.Name) : StringView("struct"));
            if (depth == 0)
            {
                FormatTo(*_output, " {{size {0}, stride {1} align {2}", shape.Size, shape.Stride, shape.Alignment);
                for (const ScriptShapeField &field : shape.Fields)
                {
                    FormatTo(*_output, ", +{0} ", field.Offset);
                    Shape(field.Shape, depth + 1);
                }
                _output->Append('}');
            }
            break;
        case ScriptShapeKind::Reference:
            FormatTo(*_output, "ref<{0}>",
                     shape.Element != ~0u ? TypeName(shape.Element)
                                          : (shape.Name != ~0u ? Text(shape.Name) : StringView("?")));
            break;
        case ScriptShapeKind::Pointer: {
            const StringView space = shape.Space == ScriptPointerSpace::Managed  ? StringView("managed")
                                     : shape.Space == ScriptPointerSpace::Static ? StringView("static")
                                                                                 : StringView("function");
            FormatTo(*_output, "ptr<{0}, ", space);
            Shape(shape.Element, depth + 1);
            _output->Append('>');
            break;
        }
        case ScriptShapeKind::Function:
            _output->Append("function(");
            for (usize i = 0; i < shape.Parameters.Count(); ++i)
            {
                _output->Append(i == 0 ? "" : ", ");
                Shape(shape.Parameters[i], depth + 1);
            }
            _output->Append(") -> ");
            Shape(shape.Element, depth + 1);
            break;
        }
    }

    void Type(uint32 index)
    {
        const ScriptTypeRecord &type = _module->Types[index];
        StringView kind;
        switch (type.Kind)
        {
        case ScriptTypeKind::Class:
            kind = "class";
            break;
        case ScriptTypeKind::Interface:
            kind = "interface";
            break;
        case ScriptTypeKind::Delegate:
            kind = "delegate";
            break;
        case ScriptTypeKind::Array:
            kind = "array";
            break;
        case ScriptTypeKind::String:
            kind = "string";
            break;
        case ScriptTypeKind::Value:
            kind = "value";
            break;
        }
        FormatTo(*_output, "type #{0} {1} {2}", index, kind, Text(type.Name));
        if ((type.Flags & ScriptTypeFlags::Abstract) != 0)
        {
            _output->Append(" abstract");
        }
        if ((type.Flags & ScriptTypeFlags::Sealed) != 0)
        {
            _output->Append(" sealed");
        }
        if ((type.Flags & ScriptTypeFlags::Imported) != 0)
        {
            _output->Append(" imported");
        }
        if (type.Layout != ~0u)
        {
            _output->Append(' ');
            Shape(type.Layout);
        }
        _output->Append('\n');
        if (type.Base != ~0u)
        {
            FormatTo(*_output, "  extends {0}\n", TypeName(type.Base));
        }
        for (const uint32 inherited : type.Implements)
        {
            FormatTo(*_output, "  extends {0}\n", TypeName(inherited));
        }
        for (usize i = 0; i < type.VirtualSlots.Count(); ++i)
        {
            FormatTo(*_output, "  slot {0}: {1}\n", i, FunctionName(type.VirtualSlots[i]));
        }
        for (const ScriptInterfaceTable &table : type.Interfaces)
        {
            FormatTo(*_output, "  implements {0}:\n", TypeName(table.Interface));
            for (usize i = 0; i < table.Methods.Count(); ++i)
            {
                FormatTo(*_output, "    slot {0}: {1}\n", i, FunctionName(table.Methods[i]));
            }
        }
    }

    void Access(uint32 word)
    {
        const uint32 payload = GetScriptAccessPayload(word);
        switch (GetScriptAccess(word))
        {
        case ScriptAccess::I8:
            _output->Append("i8");
            break;
        case ScriptAccess::U8:
            _output->Append("u8");
            break;
        case ScriptAccess::I16:
            _output->Append("i16");
            break;
        case ScriptAccess::U16:
            _output->Append("u16");
            break;
        case ScriptAccess::B32:
            _output->Append("b32");
            break;
        case ScriptAccess::B64:
            _output->Append("b64");
            break;
        case ScriptAccess::Bytes:
            FormatTo(*_output, "bytes {0}", payload);
            break;
        case ScriptAccess::Reference:
            _output->Append("ref");
            break;
        case ScriptAccess::Typed:
            _output->Append("typed ");
            Shape(payload, 1);
            break;
        default:
            FormatTo(*_output, "access {0:X8}", word);
            break;
        }
    }

    void Element(uint32 word)
    {
        FormatTo(*_output, "stride {0} align {1}", word & 0xFFFFFF, 1u << (word >> 24));
    }

    void Instruction(const ScriptFunctionRecord &function, uint32 pc, const ScriptDecodedInstruction &instruction)
    {
        const ScriptOpInfo &info = GetScriptOpInfo(instruction.Op);
        const uint32 a = instruction.A;
        const uint32 b = instruction.B;
        const uint32 c = instruction.C;
        const uint32 t0 = instruction.Trailing[0];
        const uint32 t1 = instruction.Trailing[1];
        const int64 next = static_cast<int64>(pc) + instruction.Words;
        FormatTo(*_output, "  {0:04} {1}", pc, info.Name);
        switch (instruction.Op)
        {
        case ScriptOp::MoveN:
        case ScriptOp::Await:
            FormatTo(*_output, " r{0}, r{1}, {2}", a, b, c);
            break;
        case ScriptOp::LoadI:
            FormatTo(*_output, " r{0}, {1}", a, instruction.SBx);
            break;
        case ScriptOp::LoadK:
            FormatTo(*_output, " r{0}, k{1}", a, instruction.Bx);
            break;
        case ScriptOp::LoadKN:
            FormatTo(*_output, " r{0}, k{1}, {2}", a, instruction.Bx, t0);
            break;
        case ScriptOp::Zero:
        case ScriptOp::Return:
        case ScriptOp::Yield:
            FormatTo(*_output, " r{0}, {1}", a, b);
            break;
        case ScriptOp::LoadString:
            FormatTo(*_output, " r{0}, \"{1}\"", a, Text(instruction.Bx));
            break;
        case ScriptOp::ConvChecked:
            FormatTo(*_output, " r{0}, r{1}, {2} -> {3}", a, b, GetScriptNumberName(static_cast<ScriptNumber>(c >> 4)),
                     GetScriptNumberName(static_cast<ScriptNumber>(c & 0xF)));
            break;
        case ScriptOp::Extract:
        case ScriptOp::Insert:
        case ScriptOp::LoadField:
        case ScriptOp::StoreField:
            FormatTo(*_output, " r{0}, r{1}, +{2} ", a, b, t0);
            Access(t1);
            break;
        case ScriptOp::ExtractDyn:
        case ScriptOp::InsertDyn:
            FormatTo(*_output, " r{0}, r{1}, r{2}, ", a, b, c);
            Access(t0);
            FormatTo(*_output, " x{0}", t1);
            break;
        case ScriptOp::RefStatic:
            FormatTo(*_output, " r{0}, static+{1}", a, instruction.Bx);
            break;
        case ScriptOp::RefOffset:
        case ScriptOp::RefField:
        case ScriptOp::LoadField32:
        case ScriptOp::LoadField64:
        case ScriptOp::StoreField32:
        case ScriptOp::StoreField64:
        case ScriptOp::StoreFieldRef:
            FormatTo(*_output, " r{0}, r{1}, +{2}", a, b, t0);
            break;
        case ScriptOp::AddI32K:
        case ScriptOp::MulI32K:
        case ScriptOp::AddI64K:
        case ScriptOp::AndI32K:
        case ScriptOp::ShlI32K:
        case ScriptOp::ShrI32K:
        case ScriptOp::ShrU32K:
            FormatTo(*_output, " r{0}, r{1}, {2}", a, b, static_cast<int32>(t0));
            break;
        case ScriptOp::AddF32K:
        case ScriptOp::MulF32K:
            FormatTo(*_output, " r{0}, r{1}, {2}", a, b, BitCast<float32>(t0));
            break;
        case ScriptOp::RefIndex:
        case ScriptOp::RefIndexClamp:
            FormatTo(*_output, " r{0}, r{1}, r{2}, stride {3} count {4}", a, b, c, t0, t1);
            break;
        case ScriptOp::Load:
        case ScriptOp::Store:
            FormatTo(*_output, " r{0}, r{1}, ", a, b);
            Access(t0);
            break;
        case ScriptOp::New:
        case ScriptOp::TypeOf:
            FormatTo(*_output, " r{0}, {1}", a, TypeName(instruction.Bx));
            break;
        case ScriptOp::NewArray:
        case ScriptOp::Box:
        case ScriptOp::Unbox:
        case ScriptOp::IsInstance:
        case ScriptOp::Cast:
        case ScriptOp::TryCast:
            FormatTo(*_output, " r{0}, r{1}, {2}", a, b, TypeName(t0));
            break;
        case ScriptOp::RefElem:
        case ScriptOp::LoadElem32:
        case ScriptOp::LoadElem64:
        case ScriptOp::StoreElem32:
        case ScriptOp::StoreElem64:
        case ScriptOp::StoreElemRef:
            FormatTo(*_output, " r{0}, r{1}, r{2}, ", a, b, c);
            Element(t0);
            break;
        case ScriptOp::LoadElem:
        case ScriptOp::StoreElem:
            FormatTo(*_output, " r{0}, r{1}, r{2}, ", a, b, c);
            Element(t0);
            _output->Append(' ');
            Access(t1);
            break;
        case ScriptOp::MakeDelegate:
            if (c == 2)
            {
                FormatTo(*_output, " r{0}, r{1}, {2}, slot {3}", a, b, TypeName(t0), t1);
            }
            else if (c == 3)
            {
                FormatTo(*_output, " r{0}, r{1}, {2}, site {3}", a, b, TypeName(t0), t1);
            }
            else
            {
                FormatTo(*_output, " r{0}, r{1}, {2} -> {3}", a, b, TypeName(t0), FunctionName(t1));
            }
            break;
        case ScriptOp::Jump:
            FormatTo(*_output, " -> {0:04}", next + instruction.SBx);
            break;
        case ScriptOp::Loop:
            // The header's Loop falls through; a back edge's jumps.
            if (instruction.SBx != 0)
            {
                FormatTo(*_output, " -> {0:04}", next + instruction.SBx);
            }
            break;
        case ScriptOp::JumpIf:
        case ScriptOp::JumpIfNot:
            FormatTo(*_output, " r{0} -> {1:04}", a, next + instruction.SBx);
            break;
        case ScriptOp::Switch:
            FormatTo(*_output, " r{0}, table {1}", a, instruction.Bx);
            if (instruction.Bx < function.Switches.Count())
            {
                const ScriptSwitchTable &table = function.Switches[instruction.Bx];
                for (const ScriptSwitchCase &entry : table.Cases)
                {
                    FormatTo(*_output, " [{0} -> {1:04}]", entry.Value, entry.Target);
                }
                FormatTo(*_output, " [default -> {0:04}]", table.Default);
            }
            break;
        case ScriptOp::Fault:
            FormatTo(*_output, " {0}", GetScriptFaultName(static_cast<ScriptFault>(a)));
            if (instruction.Bx != 0)
            {
                FormatTo(*_output, ", \"{0}\"", Text(instruction.Bx - 1));
            }
            break;
        case ScriptOp::Call:
            FormatTo(*_output, " r{0}, {1}", a, FunctionName(instruction.Bx));
            break;
        case ScriptOp::CallVirtual:
            FormatTo(*_output, " r{0}, slot {1}", a, instruction.Bx);
            break;
        case ScriptOp::CallInterface:
            FormatTo(*_output, " r{0}, site {1}", a, instruction.Bx);
            if (instruction.Bx < function.InterfaceSites.Count())
            {
                const ScriptInterfaceSite &site = function.InterfaceSites[instruction.Bx];
                FormatTo(*_output, " ({0}.{1})", TypeName(site.Interface), site.Method);
            }
            break;
        case ScriptOp::CallDelegate:
            FormatTo(*_output, " r{0}, {1}{2}", a, b, c != 0 ? StringView(", event") : StringView());
            break;
        case ScriptOp::Math:
            FormatTo(*_output, " {0}.{1}x{2} r{3}, r{4}, r{5}, r{6}",
                     GetScriptMathOpName(static_cast<ScriptMathOp>(t0 & 0xFF)),
                     GetScriptNumberName(static_cast<ScriptNumber>((t0 >> 8) & 0xF)), (t0 >> 12) & 0xF, a, b, c,
                     t0 >> 16);
            break;
        default:
            if (int64 target = 0; IsScriptCompareJump(instruction.Op) && GetScriptJumpTarget(instruction, pc, target))
            {
                if (info.Operands == ScriptOperands::A)
                {
                    FormatTo(*_output, " r{0}, {1} -> {2:04}", a, static_cast<int32>(t0), target);
                }
                else
                {
                    FormatTo(*_output, " r{0}, r{1} -> {2:04}", a, b, target);
                }
                break;
            }
            switch (info.Operands)
            {
            case ScriptOperands::A:
                FormatTo(*_output, " r{0}", a);
                break;
            case ScriptOperands::AB:
                FormatTo(*_output, " r{0}, r{1}", a, b);
                break;
            case ScriptOperands::ABC:
                FormatTo(*_output, " r{0}, r{1}, r{2}", a, b, c);
                break;
            case ScriptOperands::ABx:
                FormatTo(*_output, " r{0}, {1}", a, instruction.Bx);
                break;
            case ScriptOperands::AsBx:
                FormatTo(*_output, " r{0}, {1}", a, instruction.SBx);
                break;
            case ScriptOperands::sAx:
                FormatTo(*_output, " {0}", instruction.SBx);
                break;
            case ScriptOperands::None:
                break;
            }
            break;
        }
    }

    void Function(uint32 index)
    {
        const ScriptFunctionRecord &function = _module->Functions[index];
        StringBuilder<64> flags;
        switch (function.Kind)
        {
        case ScriptFunctionKind::Async:
            flags.Append(" async");
            break;
        case ScriptFunctionKind::Iterator:
            flags.Append(" iterator");
            break;
        case ScriptFunctionKind::Kernel:
            flags.Append(" kernel");
            break;
        case ScriptFunctionKind::Normal:
            break;
        }
        if ((function.Flags & ScriptFunctionFlags::Common) != 0)
        {
            flags.Append(" common");
        }
        if ((function.Flags & ScriptFunctionFlags::Imported) != 0)
        {
            flags.Append(" imported");
        }
        if ((function.Flags & ScriptFunctionFlags::StaticInitializer) != 0)
        {
            flags.Append(" static-init");
        }
        if ((function.Flags & ScriptFunctionFlags::Test) != 0)
        {
            flags.Append(" test");
        }
        FormatTo(*_output, "fn #{0} {1}{2}", index, Text(function.Name), flags.AsView());
        if ((function.Flags & ScriptFunctionFlags::Native) != 0)
        {
            FormatTo(*_output, " native(\"{0}\")", Text(function.NativeName));
        }
        _output->Append(' ');
        Shape(function.Signature);
        if (function.Code.IsEmpty())
        {
            if ((function.Flags & (ScriptFunctionFlags::Native | ScriptFunctionFlags::Imported)) == 0)
            {
                _output->Append(" abstract");
            }
            _output->Append('\n');
            return;
        }
        FormatTo(*_output, " params {0} frame {1} zeroed {2} returns {3}\n", function.ParameterSlots,
                 function.FrameSlots, function.ZeroSlots, function.ReturnSlots);
        for (usize i = 0; i < function.Constants.Count(); ++i)
        {
            FormatTo(*_output, "  k{0} =", i);
            for (const uint8 byte : function.Constants[i].Bytes)
            {
                FormatTo(*_output, " {0:X2}", byte);
            }
            _output->Append('\n');
        }
        List<ScriptLineEntry> lines;
        const bool linesOk = DecodeScriptLineTable(function.LineTable.AsSpan(), lines);
        if (!linesOk)
        {
            _output->Append("  malformed line table\n");
        }
        usize line = 0;
        usize safepoint = 0;
        const Span<const uint32> code = function.Code.AsSpan();
        uint32 pc = 0;
        while (pc < code.Count())
        {
            // RECOVERY-DECISION SA3: a line-table row is shown as `  ; file:line:column` before the first instruction
            // it covers.
            while (linesOk && line < lines.Count() && lines[line].Pc <= pc)
            {
                const ScriptLineEntry &entry = lines[line++];
                const StringView path = entry.File < _module->Files.Count() ? Text(_module->Files[entry.File].Path)
                                                                            : StringView("?");
                FormatTo(*_output, "  ; {0}:{1}:{2}\n", path, entry.Line, entry.Column);
            }
            ScriptDecodedInstruction instruction;
            if (!DecodeScriptInstruction(code, pc, instruction))
            {
                FormatTo(*_output, "  {0:04} malformed {1:X8}\n", pc, code[pc]);
                ++pc;
                continue;
            }
            Instruction(function, pc, instruction);
            while (safepoint < function.Safepoints.Count() && function.Safepoints[safepoint].Pc < pc)
            {
                ++safepoint;
            }
            if (safepoint < function.Safepoints.Count() && function.Safepoints[safepoint].Pc == pc)
            {
                _output->Append("  ; gc");
                SafepointMap(function, function.Safepoints[safepoint].Map);
            }
            _output->Append('\n');
            pc += instruction.Words;
        }
    }

    void SafepointMap(const ScriptFunctionRecord &function, uint32 map)
    {
        const usize start = static_cast<usize>(map) * function.MapBytes * 2;
        if (function.MapBytes == 0 || start + function.MapBytes * 2 > function.Maps.Count())
        {
            return;
        }
        for (uint32 kind = 0; kind < 2; ++kind)
        {
            for (uint32 bit = 0; bit < function.MapBytes * 8; ++bit)
            {
                if ((function.Maps[start + kind * function.MapBytes + bit / 8] & (1u << (bit % 8))) != 0)
                {
                    // Each bit is an 8-byte unit; two per register.
                    FormatTo(*_output, " {0}r{1}{2}", kind == 0 ? StringView() : StringView("&"), bit / 2,
                             bit % 2 == 0 ? StringView() : StringView("+8"));
                }
            }
        }
    }

    const ScriptModuleData *_module;
    StringWriter *_output;
};
} // namespace

void DisassembleScriptModule(const ScriptModuleData &module, StringWriter &output)
{
    Disassembler(module, output).Run();
}
} // namespace Marlin
