#include "ScriptModuleBuilder.h"

#include "Hash/Xxh3.h"
#include "ScriptRegisters.h"
#include "String/Format.h"
#include "String/StringBuilder.h"

namespace Marlin
{
namespace
{
ScriptTypeKind KindOf(MirScriptTypeKind kind)
{
    switch (kind)
    {
    case MirScriptTypeKind::Class:
        return ScriptTypeKind::Class;
    case MirScriptTypeKind::Interface:
        return ScriptTypeKind::Interface;
    case MirScriptTypeKind::Delegate:
        return ScriptTypeKind::Delegate;
    case MirScriptTypeKind::Array:
        return ScriptTypeKind::Array;
    case MirScriptTypeKind::String:
        return ScriptTypeKind::String;
    case MirScriptTypeKind::Value:
        return ScriptTypeKind::Value;
    }
    return ScriptTypeKind::Value;
}

ScriptFunctionKind KindOf(MirFunctionKind kind)
{
    switch (kind)
    {
    case MirFunctionKind::Normal:
        return ScriptFunctionKind::Normal;
    case MirFunctionKind::Async:
        return ScriptFunctionKind::Async;
    case MirFunctionKind::Iterator:
        return ScriptFunctionKind::Iterator;
    }
    return ScriptFunctionKind::Normal;
}

void AppendText(List<uint8> &bytes, StringView text)
{
    for (const char c : text)
    {
        bytes.Add(static_cast<uint8>(c));
    }
    bytes.Add(0);
}

void AppendHash(List<uint8> &bytes, uint64 hash)
{
    for (uint32 i = 0; i < 8; ++i)
    {
        bytes.Add(static_cast<uint8>(hash >> (i * 8)));
    }
}
} // namespace

ScriptModuleBuilder::ScriptModuleBuilder(MirModule &module, const MirTargetOptions &options, ScriptModuleData &data,
                                         List<String> &errors)
    : _module(&module), _options(&options), _data(&data), _errors(&errors)
{
    for (MirFunction *function : module.GetFunctions())
    {
        _functions.Add(function, static_cast<uint32>(_functions.Count()));
    }
}

bool ScriptModuleBuilder::Build()
{
    *_data = {};
    _data->Package = AddString(_module->GetName());
    const Span<MirScriptType *const> scriptTypes = _module->GetScriptTypes();
    for (usize i = 0; i < scriptTypes.Count(); ++i)
    {
        if (scriptTypes[i]->Reference != nullptr && !_referenceTypes.Contains(scriptTypes[i]->Reference))
        {
            _referenceTypes.Add(scriptTypes[i]->Reference, static_cast<uint32>(i));
        }
    }
    AddTypes();
    AddStatics();
    const Span<MirFunction *const> functions = _module->GetFunctions();
    _data->Functions.Resize(functions.Count());
    for (usize i = 0; i < functions.Count(); ++i)
    {
        MirFunction &function = *functions[i];
        ScriptFunctionRecord &record = _data->Functions[i];
        record.Name = AddString(function.GetName());
        record.Kind = KindOf(function.Kind);
        record.Signature = ShapeOf(function.GetType());
        record.ReadOnlyParameters = function.ReadOnlyArguments;
        uint8 flags = 0;
        flags |= function.Common ? ScriptFunctionFlags::Common : 0;
        flags |= function.Imported ? ScriptFunctionFlags::Imported : 0;
        flags |= function.Test ? ScriptFunctionFlags::Test : 0;
        if (!function.NativeName.IsEmpty())
        {
            flags |= ScriptFunctionFlags::Native;
            record.NativeName = AddString(function.NativeName);
        }
        for (const MirFunction *initializer : _module->GetStaticInitializers())
        {
            flags |= initializer == &function ? ScriptFunctionFlags::StaticInitializer : 0;
        }
        record.Flags = flags;
        for (const MirArgument *argument : function.GetArguments())
        {
            record.ParameterSlots += ScriptSlotsOf(argument->GetType());
        }
        record.ReturnSlots = ScriptSlotsOf(function.GetReturnType());
        if (function.Kind == MirFunctionKind::Async && function.ResultType != nullptr &&
            !function.ResultType->Is(MirTypeKind::Void))
        {
            record.ResultShape = ShapeOf(function.ResultType);
        }
        else if (function.Kind == MirFunctionKind::Iterator)
        {
            for (const MirBlock *block : function.GetBlocks())
            {
                for (const MirInstruction *instruction : *block)
                {
                    if (instruction->GetOp() == MirOp::Yield && !instruction->GetOperands().IsEmpty() &&
                        record.ResultShape == ~0u)
                    {
                        record.ResultShape = ShapeOf(instruction->GetOperand(0)->GetType());
                    }
                }
            }
        }
        record.FrameSlots = record.ParameterSlots;
        const bool hasBody = function.GetEntry() != nullptr && !function.Imported && function.NativeName.IsEmpty();
        if (hasBody)
        {
            CompileScriptFunction(*this, function, record);
            if (_options->DebugInfo && !record.LineTable.IsEmpty())
            {
                record.Flags |= ScriptFunctionFlags::Debuggable;
            }
        }
    }
    for (const MirFunction *initializer : _module->GetStaticInitializers())
    {
        _data->StaticInitializers.Add(FunctionIndexOf(initializer));
    }
    for (const MirAttribute *attribute : _module->GetAttributes())
    {
        ScriptAttribute &record = _data->Attributes.AddDefaulted();
        record.Target = attribute->Function != nullptr ? ScriptAttributeTarget::Function : ScriptAttributeTarget::Shape;
        record.Index = attribute->Function != nullptr ? FunctionIndexOf(attribute->Function) : ShapeOf(attribute->Type);
        record.Name = AddString(attribute->Name);
        for (const MirAttributeArgument &argument : attribute->Arguments)
        {
            record.Arguments.Add({AddString(argument.Name), AddString(argument.Value)});
        }
    }
    AddLinkage();
    return _ok;
}

uint32 ScriptModuleBuilder::AddString(StringView text)
{
    if (const uint32 *found = _strings.Find(text))
    {
        return *found;
    }
    const uint32 index = static_cast<uint32>(_data->Strings.Count());
    _data->Strings.Add(String(text));
    _strings.Add(String(text), index);
    return index;
}

uint32 ScriptModuleBuilder::MirString(uint32 index)
{
    const Span<const StringView> strings = _module->GetStrings();
    return AddString(index < strings.Count() ? strings[index] : StringView());
}

uint32 ScriptModuleBuilder::ShapeOf(const MirType *type)
{
    if (const uint32 *found = _shapes.Find(type))
    {
        return *found;
    }
    ScriptShape shape;
    shape.Size = type->GetSize();
    shape.Alignment = type->GetAlignment();
    shape.Width = static_cast<uint8>(type->GetWidth());
    shape.Signed = type->IsSigned();
    switch (type->GetKind())
    {
    case MirTypeKind::Void:
        shape.Kind = ScriptShapeKind::Void;
        break;
    case MirTypeKind::Bool:
        shape.Kind = ScriptShapeKind::Bool;
        break;
    case MirTypeKind::Int:
        shape.Kind = ScriptShapeKind::Int;
        break;
    case MirTypeKind::Float:
        shape.Kind = ScriptShapeKind::Float;
        break;
    case MirTypeKind::Vector:
        shape.Kind = ScriptShapeKind::Vector;
        shape.Count = type->GetCount();
        shape.Element = ShapeOf(type->GetElement());
        break;
    case MirTypeKind::Matrix:
        shape.Kind = ScriptShapeKind::Matrix;
        shape.Count = type->GetCount();
        shape.Rows = type->GetRows();
        shape.Stride = type->GetStride();
        shape.Element = ShapeOf(type->GetElement());
        break;
    case MirTypeKind::Array:
        shape.Kind = ScriptShapeKind::Array;
        shape.Count = type->GetCount();
        shape.Stride = type->GetStride();
        shape.Element = ShapeOf(type->GetElement());
        break;
    case MirTypeKind::Struct:
        shape.Kind = ScriptShapeKind::Struct;
        shape.Name = AddString(type->GetName());
        for (const MirStructField &field : type->GetFields())
        {
            const uint32 fieldShape = ShapeOf(field.Type);
            shape.Fields.Add({AddString(field.Name), field.Offset, fieldShape});
        }
        break;
    case MirTypeKind::Reference: {
        shape.Kind = ScriptShapeKind::Reference;
        shape.Size = 8;
        shape.Alignment = 8;
        const uint32 *scriptType = _referenceTypes.Find(type);
        shape.Element = scriptType != nullptr ? *scriptType : ~0u;
        const MirType *contents = type->GetElement();
        if (contents != nullptr && !contents->GetName().IsEmpty())
        {
            shape.Name = AddString(contents->GetName());
        }
        else if (contents != nullptr)
        {
            StringBuilder<128> name;
            contents->Print(name);
            shape.Name = AddString(name.AsView());
        }
        break;
    }
    case MirTypeKind::Pointer:
        shape.Kind = ScriptShapeKind::Pointer;
        shape.Size = 8;
        shape.Alignment = 8;
        shape.Space = type->GetAddressSpace() == MirAddressSpace::Managed  ? ScriptPointerSpace::Managed
                      : type->GetAddressSpace() == MirAddressSpace::Static ? ScriptPointerSpace::Static
                                                                           : ScriptPointerSpace::Function;
        shape.Element = ShapeOf(type->GetElement());
        break;
    case MirTypeKind::Function:
        shape.Kind = ScriptShapeKind::Function;
        shape.Element = ShapeOf(type->GetElement());
        for (const MirType *parameter : type->GetParameters())
        {
            const uint32 parameterShape = ShapeOf(parameter);
            shape.Parameters.Add(parameterShape);
        }
        break;
    default: {
        StringBuilder<128> name;
        type->Print(name);
        Error(Format("the script target has no values of type `{0}`", name.AsView()));
        break;
    }
    }
    const uint32 index = static_cast<uint32>(_data->Shapes.Count());
    _data->Shapes.Add(Move(shape));
    _shapes.Add(type, index);
    return index;
}

uint32 ScriptModuleBuilder::FunctionIndexOf(const MirFunction *function) const
{
    const uint32 *found = _functions.Find(function);
    return found != nullptr ? *found : ~0u;
}

uint32 ScriptModuleBuilder::StaticOffsetOf(const MirGlobal *global) const
{
    const uint32 *found = _statics.Find(global);
    return found != nullptr ? *found : ~0u;
}

void ScriptModuleBuilder::Error(String message)
{
    _errors->Add(Move(message));
    _ok = false;
}

void ScriptModuleBuilder::AddTypes()
{
    for (const MirScriptType *type : _module->GetScriptTypes())
    {
        ScriptTypeRecord record;
        record.Kind = KindOf(type->Kind);
        record.Flags = static_cast<uint8>((type->Abstract ? ScriptTypeFlags::Abstract : 0) |
                                          (type->Sealed ? ScriptTypeFlags::Sealed : 0) |
                                          (type->Imported ? ScriptTypeFlags::Imported : 0));
        record.Name = AddString(type->Name);
        record.Base = type->Base;
        if (type->Layout != nullptr)
        {
            record.Layout = ShapeOf(type->Layout);
        }
        for (const MirFunction *slot : type->VirtualSlots)
        {
            record.VirtualSlots.Add(FunctionIndexOf(slot));
        }
        for (const MirInterfaceImpl &implementation : type->Interfaces)
        {
            ScriptInterfaceTable &table = record.Interfaces.AddDefaulted();
            table.Interface = implementation.Interface;
            for (const MirFunction *method : implementation.Methods)
            {
                table.Methods.Add(FunctionIndexOf(method));
            }
        }
        for (const uint32 inherited : type->Implements)
        {
            record.Implements.Add(inherited);
        }
        _data->Types.Add(Move(record));
    }
}

void ScriptModuleBuilder::AddStatics()
{
    uint32 size = 0;
    uint32 alignment = 1;
    for (const MirGlobal *global : _module->GetGlobals())
    {
        if (global->GetAddressSpace() != MirAddressSpace::Static)
        {
            Error(Format("the script target has no module variables but statics, and `{0}` is not one",
                         global->GetName()));
            continue;
        }
        const MirType *type = global->GetValueType();
        const uint32 align = type->GetAlignment() == 0 ? 1 : type->GetAlignment();
        size = (size + align - 1) / align * align;
        _statics.Add(global, size);
        _data->Statics.Add({AddString(global->GetName()), size, ShapeOf(type)});
        size += type->GetSize();
        alignment = align > alignment ? align : alignment;
    }
    _data->StaticSize = (size + alignment - 1) / alignment * alignment;
    _data->StaticAlignment = alignment;
}

uint64 ScriptModuleBuilder::SignatureHash(const MirType *type) const
{
    StringBuilder<256> text;
    type->Print(text);
    return Xxh3Hash64(text.AsView().Data(), text.AsView().Length());
}

void ScriptModuleBuilder::AddLinkage()
{
    const Span<MirFunction *const> functions = _module->GetFunctions();
    for (usize i = 0; i < functions.Count(); ++i)
    {
        const MirFunction &function = *functions[i];
        const uint32 index = static_cast<uint32>(i);
        if (!function.NativeName.IsEmpty())
        {
            _data->Imports.Add({ScriptImportKind::Native, index, _data->Functions[i].NativeName, 0});
        }
        else if (function.Imported)
        {
            _data->Imports.Add(
                {ScriptImportKind::Function, index, _data->Functions[i].Name, SignatureHash(function.GetType())});
        }
        else if (function.GetEntry() != nullptr)
        {
            _data->Exports.Add({ScriptExportKind::Function, index, SignatureHash(function.GetType())});
        }
    }
    const Span<MirScriptType *const> types = _module->GetScriptTypes();
    for (usize i = 0; i < types.Count(); ++i)
    {
        const uint32 index = static_cast<uint32>(i);
        if (types[i]->Imported)
        {
            _data->Imports.Add({ScriptImportKind::Type, index, _data->Types[i].Name, 0});
        }
        else if (types[i]->Kind == MirScriptTypeKind::Class || types[i]->Kind == MirScriptTypeKind::Interface)
        {
            _data->Exports.Add({ScriptExportKind::Type, index, 0});
        }
    }
    const Span<const StringView> files = _module->GetFiles();
    const Span<const StringView> texts = _module->GetFileTexts();
    for (usize i = 0; i < files.Count(); ++i)
    {
        const StringView text = i < texts.Count() ? texts[i] : StringView();
        _data->Files.Add({AddString(files[i]), Xxh3Hash64(text.Data(), text.Length())});
    }
    // RECOVERY-DECISION S5: the interface hash covers the name and signature hash of every export, in table order.
    List<uint8> bytes;
    for (const ScriptExport &entry : _data->Exports)
    {
        const uint32 name =
            entry.Kind == ScriptExportKind::Function ? _data->Functions[entry.Index].Name : _data->Types[entry.Index].Name;
        AppendText(bytes, _data->Strings[name].AsView());
        AppendHash(bytes, entry.Hash);
    }
    _data->InterfaceHash = Xxh3Hash64(bytes.Data(), bytes.Count());
}
} // namespace Marlin
