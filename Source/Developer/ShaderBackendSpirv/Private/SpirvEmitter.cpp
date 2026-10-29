#include "ShaderBackendSpirv/SpirvTarget.h"
#include "SpirvEmitter.h"

#include "SpirvModuleBuilder.h"

#include "Containers/HashMap.h"
#include "Containers/HashSet.h"
#include "LanguageIR/MirAnalysis.h"
#include "LanguageIR/MirPrinter.h"
#include "Math/Float16.h"
#include "String/Format.h"
#include "Templates/Bit.h"
#include "spirv/unified1/GLSL.std.450.h"
#include "spirv/unified1/NonSemanticShaderDebugInfo100.h"

namespace Marlin
{
namespace
{
using spv::Op;

constexpr uint32 Bits(spv::MemorySemanticsMask mask)
{
    return static_cast<uint32>(mask);
}

struct LayoutKey
{
    const MirType *Type;
    bool Layout;

    [[nodiscard]] uint64 GetHash() const
    {
        return HashCombine(HashInteger(reinterpret_cast<uintptr>(Type)), Layout ? 1 : 0);
    }

    bool operator==(const LayoutKey &) const = default;
};

struct PointerKey
{
    const MirType *Pointee;
    uint32 StorageClass;
    bool Layout;

    [[nodiscard]] uint64 GetHash() const
    {
        return HashCombine(HashCombine(HashInteger(reinterpret_cast<uintptr>(Pointee)), StorageClass), Layout ? 1 : 0);
    }

    bool operator==(const PointerKey &) const = default;
};

class SpirvWriter
{
public:
    SpirvWriter(MirModule &module, const MirTargetOptions &options, List<String> &errors)
        : _module(&module), _errors(&errors), _spirv(options.DebugInfo)
    {
    }

    bool Write(List<uint32> &words)
    {
        _spirv.Capability(spv::Capability::Shader);
        for (const StringView text : _module->GetFileTexts())
        {
            _sourceDebugInfo = _sourceDebugInfo || (_spirv.HasDebugInfo() && !text.IsEmpty());
        }
        for (const MirEntryPoint *entry : _module->GetEntryPoints())
        {
            _entryStages.Add(entry->Function, entry->Stage);
        }
        for (const MirFunction *function : _module->GetFunctions())
        {
            PreassignIds(*function);
        }
        for (const MirFunction *function : _module->GetFunctions())
        {
            const MirStage *stage = _entryStages.Find(function);
            _stage = stage != nullptr ? *stage : MirStage::Compute;
            EmitFunction(*function);
        }
        for (const MirEntryPoint *entry : _module->GetEntryPoints())
        {
            EmitEntryPoint(*entry);
        }
        if (!_errors->IsEmpty())
        {
            return false;
        }

        _spirv.Assemble(words);
        return true;
    }

private:
    template <typename... TArgs> void Error(FormatString<TypeIdentity<TArgs>...> format, const TArgs &...args)
    {
        _errors->Add(Format(format, args...));
    }

    // Types

    void NoteWidth(const MirType *scalar)
    {
        if (scalar->Is(MirTypeKind::Float))
        {
            if (scalar->GetWidth() == 16)
            {
                _spirv.Capability(spv::Capability::Float16);
            }
            else if (scalar->GetWidth() == 64)
            {
                _spirv.Capability(spv::Capability::Float64);
            }
        }
        else if (scalar->Is(MirTypeKind::Int))
        {
            if (scalar->GetWidth() == 8)
            {
                _spirv.Capability(spv::Capability::Int8);
            }
            else if (scalar->GetWidth() == 16)
            {
                _spirv.Capability(spv::Capability::Int16);
            }
            else if (scalar->GetWidth() == 64)
            {
                _spirv.Capability(spv::Capability::Int64);
            }
        }
    }

    static bool Contains16Bit(const MirType *type)
    {
        switch (type->GetKind())
        {
        case MirTypeKind::Int:
        case MirTypeKind::Float:
            return type->GetWidth() == 16;
        case MirTypeKind::Vector:
        case MirTypeKind::Matrix:
        case MirTypeKind::Array:
        case MirTypeKind::RuntimeArray:
            return Contains16Bit(type->GetScalar() != nullptr ? type->GetScalar() : type->GetElement());
        case MirTypeKind::Struct:
            for (const MirStructField &field : type->GetFields())
            {
                if (Contains16Bit(field.Type))
                {
                    return true;
                }
            }
            return false;
        default:
            return false;
        }
    }

    /// Whether *type* differs between its explicit-layout form for buffers and its plain form.
    static bool HasLayoutForm(const MirType *type)
    {
        return type->Is(MirTypeKind::Struct) || type->Is(MirTypeKind::Array) || type->Is(MirTypeKind::RuntimeArray);
    }

    uint32 Type(const MirType *type, bool layout = false)
    {
        if (type->IsHandle())
        {
            // A buffer is a 64-bit device address (§4.5); the other handles are 32-bit heap indices (§4.4).
            type = _module->GetTypes().GetInt(type->Is(MirTypeKind::Buffer) ? 64 : 32, false);
        }
        layout = layout && HasLayoutForm(type);
        const LayoutKey key{type, layout};
        if (const uint32 *found = _types.Find(key))
        {
            return *found;
        }
        uint32 id = 0;
        switch (type->GetKind())
        {
        case MirTypeKind::Void:
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeVoid).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::Bool:
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeBool).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::Int:
            NoteWidth(type);
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeInt).Add(id).Add(type->GetWidth()).Add(type->IsSigned() ? 1u : 0u).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::Float:
            NoteWidth(type);
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeFloat).Add(id).Add(type->GetWidth()).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::Vector:
        {
            const uint32 element = Type(type->GetElement());
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeVector).Add(id).Add(element).Add(type->GetCount()).WriteTo(_spirv.Globals);
            break;
        }
        case MirTypeKind::Matrix:
        {
            const uint32 row = Type(type->GetElement());
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeMatrix).Add(id).Add(row).Add(type->GetRows()).WriteTo(_spirv.Globals);
            break;
        }
        case MirTypeKind::Array:
        {
            const uint32 element = Type(type->GetElement(), layout);
            const uint32 length = Constant(_module->GetTypes().GetInt(32, false), type->GetCount());
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeArray).Add(id).Add(element).Add(length).WriteTo(_spirv.Globals);
            if (layout)
            {
                const uint32 stride[] = {type->GetStride()};
                _spirv.Decorate(id, spv::Decoration::ArrayStride, stride);
            }
            break;
        }
        case MirTypeKind::RuntimeArray:
        {
            const uint32 element = Type(type->GetElement(), true);
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeRuntimeArray).Add(id).Add(element).WriteTo(_spirv.Globals);
            const uint32 stride[] = {type->GetStride()};
            _spirv.Decorate(id, spv::Decoration::ArrayStride, stride);
            break;
        }
        case MirTypeKind::Struct:
        {
            List<uint32> members;
            for (const MirStructField &field : type->GetFields())
            {
                members.Add(Type(field.Type, layout));
            }
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeStruct).Add(id).Add(members.AsSpan()).WriteTo(_spirv.Globals);
            _spirv.Name(id, type->GetName());
            for (usize i = 0; i < type->GetFields().Count(); ++i)
            {
                const MirStructField &field = type->GetFields()[i];
                _spirv.MemberName(id, static_cast<uint32>(i), field.Name);
                if (layout)
                {
                    DecorateMemberLayout(id, static_cast<uint32>(i), field.Type, field.Offset);
                }
            }
            break;
        }
        case MirTypeKind::Function:
        {
            const uint32 result = Type(type->GetElement());
            List<uint32> parameters;
            for (const MirType *parameter : type->GetParameters())
            {
                parameters.Add(Type(parameter));
            }
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeFunction).Add(id).Add(result).Add(parameters.AsSpan()).WriteTo(_spirv.Globals);
            break;
        }
        case MirTypeKind::Pointer:
            if (type->GetAddressSpace() == MirAddressSpace::Managed || type->GetAddressSpace() == MirAddressSpace::Static)
            {
                Error("the type `{0}` has no SPIR-V form", TypeText(type).AsView());
            }
            id = Pointer(type->GetElement(), StorageClassOf(type->GetAddressSpace()), false);
            break;
        case MirTypeKind::RayQuery:
            _spirv.Capability(spv::Capability::RayQueryKHR);
            _spirv.Extension("SPV_KHR_ray_query");
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeRayQueryKHR).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::HitObject:
            UseShaderInvocationReorder();
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeHitObjectEXT).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirTypeKind::CoopVector:
        {
            _spirv.Capability(spv::Capability::CooperativeVectorNV);
            _spirv.Extension("SPV_NV_cooperative_vector");
            const uint32 element = Type(type->GetElement());
            const uint32 count = UintConstant(type->GetCount());
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeCooperativeVectorNV).Add(id).Add(element).Add(count).WriteTo(_spirv.Globals);
            break;
        }
        case MirTypeKind::CoopMatrix:
        {
            _spirv.Capability(spv::Capability::CooperativeMatrixKHR);
            _spirv.Capability(spv::Capability::VulkanMemoryModel);
            _spirv.Capability(spv::Capability::VulkanMemoryModelDeviceScope);
            _spirv.Extension("SPV_KHR_cooperative_matrix");
            const uint32 operands[] = {Type(type->GetElement()), UintConstant(static_cast<uint32>(spv::Scope::Subgroup)),
                                       UintConstant(type->GetRows()), UintConstant(type->GetCount()),
                                       UintConstant(static_cast<uint32>(type->GetMatrixUse()))};
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeCooperativeMatrixKHR).Add(id).Add(Span<const uint32>(operands)).WriteTo(_spirv.Globals);
            break;
        }
        default:
            Error("the type `{0}` has no SPIR-V form", TypeText(type).AsView());
            id = _spirv.NewId();
            SpirvInstruction(Op::OpTypeVoid).Add(id).WriteTo(_spirv.Globals);
            break;
        }
        _types.Add(key, id);
        return id;
    }

    void DecorateMemberLayout(uint32 structId, uint32 member, const MirType *type, uint32 offset)
    {
        const uint32 offsetWords[] = {offset};
        _spirv.MemberDecorate(structId, member, spv::Decoration::Offset, offsetWords);
        const MirType *matrix = type;
        while (matrix->Is(MirTypeKind::Array) || matrix->Is(MirTypeKind::RuntimeArray))
        {
            matrix = matrix->GetElement();
        }
        if (matrix->Is(MirTypeKind::Matrix))
        {
            _spirv.MemberDecorate(structId, member, spv::Decoration::ColMajor);
            const uint32 stride[] = {matrix->GetStride()};
            _spirv.MemberDecorate(structId, member, spv::Decoration::MatrixStride, stride);
        }
    }

    static String TypeText(const MirType *type)
    {
        StringBuilder<64> text;
        type->Print(text);
        return text.ToString();
    }

    static spv::StorageClass StorageClassOf(MirAddressSpace space)
    {
        switch (space)
        {
        case MirAddressSpace::Function:
            return spv::StorageClass::Function;
        case MirAddressSpace::Private:
            return spv::StorageClass::Private;
        case MirAddressSpace::GroupShared:
            return spv::StorageClass::Workgroup;
        case MirAddressSpace::Buffer:
            return spv::StorageClass::PhysicalStorageBuffer;
        case MirAddressSpace::Uniform:
            return spv::StorageClass::PushConstant;
        case MirAddressSpace::Input:
            return spv::StorageClass::Input;
        case MirAddressSpace::Output:
            return spv::StorageClass::Output;
        case MirAddressSpace::RayPayload:
            return spv::StorageClass::RayPayloadKHR;
        case MirAddressSpace::IncomingRayPayload:
            return spv::StorageClass::IncomingRayPayloadKHR;
        case MirAddressSpace::HitAttribute:
            return spv::StorageClass::HitAttributeKHR;
        case MirAddressSpace::CallableData:
            return spv::StorageClass::CallableDataKHR;
        case MirAddressSpace::IncomingCallableData:
            return spv::StorageClass::IncomingCallableDataKHR;
        case MirAddressSpace::ShaderRecord:
            return spv::StorageClass::ShaderRecordBufferKHR;
        case MirAddressSpace::TaskPayload:
            return spv::StorageClass::TaskPayloadWorkgroupEXT;
        case MirAddressSpace::Specialization:
            return spv::StorageClass::Private;
        case MirAddressSpace::Managed: // Script memory: the frontend never lowers it into a shader.
        case MirAddressSpace::Static:
            break;
        }
        return spv::StorageClass::Function;
    }

    uint32 Pointer(const MirType *pointee, spv::StorageClass storage, bool layout)
    {
        const PointerKey key{pointee, static_cast<uint32>(storage), layout};
        if (const uint32 *found = _pointers.Find(key))
        {
            return *found;
        }
        if (Contains16Bit(pointee))
        {
            switch (storage)
            {
            case spv::StorageClass::Input:
            case spv::StorageClass::Output:
                _spirv.Capability(spv::Capability::StorageInputOutput16);
                break;
            case spv::StorageClass::PushConstant:
                _spirv.Capability(spv::Capability::StoragePushConstant16);
                break;
            default:
                break;
            }
        }
        if (storage == spv::StorageClass::PhysicalStorageBuffer)
        {
            _spirv.Capability(spv::Capability::PhysicalStorageBufferAddresses);
        }
        const uint32 pointeeId = Type(pointee, layout);
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypePointer).Add(id).Add(storage).Add(pointeeId).WriteTo(_spirv.Globals);
        _pointers.Add(key, id);
        return id;
    }

    /// A Function-storage pointer to the SPIR-V type *pointee*, such as a variable holding a device pointer.
    uint32 FunctionPointer(uint32 pointee)
    {
        if (const uint32 *found = _functionPointers.Find(pointee))
        {
            return *found;
        }
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypePointer).Add(id).Add(spv::StorageClass::Function).Add(pointee).WriteTo(_spirv.Globals);
        _functionPointers.Add(pointee, id);
        return id;
    }

    /// A PhysicalStorageBuffer pointer to the SPIR-V type *pointee*: what a buffer's device address converts to (§11.3).
    uint32 PhysicalPointer(uint32 pointee)
    {
        if (const uint32 *found = _physicalPointers.Find(pointee))
        {
            return *found;
        }
        _spirv.Capability(spv::Capability::PhysicalStorageBufferAddresses);
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypePointer).Add(id).Add(spv::StorageClass::PhysicalStorageBuffer).Add(pointee).WriteTo(_spirv.Globals);
        _physicalPointers.Add(pointee, id);
        return id;
    }

    uint32 UntypedPointer(spv::StorageClass storage)
    {
        _spirv.Capability(spv::Capability::UntypedPointersKHR);
        _spirv.Extension("SPV_KHR_untyped_pointers");
        if (storage == spv::StorageClass::PhysicalStorageBuffer)
        {
            _spirv.Capability(spv::Capability::PhysicalStorageBufferAddresses);
        }
        const auto key = static_cast<uint32>(storage);
        if (const uint32 *found = _untypedPointers.Find(key))
        {
            return *found;
        }
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeUntypedPointerKHR).Add(id).Add(storage).WriteTo(_spirv.Globals);
        _untypedPointers.Add(key, id);
        return id;
    }

    void UseHeaps()
    {
        _spirv.Capability(spv::Capability::DescriptorHeapEXT);
        _spirv.Capability(spv::Capability::UntypedPointersKHR);
        _spirv.Extension("SPV_EXT_descriptor_heap");
        _spirv.Extension("SPV_KHR_untyped_pointers");
    }

    // Constants

    uint32 Constant(const MirType *type, uint64 value)
    {
        return ConstantValue(_module->GetInt(type, static_cast<int64>(value)));
    }

    uint32 UintConstant(uint32 value)
    {
        return Constant(_module->GetTypes().GetInt(32, false), value);
    }

    uint32 ConstantValue(const MirConstant *constant)
    {
        if (const uint32 *found = _constants.Find(constant))
        {
            return *found;
        }
        const MirType *type = constant->GetType();
        const uint32 typeId = Type(type);
        const uint32 id = _spirv.NewId();
        switch (constant->GetConstantKind())
        {
        case MirConstantKind::Null:
            SpirvInstruction(Op::OpConstantNull).Add(typeId).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirConstantKind::Undef:
            SpirvInstruction(Op::OpUndef).Add(typeId).Add(id).WriteTo(_spirv.Globals);
            break;
        case MirConstantKind::Composite:
        {
            List<uint32> elements;
            for (const MirConstant *element : constant->GetElements())
            {
                elements.Add(ConstantValue(element));
                if (type->Is(MirTypeKind::CoopMatrix))
                {
                    break; // A cooperative matrix is built from one value for all its elements.
                }
            }
            SpirvInstruction(Op::OpConstantComposite).Add(typeId).Add(id).Add(elements.AsSpan()).WriteTo(_spirv.Globals);
            break;
        }
        case MirConstantKind::Scalar:
            if (type->Is(MirTypeKind::Bool))
            {
                SpirvInstruction(constant->GetBits() != 0 ? Op::OpConstantTrue : Op::OpConstantFalse)
                    .Add(typeId)
                    .Add(id)
                    .WriteTo(_spirv.Globals);
            }
            else
            {
                SpirvInstruction instruction(Op::OpConstant);
                instruction.Add(typeId).Add(id);
                const uint32 width = type->Is(MirTypeKind::Buffer) ? 64 : type->IsHandle() ? 32 : type->GetWidth();
                const uint64 bits = constant->GetBits();
                instruction.Add(static_cast<uint32>(width < 32 ? bits & ((uint64{1} << width) - 1) : bits));
                if (width == 64)
                {
                    instruction.Add(static_cast<uint32>(bits >> 32));
                }
                instruction.WriteTo(_spirv.Globals);
            }
            break;
        }
        _constants.Add(constant, id);
        return id;
    }

    // Globals

    static spv::BuiltIn BuiltInOf(MirBuiltin builtin, MirStage stage, bool isInput, bool &supported)
    {
        supported = true;
        switch (builtin)
        {
        case MirBuiltin::Position:
            return stage == MirStage::Pixel && isInput ? spv::BuiltIn::FragCoord : spv::BuiltIn::Position;
        case MirBuiltin::VertexIndex:
            return spv::BuiltIn::VertexIndex;
        case MirBuiltin::InstanceIndex:
            return spv::BuiltIn::InstanceIndex;
        case MirBuiltin::DispatchThreadId:
            return spv::BuiltIn::GlobalInvocationId;
        case MirBuiltin::GroupThreadId:
            return spv::BuiltIn::LocalInvocationId;
        case MirBuiltin::GroupId:
            return spv::BuiltIn::WorkgroupId;
        case MirBuiltin::GroupIndex:
            return spv::BuiltIn::LocalInvocationIndex;
        case MirBuiltin::Depth:
            return spv::BuiltIn::FragDepth;
        case MirBuiltin::FrontFace:
            return spv::BuiltIn::FrontFacing;
        case MirBuiltin::SampleIndex:
            return spv::BuiltIn::SampleId;
        case MirBuiltin::PrimitiveId:
            return spv::BuiltIn::PrimitiveId;
        case MirBuiltin::ViewIndex:
            return spv::BuiltIn::ViewIndex;
        case MirBuiltin::RenderTargetIndex:
            return spv::BuiltIn::Layer;
        case MirBuiltin::ViewportIndex:
            return spv::BuiltIn::ViewportIndex;
        case MirBuiltin::WaveLaneIndex:
            return spv::BuiltIn::SubgroupLocalInvocationId;
        case MirBuiltin::WaveLaneCount:
            return spv::BuiltIn::SubgroupSize;
        case MirBuiltin::HelperLane:
            return spv::BuiltIn::HelperInvocation;
        case MirBuiltin::Barycentrics:
            return spv::BuiltIn::BaryCoordKHR;
        case MirBuiltin::Coverage:
            return spv::BuiltIn::SampleMask;
        case MirBuiltin::CullPrimitive:
            return spv::BuiltIn::CullPrimitiveEXT;
        case MirBuiltin::LaunchIndex:
            return spv::BuiltIn::LaunchIdKHR;
        case MirBuiltin::LaunchSize:
            return spv::BuiltIn::LaunchSizeKHR;
        case MirBuiltin::WorldRayOrigin:
            return spv::BuiltIn::WorldRayOriginKHR;
        case MirBuiltin::WorldRayDirection:
            return spv::BuiltIn::WorldRayDirectionKHR;
        case MirBuiltin::ObjectRayOrigin:
            return spv::BuiltIn::ObjectRayOriginKHR;
        case MirBuiltin::ObjectRayDirection:
            return spv::BuiltIn::ObjectRayDirectionKHR;
        case MirBuiltin::RayTMin:
            return spv::BuiltIn::RayTminKHR;
        case MirBuiltin::RayTCurrent:
            return spv::BuiltIn::RayTmaxKHR;
        case MirBuiltin::IncomingRayFlags:
            return spv::BuiltIn::IncomingRayFlagsKHR;
        case MirBuiltin::HitKind:
            return spv::BuiltIn::HitKindKHR;
        case MirBuiltin::InstanceIndexRt:
            return spv::BuiltIn::InstanceId;
        case MirBuiltin::InstanceId:
            return spv::BuiltIn::InstanceCustomIndexKHR;
        case MirBuiltin::GeometryIndex:
            return spv::BuiltIn::RayGeometryIndexKHR;
        case MirBuiltin::PrimitiveIndexRt:
            return spv::BuiltIn::PrimitiveId;
        case MirBuiltin::ObjectToWorld:
            return spv::BuiltIn::ObjectToWorldKHR;
        case MirBuiltin::WorldToObject:
            return spv::BuiltIn::WorldToObjectKHR;
        case MirBuiltin::PrimitiveTriangleIndices:
            return spv::BuiltIn::PrimitiveTriangleIndicesEXT;
        case MirBuiltin::PrimitiveLineIndices:
            return spv::BuiltIn::PrimitiveLineIndicesEXT;
        case MirBuiltin::PrimitivePointIndices:
            return spv::BuiltIn::PrimitivePointIndicesEXT;
        case MirBuiltin::ShadingRate:
            return stage == MirStage::Pixel ? spv::BuiltIn::ShadingRateKHR : spv::BuiltIn::PrimitiveShadingRateKHR;
        case MirBuiltin::PrimitiveShadingRate:
            return spv::BuiltIn::PrimitiveShadingRateKHR;
        default:
            supported = false;
            return spv::BuiltIn::Position;
        }
    }

    uint32 Global(const MirGlobal *global)
    {
        if (const uint32 *found = _values.Find(global))
        {
            return *found;
        }
        const MirAddressSpace space = global->GetAddressSpace();
        const spv::StorageClass storage = StorageClassOf(space);
        const uint32 id = _spirv.NewId();
        _values.Add(global, id);
        _spirv.Name(id, global->GetName());
        if (space == MirAddressSpace::Specialization)
        {
            const MirType *type = global->GetValueType();
            const uint64 bits = global->Initializer != nullptr ? global->Initializer->GetBits() : 0;
            if (type->Is(MirTypeKind::Bool))
            {
                SpirvInstruction(bits != 0 ? Op::OpSpecConstantTrue : Op::OpSpecConstantFalse)
                    .Add(Type(type))
                    .Add(id)
                    .WriteTo(_spirv.Globals);
            }
            else
            {
                SpirvInstruction instruction(Op::OpSpecConstant);
                instruction.Add(Type(type)).Add(id);
                const uint32 width = type->GetWidth();
                instruction.Add(static_cast<uint32>(width < 32 ? bits & ((uint64{1} << width) - 1) : bits));
                if (width == 64)
                {
                    instruction.Add(static_cast<uint32>(bits >> 32));
                }
                instruction.WriteTo(_spirv.Globals);
            }
            const uint32 specId[] = {global->Location};
            _spirv.Decorate(id, spv::Decoration::SpecId, specId);
            return id;
        }
        if (space == MirAddressSpace::Uniform || space == MirAddressSpace::ShaderRecord)
        {
            const uint32 block = ArgumentsBlock(global->GetValueType());
            const uint32 pointer = _spirv.NewId();
            SpirvInstruction(Op::OpTypePointer).Add(pointer).Add(storage).Add(block).WriteTo(_spirv.Globals);
            SpirvInstruction(Op::OpVariable).Add(pointer).Add(id).Add(storage).WriteTo(_spirv.Globals);
            if (space == MirAddressSpace::Uniform && Contains16Bit(global->GetValueType()))
            {
                _spirv.Capability(spv::Capability::StoragePushConstant16);
            }
            _argumentBlocks.Add(id);
            _usedGlobals.Add(id);
            return id;
        }
        const uint32 pointer = Pointer(DeclaredType(global), storage, false);
        SpirvInstruction instruction(Op::OpVariable);
        instruction.Add(pointer).Add(id).Add(storage);
        if (global->Initializer != nullptr)
        {
            instruction.Add(ConstantValue(global->Initializer));
        }
        instruction.WriteTo(_spirv.Globals);
        _usedGlobals.Add(id);
        const bool isInput = space == MirAddressSpace::Input;
        if (global->Builtin != MirBuiltin::None)
        {
            bool supported = false;
            const MirStage stage = _globalStages.Contains(global) ? *_globalStages.Find(global) : _stage;
            const spv::BuiltIn builtin = BuiltInOf(global->Builtin, stage, isInput, supported);
            if (!supported)
            {
                Error("the system value {0} has no SPIR-V form yet", GetMirBuiltinName(global->Builtin));
            }
            const uint32 builtinWord[] = {static_cast<uint32>(builtin)};
            _spirv.Decorate(id, spv::Decoration::BuiltIn, builtinWord);
            if (builtin == spv::BuiltIn::SubgroupLocalInvocationId || builtin == spv::BuiltIn::SubgroupSize)
            {
                _spirv.Capability(spv::Capability::GroupNonUniform);
            }
            if (builtin == spv::BuiltIn::ViewIndex)
            {
                _spirv.Capability(spv::Capability::MultiView);
            }
            if (builtin == spv::BuiltIn::BaryCoordKHR)
            {
                _spirv.Capability(spv::Capability::FragmentBarycentricKHR);
                _spirv.Extension("SPV_KHR_fragment_shader_barycentric");
            }
            if (builtin == spv::BuiltIn::FragDepth)
            {
                _writesDepth = true;
            }
            if (builtin == spv::BuiltIn::ShadingRateKHR || builtin == spv::BuiltIn::PrimitiveShadingRateKHR)
            {
                _spirv.Capability(spv::Capability::FragmentShadingRateKHR);
                _spirv.Extension("SPV_KHR_fragment_shading_rate");
            }
            if (stage == MirStage::Pixel && isInput &&
                (builtin == spv::BuiltIn::SampleId || builtin == spv::BuiltIn::PrimitiveId ||
                 builtin == spv::BuiltIn::ViewIndex || builtin == spv::BuiltIn::Layer))
            {
                _spirv.Decorate(id, spv::Decoration::Flat);
            }
        }
        if (global->PerPrimitive)
        {
            _spirv.Decorate(id, spv::Decoration::PerPrimitiveEXT);
            _spirv.Capability(spv::Capability::MeshShadingEXT);
            _spirv.Extension("SPV_EXT_mesh_shader");
        }
        if (global->Builtin == MirBuiltin::None && global->Location != ~0u)
        {
            const uint32 location[] = {global->Location};
            _spirv.Decorate(id, spv::Decoration::Location, location);
            const MirType *scalar = global->GetValueType()->GetScalar();
            const bool integral = scalar != nullptr && !scalar->Is(MirTypeKind::Float);
            if (global->Interpolation == MirInterpolation::NoInterpolation || (integral && isInput && _stage == MirStage::Pixel))
            {
                _spirv.Decorate(id, spv::Decoration::Flat);
            }
            else if (global->Interpolation == MirInterpolation::Linear)
            {
                _spirv.Decorate(id, spv::Decoration::NoPerspective);
            }
            if (global->Sampling == MirSampling::Centroid)
            {
                _spirv.Decorate(id, spv::Decoration::Centroid);
            }
            else if (global->Sampling == MirSampling::Sample)
            {
                _spirv.Capability(spv::Capability::SampleRateShading);
                _spirv.Decorate(id, spv::Decoration::Sample);
            }
        }
        return id;
    }

    /// The Block struct `{ T value; }` that holds the [Uniform] parameter block of type *type* (§9.3).
    uint32 ArgumentsBlock(const MirType *type)
    {
        if (const uint32 *found = _rootBlocks.Find(type))
        {
            return *found;
        }
        const uint32 member = Type(type, true);
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeStruct).Add(id).Add(member).WriteTo(_spirv.Globals);
        _spirv.Decorate(id, spv::Decoration::Block);
        DecorateMemberLayout(id, 0, type, 0);
        _spirv.Name(id, "Arguments");
        _rootBlocks.Add(type, id);
        return id;
    }

    /// A struct of a buffer as the buffer lays it out, with the members of nested structs pulled up into it at their
    /// offsets from its start: First holds the flattened member of each field's first leaf. NVIDIA's compiler (driver
    /// 610) reads and writes the members of a struct nested in a buffer element at the nested struct's own offsets,
    /// as if it began the element, through any access chain or whole-struct load; flat structs have no nesting.
    struct FlatStruct
    {
        uint32 Id = 0;
        List<uint32> First;
    };

    const FlatStruct &Flat(const MirType *type)
    {
        if (const UniquePtr<FlatStruct> *found = _flatStructs.Find(type))
        {
            return **found;
        }
        auto flat = MakeUnique<FlatStruct>();
        List<uint32> members;
        List<const MirType *> leafTypes;
        List<uint32> offsets;
        for (const MirStructField &field : type->GetFields())
        {
            flat->First.Add(static_cast<uint32>(members.Count()));
            CollectLeaves(field.Type, field.Offset, members, leafTypes, offsets);
        }
        flat->Id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeStruct).Add(flat->Id).Add(members.AsSpan()).WriteTo(_spirv.Globals);
        _spirv.Name(flat->Id, type->GetName());
        for (usize index = 0; index < members.Count(); ++index)
        {
            DecorateMemberLayout(flat->Id, static_cast<uint32>(index), leafTypes[index], offsets[index]);
        }
        const FlatStruct &result = *flat;
        _flatStructs.Add(type, Move(flat));
        return result;
    }

    void CollectLeaves(const MirType *type, uint32 offset, List<uint32> &members, List<const MirType *> &leafTypes,
                       List<uint32> &offsets)
    {
        if (type->Is(MirTypeKind::Struct))
        {
            for (const MirStructField &field : type->GetFields())
            {
                CollectLeaves(field.Type, offset + field.Offset, members, leafTypes, offsets);
            }
            return;
        }
        members.Add(BufferLayoutType(type));
        leafTypes.Add(type);
        offsets.Add(offset);
    }

    /// The type of *type* in a buffer: structs flattened (FlatStruct), arrays of them strided, the rest as is.
    uint32 BufferLayoutType(const MirType *type)
    {
        if (type->Is(MirTypeKind::Struct))
        {
            return Flat(type).Id;
        }
        if (!type->Is(MirTypeKind::Array) && !type->Is(MirTypeKind::RuntimeArray))
        {
            return Type(type);
        }
        const MirType *innermost = type->GetElement();
        while (innermost->Is(MirTypeKind::Array) || innermost->Is(MirTypeKind::RuntimeArray))
        {
            innermost = innermost->GetElement();
        }
        if (!innermost->Is(MirTypeKind::Struct))
        {
            // No struct inside: the one layout type, which typed access chains (cooperative types) also name.
            return Type(type, true);
        }
        if (const uint32 *found = _bufferArrays.Find(type))
        {
            return *found;
        }
        const uint32 element = BufferLayoutType(type->GetElement());
        const uint32 id = _spirv.NewId();
        if (type->Is(MirTypeKind::Array))
        {
            const uint32 length = Constant(_module->GetTypes().GetInt(32, false), type->GetCount());
            SpirvInstruction(Op::OpTypeArray).Add(id).Add(element).Add(length).WriteTo(_spirv.Globals);
        }
        else
        {
            SpirvInstruction(Op::OpTypeRuntimeArray).Add(id).Add(element).WriteTo(_spirv.Globals);
        }
        const uint32 stride[] = {type->GetStride()};
        _spirv.Decorate(id, spv::Decoration::ArrayStride, stride);
        _bufferArrays.Add(type, id);
        return id;
    }

    /// The Block struct `{ T data[]; }` a buffer pointer points to, by the runtime array type of MIR; a read-only
    /// pointer's (§4.5) has its member NonWritable.
    uint32 BufferBlock(const MirType *runtimeArray, bool readOnly = false)
    {
        HashMap<const MirType *, uint32> &blocks = readOnly ? _readOnlyBufferBlocks : _bufferBlocks;
        if (const uint32 *found = blocks.Find(runtimeArray))
        {
            return *found;
        }
        const uint32 member = BufferLayoutType(runtimeArray);
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeStruct).Add(id).Add(member).WriteTo(_spirv.Globals);
        _spirv.Decorate(id, spv::Decoration::Block);
        DecorateMemberLayout(id, 0, runtimeArray, 0);
        if (readOnly)
        {
            _spirv.MemberDecorate(id, 0, spv::Decoration::NonWritable);
        }
        blocks.Add(runtimeArray, id);
        if (Contains16Bit(runtimeArray))
        {
            _spirv.Capability(spv::Capability::StorageBuffer16BitAccess);
        }
        return id;
    }

    uint32 Heap(bool samplers)
    {
        uint32 &heap = samplers ? _samplerHeap : _resourceHeap;
        if (heap == 0)
        {
            UseHeaps();
            const uint32 pointer = UntypedPointer(spv::StorageClass::UniformConstant);
            heap = _spirv.NewId();
            SpirvInstruction(Op::OpUntypedVariableKHR).Add(pointer).Add(heap).Add(spv::StorageClass::UniformConstant).WriteTo(_spirv.Globals);
            const uint32 builtin[] = {static_cast<uint32>(samplers ? spv::BuiltIn::SamplerHeapEXT : spv::BuiltIn::ResourceHeapEXT)};
            _spirv.Decorate(heap, spv::Decoration::BuiltIn, builtin);
            _spirv.Name(heap, samplers ? StringView("SamplerHeap") : StringView("ResourceHeap"));
            _usedGlobals.Add(heap);
        }
        return heap;
    }

    /// The distance between descriptors in the resource or sampler heap: one stride per heap for every kind of
    /// descriptor, the device's, which the RHI gives each stage as a specialization constant (§11.3).
    uint32 HeapStride(bool samplers)
    {
        uint32 &stride = samplers ? _samplerStride : _resourceStride;
        if (stride == 0)
        {
            stride = _spirv.NewId();
            // RECOVERY-DECISION lang-spirv-D1: the default stride, used if the RHI gives none, is 64 bytes.
            SpirvInstruction(Op::OpSpecConstant).Add(Type(_module->GetTypes().GetInt(32, false))).Add(stride).Add(64u).WriteTo(_spirv.Globals);
            const uint32 specId[] = {samplers ? SpirvSamplerStrideSpecId : SpirvResourceStrideSpecId};
            _spirv.Decorate(stride, spv::Decoration::SpecId, specId);
        }
        return stride;
    }

    /// The runtime array of descriptors of type *descriptor* that the resource or sampler heap is indexed as.
    uint32 DescriptorArray(uint32 descriptor, bool samplers)
    {
        if (const uint32 *found = _descriptorArrays.Find(descriptor))
        {
            return *found;
        }
        const uint32 stride = HeapStride(samplers);
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeRuntimeArray).Add(id).Add(descriptor).WriteTo(_spirv.Globals);
        SpirvInstruction(Op::OpDecorateId).Add(id).Add(spv::Decoration::ArrayStrideIdEXT).Add(stride).WriteTo(_spirv.Annotations);
        _descriptorArrays.Add(descriptor, id);
        return id;
    }

    static spv::ImageFormat ImageFormatOf(MirImageFormat format)
    {
        using F = spv::ImageFormat;
        static constexpr F formats[] = {
            F::Unknown,  F::Rgba32f,     F::Rgba16f,   F::Rg32f,     F::Rg16f,    F::R11fG11fB10f, F::R32f,
            F::R16f,     F::Rgba16,      F::Rgb10A2,   F::Rgba8,     F::Rg16,     F::Rg8,          F::R16,
            F::R8,       F::Rgba16Snorm, F::Rgba8Snorm, F::Rg16Snorm, F::Rg8Snorm, F::R16Snorm,     F::R8Snorm,
            F::Rgba32i,  F::Rgba16i,     F::Rgba8i,    F::Rg32i,     F::Rg16i,    F::Rg8i,         F::R32i,
            F::R16i,     F::R8i,         F::R64i,      F::Rgba32ui,  F::Rgba16ui, F::Rgba8ui,      F::Rg32ui,
            F::Rg16ui,   F::Rg8ui,       F::R32ui,     F::R16ui,     F::R8ui,     F::R64ui,
        };
        return formats[static_cast<usize>(format)];
    }

    /// Whether *format* is usable with the Shader capability alone, without StorageImageExtendedFormats.
    static bool IsBaseImageFormat(spv::ImageFormat format)
    {
        using F = spv::ImageFormat;
        switch (format)
        {
        case F::Unknown:
        case F::Rgba32f:
        case F::Rgba16f:
        case F::R32f:
        case F::Rgba8:
        case F::Rgba8Snorm:
        case F::Rgba32i:
        case F::Rgba16i:
        case F::Rgba8i:
        case F::R32i:
        case F::Rgba32ui:
        case F::Rgba16ui:
        case F::Rgba8ui:
        case F::R32ui:
            return true;
        default:
            return false;
        }
    }

    uint32 ImageType(const MirType *texture, bool depth)
    {
        const MirType *scalar = texture->GetElement()->GetScalar();
        const MirImageFormat format = texture->GetImageFormat();
        const bool wide = format == MirImageFormat::R64i || format == MirImageFormat::R64u;
        const MirType *sampled = SampledScalar(scalar);
        // RECOVERY-DECISION lang-spirv-D10: images are keyed by their SPIR-V shape, as textures that differ only in
        // their texel (Texture2D<float> and Texture2D<float4>) are one image type, which may be declared once.
        uint64 key = sampled->Is(MirTypeKind::Float) ? 0 : (sampled->IsSigned() ? 1 : 2) + (sampled->GetWidth() == 64 ? 2 : 0);
        key = (key << 2) | static_cast<uint64>(texture->GetTextureDim());
        key = (key << 4) | (depth ? 1 : 0) | (texture->IsArrayed() ? 2 : 0) | (texture->IsMultisampled() ? 4 : 0) |
              (texture->IsWritable() ? 8 : 0);
        key = (key << 8) | static_cast<uint64>(format);
        if (const uint32 *found = _images.Find(key))
        {
            return *found;
        }
        spv::Dim dim = spv::Dim::Dim2D;
        switch (texture->GetTextureDim())
        {
        case MirTextureDim::Texture1D:
            dim = spv::Dim::Dim1D;
            _spirv.Capability(texture->IsWritable() ? spv::Capability::Image1D : spv::Capability::Sampled1D);
            break;
        case MirTextureDim::Texture2D:
            dim = spv::Dim::Dim2D;
            break;
        case MirTextureDim::Texture3D:
            dim = spv::Dim::Dim3D;
            break;
        case MirTextureDim::Cube:
            dim = spv::Dim::Cube;
            if (texture->IsArrayed())
            {
                _spirv.Capability(spv::Capability::SampledCubeArray);
            }
            break;
        }
        if (texture->IsWritable() && format == MirImageFormat::Unknown)
        {
            _spirv.Capability(spv::Capability::StorageImageReadWithoutFormat);
            _spirv.Capability(spv::Capability::StorageImageWriteWithoutFormat);
        }
        const spv::ImageFormat spirvFormat = ImageFormatOf(format);
        if (wide)
        {
            _spirv.Extension("SPV_EXT_shader_image_int64");
            _spirv.Capability(spv::Capability::Int64ImageEXT);
        }
        else if (!IsBaseImageFormat(spirvFormat))
        {
            _spirv.Capability(spv::Capability::StorageImageExtendedFormats);
        }
        if (texture->IsMultisampled() && texture->IsArrayed())
        {
            _spirv.Capability(spv::Capability::ImageMSArray);
        }
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpTypeImage)
            .Add(id)
            .Add(Type(sampled))
            .Add(dim)
            .Add(depth ? 1u : 0u)
            .Add(texture->IsArrayed() ? 1u : 0u)
            .Add(texture->IsMultisampled() ? 1u : 0u)
            .Add(texture->IsWritable() ? 2u : 1u)
            .Add(spirvFormat)
            .WriteTo(_spirv.Globals);
        _images.Add(key, id);
        return id;
    }

    uint32 AccelerationStructureType()
    {
        if (_accelerationStructureType == 0)
        {
            _accelerationStructureType = _spirv.NewId();
            SpirvInstruction(Op::OpTypeAccelerationStructureKHR)
                .Add(_accelerationStructureType)
                .WriteTo(_spirv.Globals);
        }
        return _accelerationStructureType;
    }

    /// Origin, TMin, Direction and TMax of the RayDesc *ray*.
    void RayParts(const MirValue *ray, uint32 (&parts)[4])
    {
        MirTypeContext &types = _module->GetTypes();
        const MirType *f32 = types.GetFloat(32);
        const MirType *fields[] = {types.GetVector(f32, 3), f32, types.GetVector(f32, 3), f32};
        const uint32 value = Value(ray);
        for (uint32 i = 0; i < 4; ++i)
        {
            parts[i] = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpCompositeExtract).Add(Type(fields[i])).Add(parts[i]).Add(value).Add(i));
        }
    }

    void UseRayTracing()
    {
        _spirv.Capability(spv::Capability::RayTracingKHR);
        _spirv.Extension("SPV_KHR_ray_tracing");
    }

    void UseShaderInvocationReorder()
    {
        _spirv.Capability(spv::Capability::ShaderInvocationReorderEXT);
        _spirv.Extension("SPV_EXT_shader_invocation_reorder");
    }

    /// The type a global is declared with: ObjectToWorld and WorldToObject are SPIR-V's 4x3 matrices, the
    /// transpose of the 3x4 rows they hold (§11.3); loads transpose them back.
    const MirType *DeclaredType(const MirGlobal *global)
    {
        const MirType *type = global->GetValueType();
        if (global->Builtin == MirBuiltin::ObjectToWorld || global->Builtin == MirBuiltin::WorldToObject)
        {
            return _module->GetTypes().GetMatrix(type->GetScalar(), type->GetCount(), type->GetRows());
        }
        return type;
    }

    /// The HitObjectAttributeEXT variable hit objects made from ray queries record triangle barycentrics in.
    uint32 HitObjectAttributes()
    {
        if (_hitObjectAttributes == 0)
        {
            MirTypeContext &types = _module->GetTypes();
            _hitObjectAttributes = _spirv.NewId();
            const uint32 pointer = Pointer(types.GetVector(types.GetFloat(32), 2), spv::StorageClass::HitObjectAttributeEXT, false);
            SpirvInstruction(Op::OpVariable)
                .Add(pointer)
                .Add(_hitObjectAttributes)
                .Add(spv::StorageClass::HitObjectAttributeEXT)
                .WriteTo(_spirv.Globals);
            _spirv.Name(_hitObjectAttributes, "hitObjectAttributes");
            _usedGlobals.Add(_hitObjectAttributes);
        }
        return _hitObjectAttributes;
    }

    /// A typed pointer to the 32-bit words of the byte address buffer *handle* names, which cooperative loads, stores
    /// and matrix multiplications address with byte offsets; their pointers may not be untyped.
    uint32 ByteBufferWords(const MirValue *handle)
    {
        MirTypeContext &types = _module->GetTypes();
        const MirType *words = types.GetRuntimeArray(types.GetInt(32, false));
        if (_wordBufferPointer == 0)
        {
            const uint32 block = BufferBlock(words);
            _wordBufferPointer = _spirv.NewId();
            SpirvInstruction(Op::OpTypePointer).Add(_wordBufferPointer).Add(spv::StorageClass::PhysicalStorageBuffer).Add(block).WriteTo(_spirv.Globals);
        }
        const uint32 buffer = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpConvertUToPtr).Add(_wordBufferPointer).Add(buffer).Add(Value(handle)));
        const uint32 pointer = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpAccessChain)
                 .Add(Pointer(words, spv::StorageClass::PhysicalStorageBuffer, true))
                 .Add(pointer)
                 .Add(buffer)
                 .Add(UintConstant(0)));
        return pointer;
    }

    /// The constant component type of element code *code*: 0 to 10 are the scalars as ElementCodeOf numbers them;
    /// 11 and 12 are four signed or unsigned 8-bit integers packed in 32 bits, 13 and 14 the 8-bit floats E4M3 and
    /// E5M2, which matrices and inputs may be interpreted as (§9.9).
    uint32 ComponentTypeOf(int64 code)
    {
        using C = spv::ComponentType;
        static constexpr C components[] = {
            C::Float16NV,      C::Float32NV,      C::Float64NV,          C::SignedInt8NV,         C::SignedInt16NV,
            C::SignedInt32NV,  C::SignedInt64NV,  C::UnsignedInt8NV,     C::UnsignedInt16NV,      C::UnsignedInt32NV,
            C::UnsignedInt64NV, C::SignedInt8PackedNV, C::UnsignedInt8PackedNV, C::FloatE4M3NV, C::FloatE5M2NV,
        };
        // RECOVERY-DECISION lang-spirv-D5: element codes are this table's indices, a MIR-level numbering the
        // frontend puts in the literals of the matrix multiplications; an unknown code reads as 32-bit float.
        if (code < 0 || code >= static_cast<int64>(sizeof(components) / sizeof(components[0])))
        {
            code = 1;
        }
        return UintConstant(static_cast<uint32>(components[code]));
    }

    /// The element code of *scalar* (ComponentTypeOf): floats 0 to 2, signed integers 3 to 6, unsigned 7 to 10.
    static int64 ElementCodeOf(const MirType *scalar)
    {
        const int64 first = scalar->Is(MirTypeKind::Float) ? 0 : scalar->IsSigned() ? 3 : 7;
        return first + FloorLog2(scalar->GetWidth() / (scalar->Is(MirTypeKind::Float) ? 16u : 8u));
    }

    /// Field *index* of the struct *object* has type *type*: the constituent itself when the struct is built here.
    uint32 Field(const MirValue *object, uint32 index, const MirType *type)
    {
        if (const MirValue *constituent = Constituent(object, index))
        {
            return Value(constituent);
        }
        const uint32 id = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpCompositeExtract).Add(Type(type)).Add(id).Add(Value(object)).Add(index));
        return id;
    }

    /// Operand *index* of *object* if it is a CompositeConstruct, so values such as the buffer and layout of a
    /// MatrixRef stay what they were built from; null otherwise.
    static const MirValue *Constituent(const MirValue *object, uint32 index)
    {
        if (object->GetValueKind() != MirValueKind::Instruction)
        {
            return nullptr;
        }
        const auto *construct = static_cast<const MirInstruction *>(object);
        return construct->GetOp() == MirOp::CompositeConstruct && index < construct->GetOperands().Count()
                   ? construct->GetOperand(index)
                   : nullptr;
    }

    /// Cooperative vectors (SPV_NV_cooperative_vector) and matrices (SPV_KHR_cooperative_matrix), §9.9. Buffers are
    /// byte address buffers at byte offsets; a MatrixRef is the struct {buffer, offset, layout, stride} and a
    /// VectorRef {buffer, offset}, built where they are used; the literals of MatMul and MatMulAdd are the element
    /// codes of the matrix and the bias (ComponentTypeOf), by default those of the input and the result.
    // RECOVERY-DECISION lang-spirv-D4: the operand layout of the cooperative intrinsics, which the frontend lowers
    // the library functions of §9.9 to in parameter order (receiver first): see the notes.
    void EmitCooperative(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const Span<const int64> literals = instruction.GetLiterals();
        const MirType *type = instruction.GetType();
        MirTypeContext &types = _module->GetTypes();
        const MirType *u32 = types.GetInt(32, false);
        auto useVectors = [&](bool training) {
            _spirv.Capability(spv::Capability::CooperativeVectorNV);
            if (training)
            {
                _spirv.Capability(spv::Capability::CooperativeVectorTrainingNV);
            }
            _spirv.Extension("SPV_NV_cooperative_vector");
        };
        auto interpretation = [&](usize literal, const MirType *scalar) {
            return ComponentTypeOf(literal < literals.Count() ? literals[literal] : ElementCodeOf(scalar));
        };
        // The words of the buffer of a MatrixRef or VectorRef.
        auto viewWords = [&](const MirValue *view) {
            const MirValue *buffer = Constituent(view, 0);
            if (buffer == nullptr)
            {
                Error("{0} has no SPIR-V form yet", GetMirIntrinsicName(instruction.GetIntrinsic()));
                return UintConstant(0);
            }
            return ByteBufferWords(buffer);
        };
        // The word at a byte offset in a buffer, which a cooperative matrix starts at, and a byte stride in the words
        // the pointer steps by.
        auto words = [&](const MirValue *bytes) {
            const uint32 id = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpShiftRightLogical).Add(Type(u32)).Add(id).Add(Value(bytes)).Add(UintConstant(2)));
            return id;
        };
        auto matrixPointer = [&](const MirValue *buffer, const MirValue *offset) {
            const uint32 array = ByteBufferWords(buffer);
            const uint32 pointer = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpAccessChain)
                     .Add(Pointer(u32, spv::StorageClass::PhysicalStorageBuffer, false))
                     .Add(pointer)
                     .Add(array)
                     .Add(words(offset)));
            return pointer;
        };
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::CoopVectorLoad:
            useVectors(false);
            Emit(SpirvInstruction(Op::OpCooperativeVectorLoadNV)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(ByteBufferWords(operands[0]))
                     .Add(Value(operands[1])));
            break;
        case MirIntrinsic::CoopVectorStore:
            useVectors(false);
            Emit(SpirvInstruction(Op::OpCooperativeVectorStoreNV)
                     .Add(ByteBufferWords(operands[1]))
                     .Add(Value(operands[2]))
                     .Add(Value(operands[0])));
            break;
        case MirIntrinsic::CoopVectorMatMul:
        case MirIntrinsic::CoopVectorMatMulAdd:
        {
            useVectors(false);
            const bool add = instruction.GetIntrinsic() == MirIntrinsic::CoopVectorMatMulAdd;
            const MirValue *matrix = operands[0];
            const MirType *input = operands[1]->GetType();
            SpirvInstruction multiply(add ? Op::OpCooperativeVectorMatrixMulAddNV : Op::OpCooperativeVectorMatrixMulNV);
            multiply.Add(Type(type))
                .Add(Value(&instruction))
                .Add(Value(operands[1]))
                .Add(ComponentTypeOf(ElementCodeOf(input->GetElement())))
                .Add(viewWords(matrix))
                .Add(Field(matrix, 1, u32))
                .Add(interpretation(0, input->GetElement()));
            if (add)
            {
                multiply.Add(viewWords(operands[2])).Add(Field(operands[2], 1, u32)).Add(interpretation(1, type->GetElement()));
            }
            multiply.Add(UintConstant(type->GetCount()))
                .Add(UintConstant(input->GetCount()))
                .Add(Field(matrix, 2, u32))
                .Add(ConstantValue(_module->GetBool(false)))
                .Add(Field(matrix, 3, u32));
            Emit(multiply);
            break;
        }
        case MirIntrinsic::CoopVectorOuterProductAccumulate:
            useVectors(true);
            Emit(SpirvInstruction(Op::OpCooperativeVectorOuterProductAccumulateNV)
                     .Add(ByteBufferWords(operands[2]))
                     .Add(Value(operands[3]))
                     .Add(Value(operands[0]))
                     .Add(Value(operands[1]))
                     .Add(Value(operands[4]))
                     .Add(interpretation(0, operands[0]->GetType()->GetElement())));
            break;
        case MirIntrinsic::CoopVectorReduceSumAccumulate:
            useVectors(true);
            Emit(SpirvInstruction(Op::OpCooperativeVectorReduceSumAccumulateNV)
                     .Add(ByteBufferWords(operands[1]))
                     .Add(Value(operands[2]))
                     .Add(Value(operands[0])));
            break;
        case MirIntrinsic::CoopMatrixLoad:
            Emit(SpirvInstruction(Op::OpCooperativeMatrixLoadKHR)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(matrixPointer(operands[0], operands[1]))
                     .Add(Value(operands[3]))
                     .Add(words(operands[2])));
            break;
        case MirIntrinsic::CoopMatrixStore:
            Emit(SpirvInstruction(Op::OpCooperativeMatrixStoreKHR)
                     .Add(matrixPointer(operands[1], operands[2]))
                     .Add(Value(operands[0]))
                     .Add(Value(operands[4]))
                     .Add(words(operands[3])));
            break;
        case MirIntrinsic::CoopMatrixMulAdd:
        {
            uint32 signedness = 0;
            for (uint32 i = 0; i < 3; ++i)
            {
                const MirType *scalar = operands[i]->GetType()->GetElement();
                signedness |= scalar->Is(MirTypeKind::Int) && scalar->IsSigned() ? 1u << i : 0u;
            }
            const MirType *result = type->GetElement();
            signedness |= result->Is(MirTypeKind::Int) && result->IsSigned() ? 8u : 0u;
            SpirvInstruction multiply(Op::OpCooperativeMatrixMulAddKHR);
            multiply.Add(Type(type)).Add(Value(&instruction)).Add(Value(operands[0])).Add(Value(operands[1])).Add(Value(operands[2]));
            if (signedness != 0)
            {
                multiply.Add(signedness);
            }
            Emit(multiply);
            break;
        }
        case MirIntrinsic::CoopMatrixLength:
            Emit(SpirvInstruction(Op::OpCooperativeMatrixLengthKHR)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(Type(operands[0]->GetType())));
            break;
        default:
            break;
        }
    }

    /// TraceRay, CallShader, ReportHit, hit objects and ReorderThread (§9.8).
    void EmitRayTracing(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        MirTypeContext &types = _module->GetTypes();
        uint32 parts[4] = {};
        auto get = [&](Op op) {
            Emit(SpirvInstruction(op).Add(Type(instruction.GetType())).Add(Value(&instruction)).Add(Value(operands[0])));
        };
        auto transposed = [&](Op op) {
            const MirType *type = instruction.GetType();
            const uint32 raw = _spirv.NewId();
            Emit(SpirvInstruction(op)
                     .Add(Type(types.GetMatrix(type->GetScalar(), type->GetCount(), type->GetRows())))
                     .Add(raw)
                     .Add(Value(operands[0])));
            Emit(SpirvInstruction(Op::OpTranspose).Add(Type(type)).Add(Value(&instruction)).Add(raw));
        };
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::TraceRay:
        {
            UseRayTracing();
            const uint32 scene = LoadDescriptor(operands[0], AccelerationStructureType(), false, instruction.NonUniform);
            RayParts(operands[1], parts);
            Emit(SpirvInstruction(Op::OpTraceRayKHR)
                     .Add(scene)
                     .Add(Value(operands[3]))
                     .Add(Value(operands[4]))
                     .Add(Value(operands[5]))
                     .Add(Value(operands[6]))
                     .Add(Value(operands[7]))
                     .Add(Span<const uint32>(parts))
                     .Add(Value(operands[2])));
            break;
        }
        case MirIntrinsic::CallShader:
            UseRayTracing();
            Emit(SpirvInstruction(Op::OpExecuteCallableKHR).Add(Value(operands[0])).Add(Value(operands[1])));
            break;
        case MirIntrinsic::ReportHit:
            UseRayTracing();
            Emit(SpirvInstruction(Op::OpReportIntersectionKHR)
                     .Add(Type(instruction.GetType()))
                     .Add(Value(&instruction))
                     .Add(Value(operands[0]))
                     .Add(Value(operands[1])));
            break;
        case MirIntrinsic::HitObjectTraceRay:
        {
            UseRayTracing();
            const uint32 scene = LoadDescriptor(operands[1], AccelerationStructureType(), false, instruction.NonUniform);
            RayParts(operands[2], parts);
            Emit(SpirvInstruction(Op::OpHitObjectTraceRayEXT)
                     .Add(Value(operands[0]))
                     .Add(scene)
                     .Add(Value(operands[4]))
                     .Add(Value(operands[5]))
                     .Add(Value(operands[6]))
                     .Add(Value(operands[7]))
                     .Add(Value(operands[8]))
                     .Add(Span<const uint32>(parts))
                     .Add(Value(operands[3])));
            break;
        }
        case MirIntrinsic::HitObjectFromRayQuery:
            Emit(SpirvInstruction(Op::OpHitObjectRecordFromQueryEXT)
                     .Add(Value(operands[0]))
                     .Add(Value(operands[1]))
                     .Add(Value(operands[2]))
                     .Add(HitObjectAttributes()));
            break;
        case MirIntrinsic::HitObjectMakeMiss:
            RayParts(operands[2], parts);
            Emit(SpirvInstruction(Op::OpHitObjectRecordMissEXT)
                     .Add(Value(operands[0]))
                     .Add(UintConstant(0))
                     .Add(Value(operands[1]))
                     .Add(Span<const uint32>(parts)));
            break;
        case MirIntrinsic::HitObjectMakeNop:
            Emit(SpirvInstruction(Op::OpHitObjectRecordEmptyEXT).Add(Value(operands[0])));
            break;
        case MirIntrinsic::HitObjectInvoke:
            UseRayTracing();
            Emit(SpirvInstruction(Op::OpHitObjectExecuteShaderEXT).Add(Value(operands[0])).Add(Value(operands[1])));
            break;
        case MirIntrinsic::ReorderThread:
        {
            if (operands[0]->GetType()->Is(MirTypeKind::Pointer))
            {
                SpirvInstruction reorder(Op::OpReorderThreadWithHitObjectEXT);
                reorder.Add(Value(operands[0]));
                for (usize i = 1; i < operands.Count(); ++i)
                {
                    reorder.Add(Value(operands[i]));
                }
                Emit(reorder);
            }
            else
            {
                Emit(SpirvInstruction(Op::OpReorderThreadWithHintEXT).Add(Value(operands[0])).Add(Value(operands[1])));
            }
            break;
        }
        case MirIntrinsic::HitObjectGet:
            switch (instruction.GetLiterals()[0])
            {
            case 0:
                get(Op::OpHitObjectIsHitEXT);
                break;
            case 1:
                get(Op::OpHitObjectIsMissEXT);
                break;
            case 2:
                get(Op::OpHitObjectIsEmptyEXT);
                break;
            case 3:
                get(Op::OpHitObjectGetInstanceIdEXT);
                break;
            case 4:
                get(Op::OpHitObjectGetInstanceCustomIndexEXT);
                break;
            case 5:
                get(Op::OpHitObjectGetGeometryIndexEXT);
                break;
            case 6:
                get(Op::OpHitObjectGetPrimitiveIndexEXT);
                break;
            case 7:
                get(Op::OpHitObjectGetHitKindEXT);
                break;
            case 8:
                get(Op::OpHitObjectGetRayTMaxEXT);
                break;
            case 9:
                get(Op::OpHitObjectGetRayTMinEXT);
                break;
            case 10:
                get(Op::OpHitObjectGetRayFlagsEXT);
                break;
            case 11:
                get(Op::OpHitObjectGetWorldRayOriginEXT);
                break;
            case 12:
                get(Op::OpHitObjectGetWorldRayDirectionEXT);
                break;
            case 13:
                get(Op::OpHitObjectGetObjectRayOriginEXT);
                break;
            case 14:
                get(Op::OpHitObjectGetObjectRayDirectionEXT);
                break;
            case 15:
                transposed(Op::OpHitObjectGetObjectToWorldEXT);
                break;
            case 16:
                transposed(Op::OpHitObjectGetWorldToObjectEXT);
                break;
            case 17:
                get(Op::OpHitObjectGetShaderBindingTableRecordIndexEXT);
                break;
            default:
                Error("unknown hit object value {0}", instruction.GetLiterals()[0]);
                break;
            }
            break;
        default:
            break;
        }
    }

    /// The ray query *query* points to: `RayQuery<Flags>` ORs its fixed flags into the ones given at the start.
    void EmitRayQuery(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const uint32 query = Value(operands[0]);
        MirTypeContext &types = _module->GetTypes();
        const MirType *f32 = types.GetFloat(32);
        const MirType *u32 = types.GetInt(32, false);
        auto get = [&](Op op, uint32 committed) {
            Emit(SpirvInstruction(op)
                     .Add(Type(instruction.GetType()))
                     .Add(Value(&instruction))
                     .Add(query)
                     .Add(UintConstant(committed)));
        };
        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::RayQueryInitialize:
        {
            const uint32 scene =
                LoadDescriptor(operands[1], AccelerationStructureType(), false, instruction.NonUniform);
            uint32 flags = Value(operands[2]);
            const uint32 fixed = operands[0]->GetType()->GetElement()->GetCount();
            if (fixed != 0)
            {
                const uint32 combined = _spirv.NewId();
                Emit(SpirvInstruction(Op::OpBitwiseOr).Add(Type(u32)).Add(combined).Add(flags).Add(UintConstant(fixed)));
                flags = combined;
            }
            uint32 parts[4] = {};
            RayParts(operands[4], parts);
            Emit(SpirvInstruction(Op::OpRayQueryInitializeKHR)
                     .Add(query)
                     .Add(scene)
                     .Add(flags)
                     .Add(Value(operands[3]))
                     .Add(Span<const uint32>(parts)));
            break;
        }
        case MirIntrinsic::RayQueryProceed:
            Emit(SpirvInstruction(Op::OpRayQueryProceedKHR).Add(Type(instruction.GetType())).Add(Value(&instruction)).Add(query));
            break;
        case MirIntrinsic::RayQueryAbort:
            Emit(SpirvInstruction(Op::OpRayQueryTerminateKHR).Add(query));
            break;
        case MirIntrinsic::RayQueryConfirmIntersection:
            Emit(SpirvInstruction(Op::OpRayQueryConfirmIntersectionKHR).Add(query));
            break;
        case MirIntrinsic::RayQueryGenerateIntersection:
            Emit(SpirvInstruction(Op::OpRayQueryGenerateIntersectionKHR).Add(query).Add(Value(operands[1])));
            break;
        case MirIntrinsic::RayQueryGet:
            switch (instruction.GetLiterals()[0])
            {
            case 0:
                get(Op::OpRayQueryGetIntersectionTypeKHR, 1);
                break;
            case 1:
                get(Op::OpRayQueryGetIntersectionTypeKHR, 0);
                break;
            case 2:
                get(Op::OpRayQueryGetIntersectionTKHR, 1);
                break;
            case 3:
                get(Op::OpRayQueryGetIntersectionTKHR, 0);
                break;
            case 4:
                get(Op::OpRayQueryGetIntersectionInstanceIdKHR, 1);
                break;
            case 5:
                get(Op::OpRayQueryGetIntersectionInstanceCustomIndexKHR, 1);
                break;
            case 6:
                get(Op::OpRayQueryGetIntersectionGeometryIndexKHR, 1);
                break;
            case 7:
                get(Op::OpRayQueryGetIntersectionPrimitiveIndexKHR, 1);
                break;
            case 8:
                get(Op::OpRayQueryGetIntersectionBarycentricsKHR, 1);
                break;
            case 9:
                get(Op::OpRayQueryGetIntersectionFrontFaceKHR, 1);
                break;
            case 10:
            {
                const MirType *type = instruction.GetType();
                const MirType *transposed = types.GetMatrix(f32, type->GetCount(), type->GetRows());
                const uint32 raw = _spirv.NewId();
                Emit(SpirvInstruction(Op::OpRayQueryGetIntersectionObjectToWorldKHR)
                         .Add(Type(transposed))
                         .Add(raw)
                         .Add(query)
                         .Add(UintConstant(1)));
                Emit(SpirvInstruction(Op::OpTranspose).Add(Type(type)).Add(Value(&instruction)).Add(raw));
                break;
            }
            case 11:
                get(Op::OpRayQueryGetIntersectionInstanceIdKHR, 0);
                break;
            case 12:
                get(Op::OpRayQueryGetIntersectionPrimitiveIndexKHR, 0);
                break;
            case 13:
                get(Op::OpRayQueryGetIntersectionBarycentricsKHR, 0);
                break;
            default:
                Error("unknown ray query value {0}", instruction.GetLiterals()[0]);
                break;
            }
            break;
        default:
            break;
        }
    }

    uint32 SamplerType()
    {
        if (_samplerType == 0)
        {
            _samplerType = _spirv.NewId();
            SpirvInstruction(Op::OpTypeSampler).Add(_samplerType).WriteTo(_spirv.Globals);
        }
        return _samplerType;
    }

    uint32 BufferDescriptorType()
    {
        if (_bufferType == 0)
        {
            _bufferType = _spirv.NewId();
            SpirvInstruction(Op::OpTypeBufferEXT).Add(_bufferType).Add(spv::StorageClass::StorageBuffer).WriteTo(_spirv.Globals);
        }
        return _bufferType;
    }

    // Functions

    void PreassignIds(const MirFunction &function)
    {
        _values.Add(&function, _spirv.NewId());
        for (const MirArgument *argument : function.GetArguments())
        {
            _values.Add(argument, _spirv.NewId());
        }
        for (const MirBlock *block : function.GetBlocks())
        {
            _blocks.Add(block, _spirv.NewId());
            for (const MirInstruction *instruction : *block)
            {
                if (!instruction->GetType()->Is(MirTypeKind::Void))
                {
                    _values.Add(instruction, _spirv.NewId());
                }
            }
        }
    }

    uint32 Value(const MirValue *value)
    {
        switch (value->GetValueKind())
        {
        case MirValueKind::Constant:
            return ConstantValue(static_cast<const MirConstant *>(value));
        case MirValueKind::Global:
            return Global(static_cast<const MirGlobal *>(value));
        default:
        {
            const uint32 *id = _values.Find(value);
            MARLIN_ASSERT(id != nullptr);
            return id != nullptr ? *id : 0;
        }
        }
    }

    void Emit(const SpirvInstruction &instruction)
    {
        instruction.WriteTo(_body);
    }

    void EmitFunction(const MirFunction &function)
    {
        _body.Clear();
        _variables.Clear();
        const uint32 id = Value(&function);
        _spirv.Name(id, function.GetName());
        const uint32 returnType = Type(function.GetReturnType());
        const uint32 functionType = Type(function.GetType());
        SpirvInstruction(Op::OpFunction).Add(returnType).Add(id).Add(spv::FunctionControlMask::MaskNone).Add(functionType).WriteTo(_spirv.Functions);
        for (const MirArgument *argument : function.GetArguments())
        {
            SpirvInstruction(Op::OpFunctionParameter).Add(Type(argument->GetType())).Add(Value(argument)).WriteTo(_spirv.Functions);
            _spirv.Name(Value(argument), argument->GetName());
        }
        _debugFunction = _sourceDebugInfo ? DebugFunction(function, id) : 0;
        // RECOVERY-DECISION lang-spirv-D11: blocks go in reverse post-order, as SPIR-V needs every block after its
        // dominators and passes such as inlining may leave the function's list in another order; unreachable blocks
        // follow, as merge instructions may still name them.
        const MirControlFlow flow(function);
        List<const MirBlock *> order;
        for (const MirBlock *block : flow.GetReversePostOrder())
        {
            order.Add(block);
        }
        for (const MirBlock *block : function.GetBlocks())
        {
            if (!flow.IsReachable(block))
            {
                order.Add(block);
            }
        }
        for (const MirBlock *block : order)
        {
            Emit(SpirvInstruction(Op::OpLabel).Add(*_blocks.Find(block)));
            if (block == function.GetEntry())
            {
                _variablesAt = _body.Count();
            }
            _scopeOpen = false;
            _lastLine = {};
            if (block->begin() == block->end() || (*block->begin())->GetOp() != MirOp::Phi)
            {
                OpenScope();
            }
            for (const MirInstruction *instruction : *block)
            {
                if (instruction->IsTerminator())
                {
                    SetLine(instruction->Location);
                    EmitMerge(*block);
                }
                EmitInstruction(*instruction);
            }
        }
        List<uint32> body;
        body.AddRange(Span<const uint32>(_body.Data(), _variablesAt));
        body.AddRange(_variables.AsSpan());
        body.AddRange(Span<const uint32>(_body.Data() + _variablesAt, _body.Count() - _variablesAt));
        _spirv.Functions.AddRange(body.AsSpan());
        SpirvInstruction(Op::OpFunctionEnd).WriteTo(_spirv.Functions);
    }

    void EmitMerge(const MirBlock &block)
    {
        if (block.Merge.Kind == MirMergeKind::Selection)
        {
            Emit(SpirvInstruction(Op::OpSelectionMerge).Add(*_blocks.Find(block.Merge.MergeBlock)).Add(spv::SelectionControlMask::MaskNone));
        }
        else if (block.Merge.Kind == MirMergeKind::Loop)
        {
            SpirvInstruction merge(Op::OpLoopMerge);
            merge.Add(*_blocks.Find(block.Merge.MergeBlock)).Add(*_blocks.Find(block.Merge.ContinueBlock));
            if (block.Merge.Control != MirLoopControl::Unroll)
            {
                merge.Add(spv::LoopControlMask::MaskNone);
            }
            else if (block.Merge.UnrollCount == 0)
            {
                merge.Add(spv::LoopControlMask::Unroll);
            }
            else
            {
                merge.Add(spv::LoopControlMask::PartialCount).Add(block.Merge.UnrollCount);
            }
            Emit(merge);
        }
    }

    // Source debug information: NonSemantic.Shader.DebugInfo.100 (§11.3), emitted when the frontend kept the
    // sources: a compilation unit, the source texts, types, functions with their lines, and the local variables of
    // DebugDeclare. Without the sources, OpLine still maps instructions to files and lines.

    /// An instruction of the debug information set, with result *id* or a new one.
    SpirvInstruction DebugInstruction(uint32 instruction, Span<const uint32> operands, uint32 id = 0)
    {
        if (_debugSet == 0)
        {
            _spirv.Extension("SPV_KHR_non_semantic_info");
            _debugSet = _spirv.ExtendedInstructions("NonSemantic.Shader.DebugInfo.100");
        }
        SpirvInstruction result(Op::OpExtInst);
        result.Add(Type(_module->GetTypes().GetVoid()))
            .Add(id != 0 ? id : _spirv.NewId())
            .Add(_debugSet)
            .Add(instruction)
            .Add(operands);
        return result;
    }

    uint32 DebugGlobal(uint32 instruction, Span<const uint32> operands)
    {
        const uint32 id = _spirv.NewId();
        DebugInstruction(instruction, operands, id).WriteTo(_spirv.Globals);
        return id;
    }

    uint32 DebugString(StringView text)
    {
        const uint32 id = _spirv.NewId();
        SpirvInstruction(Op::OpString).Add(id).Add(text).WriteTo(_spirv.DebugStrings);
        return id;
    }

    uint32 DebugNone()
    {
        if (_debugNone == 0)
        {
            _debugNone = DebugGlobal(NonSemanticShaderDebugInfo100DebugInfoNone, {});
        }
        return _debugNone;
    }

    uint32 DebugExpression()
    {
        if (_debugExpression == 0)
        {
            _debugExpression = DebugGlobal(NonSemanticShaderDebugInfo100DebugExpression, {});
        }
        return _debugExpression;
    }

    /// The DebugSource of file *file* of the module, with its text when the frontend kept it.
    uint32 DebugSource(uint32 file)
    {
        if (const uint32 *found = _debugSources.Find(file))
        {
            return *found;
        }
        // RECOVERY-DECISION lang-spirv-D2: texts go in pieces of 64 KiB, well below the 262 KB an OpString can hold;
        // the rest follow as DebugSourceContinued.
        constexpr usize piece = 65536;
        const StringView text = _module->GetFileTexts()[file - 1];
        List<uint32> source;
        source.Add(DebugString(_module->GetFiles()[file - 1]));
        if (!text.IsEmpty())
        {
            source.Add(DebugString(text.Substring(0, piece)));
        }
        const uint32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugSource, source.AsSpan());
        for (usize at = piece; at < text.Length(); at += piece)
        {
            const uint32 rest[] = {DebugString(text.Substring(at, piece))};
            DebugGlobal(NonSemanticShaderDebugInfo100DebugSourceContinued, rest);
        }
        _debugSources.Add(file, id);
        return id;
    }

    /// The file a compilation unit is named after: the file of the first entry point, else the first file.
    uint32 MainFile() const
    {
        const Span<MirEntryPoint *const> entries = _module->GetEntryPoints();
        const uint32 file = entries.IsEmpty() ? 0 : entries[0]->Function->Location.File;
        return file != 0 ? file : 1;
    }

    uint32 CompilationUnit()
    {
        if (_compilationUnit == 0)
        {
            // RECOVERY-DECISION lang-spirv-D3: the language is Unknown; SPIR-V has no id for the Marlin language.
            const uint32 operands[] = {UintConstant(NonSemanticShaderDebugInfo100Version), UintConstant(5),
                                       DebugSource(MainFile()), UintConstant(static_cast<uint32>(spv::SourceLanguage::Unknown))};
            _compilationUnit = DebugGlobal(NonSemanticShaderDebugInfo100DebugCompilationUnit, operands);
        }
        return _compilationUnit;
    }

    /// The debug type of *type*; DebugInfoNone for types debuggers cannot show, such as ray queries.
    uint32 DebugType(const MirType *type)
    {
        if (const uint32 *found = _debugTypes.Find(type))
        {
            return *found;
        }
        uint32 id = 0;
        switch (type->GetKind())
        {
        case MirTypeKind::Bool:
        case MirTypeKind::Int:
        case MirTypeKind::Float:
        case MirTypeKind::Texture:
        case MirTypeKind::Buffer:
        case MirTypeKind::Sampler:
        case MirTypeKind::AccelerationStructure:
        {
            // Handles show as what they are stored as: buffers as 64-bit addresses, the others as 32-bit heap indices.
            const uint32 encoding = type->Is(MirTypeKind::Bool)    ? NonSemanticShaderDebugInfo100Boolean
                                    : type->Is(MirTypeKind::Float) ? NonSemanticShaderDebugInfo100Float
                                    : type->IsSigned()             ? NonSemanticShaderDebugInfo100Signed
                                                                   : NonSemanticShaderDebugInfo100Unsigned;
            const uint32 bits = type->Is(MirTypeKind::Buffer)                         ? 64
                                : type->IsHandle() || type->Is(MirTypeKind::Bool) ? 32
                                                                                  : type->GetWidth();
            const uint32 operands[] = {DebugString(TypeText(type).AsView()), UintConstant(bits), UintConstant(encoding),
                                       UintConstant(0)};
            id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeBasic, operands);
            break;
        }
        case MirTypeKind::Vector:
        {
            const uint32 operands[] = {DebugType(type->GetElement()), UintConstant(type->GetCount())};
            id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeVector, operands);
            break;
        }
        case MirTypeKind::Matrix:
        {
            // SPIR-V's columns are our rows (§11.3).
            const uint32 operands[] = {DebugType(type->GetElement()), UintConstant(type->GetRows()),
                                       ConstantValue(_module->GetBool(true))};
            id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeMatrix, operands);
            break;
        }
        case MirTypeKind::Array:
        case MirTypeKind::RuntimeArray:
        {
            const uint32 count = type->Is(MirTypeKind::Array) ? type->GetCount() : 0;
            const uint32 operands[] = {DebugType(type->GetElement()), UintConstant(count)};
            id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeArray, operands);
            break;
        }
        case MirTypeKind::Struct:
        {
            const uint32 name = DebugString(type->GetName());
            const uint32 source = DebugSource(MainFile());
            List<uint32> operands = {name, UintConstant(NonSemanticShaderDebugInfo100Structure), source, UintConstant(0),
                                     UintConstant(0), CompilationUnit(), name, UintConstant(type->GetSize() * 8)};
            operands.Add(UintConstant(NonSemanticShaderDebugInfo100FlagIsPublic));
            for (const MirStructField &field : type->GetFields())
            {
                const uint32 member[] = {DebugString(field.Name), DebugType(field.Type), source, UintConstant(0),
                                         UintConstant(0), UintConstant(field.Offset * 8),
                                         UintConstant(field.Type->GetSize() * 8),
                                         UintConstant(NonSemanticShaderDebugInfo100FlagIsPublic)};
                operands.Add(DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeMember, member));
            }
            id = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeComposite, operands.AsSpan());
            break;
        }
        default:
            id = DebugNone();
            break;
        }
        _debugTypes.Add(type, id);
        return id;
    }

    /// The DebugFunction of *function*, whose OpFunction is *definition*; the entry block of the definition gets
    /// its DebugFunctionDefinition when the scope opens.
    uint32 DebugFunction(const MirFunction &function, uint32 definition)
    {
        _currentFunction = definition;
        _definitionPending = true;
        if (const uint32 *found = _debugFunctions.Find(&function))
        {
            return *found;
        }
        List<uint32> signature;
        signature.Add(UintConstant(NonSemanticShaderDebugInfo100FlagIsPublic));
        const MirType *returnType = function.GetReturnType();
        signature.Add(returnType->Is(MirTypeKind::Void) ? Type(returnType) : DebugType(returnType));
        for (const MirArgument *argument : function.GetArguments())
        {
            signature.Add(DebugType(argument->GetType()));
        }
        const uint32 type = DebugGlobal(NonSemanticShaderDebugInfo100DebugTypeFunction, signature.AsSpan());
        const MirLocation location = function.Location;
        const uint32 name = DebugString(function.GetName());
        const uint32 operands[] = {name, type, DebugSource(location.File != 0 ? location.File : MainFile()),
                                   UintConstant(location.Line), UintConstant(location.Column), CompilationUnit(), name,
                                   UintConstant(NonSemanticShaderDebugInfo100FlagIsPublic), UintConstant(location.Line)};
        const uint32 id = DebugGlobal(NonSemanticShaderDebugInfo100DebugFunction, operands);
        _debugFunctions.Add(&function, id);
        return id;
    }

    /// Opens the scope of the function being written in the current block, as SPIR-V ends scopes and lines with
    /// each block; the first scope of a function is followed by its DebugFunctionDefinition.
    void OpenScope()
    {
        if (_debugFunction == 0 || _scopeOpen)
        {
            return;
        }
        _scopeOpen = true;
        const uint32 scope[] = {_debugFunction};
        Emit(DebugInstruction(NonSemanticShaderDebugInfo100DebugScope, scope));
        if (_definitionPending)
        {
            _definitionPending = false;
            const uint32 definition[] = {_debugFunction, _currentFunction};
            Emit(DebugInstruction(NonSemanticShaderDebugInfo100DebugFunctionDefinition, definition));
        }
    }

    void SetLine(const MirLocation &location)
    {
        if (!_spirv.HasDebugInfo() || location.File == 0)
        {
            return;
        }
        if (_sourceDebugInfo)
        {
            OpenScope();
            if (location.File == _lastLine.File && location.Line == _lastLine.Line && location.Column == _lastLine.Column)
            {
                return;
            }
            _lastLine = location;
            const uint32 line = UintConstant(location.Line);
            const uint32 column = UintConstant(location.Column);
            const uint32 operands[] = {DebugSource(location.File), line, line, column, column};
            Emit(DebugInstruction(NonSemanticShaderDebugInfo100DebugLine, operands));
            return;
        }
        uint32 *file = _fileStrings.Find(location.File);
        if (file == nullptr)
        {
            const uint32 id = _spirv.NewId();
            const StringView path = _module->GetFiles()[location.File - 1];
            SpirvInstruction(Op::OpString).Add(id).Add(path).WriteTo(_spirv.DebugStrings);
            SpirvInstruction(Op::OpSource).Add(spv::SourceLanguage::Unknown).Add(0u).Add(id).WriteTo(_spirv.DebugStrings);
            file = &_fileStrings.Add(location.File, id);
        }
        Emit(SpirvInstruction(Op::OpLine).Add(*file).Add(location.Line).Add(location.Column));
    }

    /// DebugDeclare of source variable literal 0, whose value is now operand 0. Debuggers read variables from
    /// memory, so the value is stored to a function variable, declared at the first DebugDeclare of the variable
    /// in the function.
    // RECOVERY-DECISION lang-spirv-D6: one Function variable per source variable and function, stored at every
    // DebugDeclare; values that cannot live in a variable (pointers, ray queries, hit objects) are not shown.
    void EmitDebugDeclare(const MirInstruction &instruction)
    {
        const MirValue *value = instruction.GetOperand(0);
        const MirType *type = value->GetType();
        switch (type->GetKind())
        {
        case MirTypeKind::Void:
        case MirTypeKind::Pointer:
        case MirTypeKind::Function:
        case MirTypeKind::RayQuery:
        case MirTypeKind::HitObject:
        case MirTypeKind::Reference:
            return;
        default:
            break;
        }
        const auto index = static_cast<uint32>(instruction.GetLiterals()[0]);
        const uint64 key = (static_cast<uint64>(_debugFunction) << 32) | index;
        uint32 *variable = _debugLocals.Find(key);
        if (variable == nullptr)
        {
            const MirDebugVariable &source = _module->GetDebugVariables()[index];
            const uint32 id = _spirv.NewId();
            SpirvInstruction(Op::OpVariable)
                .Add(Pointer(type, spv::StorageClass::Function, false))
                .Add(id)
                .Add(spv::StorageClass::Function)
                .WriteTo(_variables);
            _spirv.Name(id, source.Name);
            const MirLocation location = source.Location;
            const uint32 local[] = {DebugString(source.Name),
                                    DebugType(source.Type != nullptr ? source.Type : type),
                                    DebugSource(location.File != 0 ? location.File : MainFile()),
                                    UintConstant(location.Line),
                                    UintConstant(location.Column),
                                    _debugFunction,
                                    UintConstant(NonSemanticShaderDebugInfo100FlagIsLocal)};
            const uint32 declared[] = {DebugGlobal(NonSemanticShaderDebugInfo100DebugLocalVariable, local), id,
                                       DebugExpression()};
            OpenScope();
            Emit(DebugInstruction(NonSemanticShaderDebugInfo100DebugDeclare, declared));
            variable = &_debugLocals.Add(key, id);
        }
        Emit(SpirvInstruction(Op::OpStore).Add(*variable).Add(Value(value)));
    }

    bool IsFloat(const MirType *type)
    {
        const MirType *scalar = ScalarOf(type);
        return scalar != nullptr && scalar->Is(MirTypeKind::Float);
    }

    bool IsSigned(const MirType *type)
    {
        const MirType *scalar = ScalarOf(type);
        return scalar != nullptr && scalar->IsSigned();
    }

    bool IsBool(const MirType *type)
    {
        const MirType *scalar = ScalarOf(type);
        return scalar != nullptr && scalar->Is(MirTypeKind::Bool);
    }

    /// The scalar of a scalar, vector, matrix or cooperative type, whose arithmetic is per element.
    static const MirType *ScalarOf(const MirType *type)
    {
        const bool cooperative = type->Is(MirTypeKind::CoopVector) || type->Is(MirTypeKind::CoopMatrix);
        return cooperative ? type->GetElement() : type->GetScalar();
    }

    /// The constant *value* of *scalar*, or a vector of *count* of them.
    const MirConstant *SplatConstant(const MirType *scalar, int64 value, uint32 count)
    {
        MirConstant *element = _module->GetInt(scalar, value);
        if (count == 1)
        {
            return element;
        }
        List<MirConstant *> elements;
        for (uint32 i = 0; i < count; ++i)
        {
            elements.Add(element);
        }
        return _module->GetComposite(_module->GetTypes().GetVector(scalar, count), elements.AsSpan());
    }

    uint32 Extended(const MirInstruction &instruction, uint32 operation, Span<const uint32> operands)
    {
        const uint32 id = Value(&instruction);
        Emit(SpirvInstruction(Op::OpExtInst).Add(Type(instruction.GetType())).Add(id).Add(_spirv.ExtendedInstructions("GLSL.std.450")).Add(operation).Add(operands));
        return id;
    }

    /// Whether a value of *pointer*'s pointee is stored with explicit layout, so loads and stores convert.
    static bool IsLayoutPointer(const MirType *pointer)
    {
        const MirAddressSpace space = pointer->GetAddressSpace();
        return (space == MirAddressSpace::Buffer || space == MirAddressSpace::Uniform ||
                space == MirAddressSpace::ShaderRecord) &&
               HasLayoutForm(pointer->GetElement());
    }

    /// Loads the descriptor a handle names from its heap.
    uint32 LoadDescriptor(const MirValue *handle, uint32 descriptorType, bool samplers, bool nonUniform)
    {
        const uint32 heap = Heap(samplers);
        const uint32 array = DescriptorArray(descriptorType, samplers);
        const uint32 pointer = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpUntypedAccessChainKHR)
                 .Add(UntypedPointer(spv::StorageClass::UniformConstant))
                 .Add(pointer)
                 .Add(array)
                 .Add(heap)
                 .Add(Value(handle)));
        const uint32 descriptor = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpLoad).Add(descriptorType).Add(descriptor).Add(pointer));
        if (nonUniform)
        {
            _spirv.Capability(spv::Capability::ShaderNonUniform);
            _spirv.Decorate(pointer, spv::Decoration::NonUniform);
            _spirv.Decorate(descriptor, spv::Decoration::NonUniform);
        }
        return descriptor;
    }

    uint32 SampledImage(const MirInstruction &instruction, const MirValue *texture, const MirValue *sampler, bool depth)
    {
        const uint32 imageType = ImageType(texture->GetType(), depth);
        const uint32 image = LoadDescriptor(texture, imageType, false, instruction.NonUniform);
        const uint32 samplerValue = LoadDescriptor(sampler, SamplerType(), true, instruction.NonUniform);
        auto found = _sampledImageTypes.Find(imageType);
        uint32 sampledType = found != nullptr ? *found : 0;
        if (sampledType == 0)
        {
            sampledType = _spirv.NewId();
            SpirvInstruction(Op::OpTypeSampledImage).Add(sampledType).Add(imageType).WriteTo(_spirv.Globals);
            _sampledImageTypes.Add(imageType, sampledType);
        }
        const uint32 combined = _spirv.NewId();
        Emit(SpirvInstruction(Op::OpSampledImage).Add(sampledType).Add(combined).Add(image).Add(samplerValue));
        if (instruction.NonUniform)
        {
            _spirv.Decorate(combined, spv::Decoration::NonUniform);
        }
        return combined;
    }

    uint32 ImageResult(const MirInstruction &instruction, const MirType *texel, uint32 raw)
    {
        const uint32 id = Value(&instruction);
        const MirType *scalar = texel->GetScalar();
        const uint32 count = texel->Is(MirTypeKind::Vector) ? texel->GetCount() : 1;
        MirTypeContext &types = _module->GetTypes();
        const MirType *wideScalar = scalar->Is(MirTypeKind::Float) ? types.GetFloat(32) : types.GetInt(32, scalar->IsSigned());
        const MirType *narrowWide = count == 1 ? wideScalar : types.GetVector(wideScalar, count);
        uint32 value = raw;
        if (count < 4)
        {
            value = _spirv.NewId();
            if (count == 1)
            {
                Emit(SpirvInstruction(Op::OpCompositeExtract).Add(Type(narrowWide)).Add(value).Add(raw).Add(0u));
            }
            else
            {
                SpirvInstruction shuffle(Op::OpVectorShuffle);
                shuffle.Add(Type(narrowWide)).Add(value).Add(raw).Add(raw);
                for (uint32 i = 0; i < count; ++i)
                {
                    shuffle.Add(i);
                }
                Emit(shuffle);
            }
        }
        if (narrowWide == texel)
        {
            Emit(SpirvInstruction(Op::OpCopyObject).Add(Type(texel)).Add(id).Add(value));
        }
        else
        {
            Emit(SpirvInstruction(scalar->Is(MirTypeKind::Float) ? Op::OpFConvert : (scalar->IsSigned() ? Op::OpSConvert : Op::OpUConvert))
                     .Add(Type(texel))
                     .Add(id)
                     .Add(value));
        }
        return id;
    }

    /// Widens a texel to the 4-component vector of 32-bit components an image write takes.
    /// The component type images of *scalar* texels are read and written as: 32 bits, or 64 for 64-bit integers.
    const MirType *SampledScalar(const MirType *scalar)
    {
        MirTypeContext &types = _module->GetTypes();
        if (scalar->Is(MirTypeKind::Float))
        {
            return types.GetFloat(32);
        }
        return types.GetInt(scalar->GetWidth() == 64 ? 64 : 32, scalar->IsSigned());
    }

    uint32 WideTexel(const MirValue *value)
    {
        const MirType *type = value->GetType();
        const MirType *scalar = type->GetScalar();
        MirTypeContext &types = _module->GetTypes();
        const MirType *wideScalar = SampledScalar(scalar);
        const uint32 count = type->Is(MirTypeKind::Vector) ? type->GetCount() : 1;
        uint32 id = Value(value);
        if (wideScalar != scalar)
        {
            const uint32 converted = _spirv.NewId();
            const MirType *wide = count == 1 ? wideScalar : types.GetVector(wideScalar, count);
            Emit(SpirvInstruction(scalar->Is(MirTypeKind::Float) ? Op::OpFConvert : (scalar->IsSigned() ? Op::OpSConvert : Op::OpUConvert))
                     .Add(Type(wide))
                     .Add(converted)
                     .Add(id));
            id = converted;
        }
        return id;
    }

    void EmitIntrinsic(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const MirType *type = instruction.GetType();
        auto glsl = [&](GLSLstd450 operation) {
            List<uint32> arguments;
            for (const MirValue *operand : operands)
            {
                arguments.Add(Value(operand));
            }
            Extended(instruction, static_cast<uint32>(operation), arguments.AsSpan());
        };
        auto unaryOp = [&](Op op) {
            Emit(SpirvInstruction(op).Add(Type(type)).Add(Value(&instruction)).Add(Value(operands[0])));
        };
        const bool isFloat = IsFloat(operands.IsEmpty() ? type : operands[0]->GetType());
        const bool isSigned = IsSigned(operands.IsEmpty() ? type : operands[0]->GetType());
        const uint32 subgroup = UintConstant(static_cast<uint32>(spv::Scope::Subgroup));
        auto groupOp = [&](Op op, spv::Capability capability, bool reduce) {
            _spirv.Capability(spv::Capability::GroupNonUniform);
            _spirv.Capability(capability);
            SpirvInstruction group(op);
            group.Add(Type(type)).Add(Value(&instruction)).Add(subgroup);
            if (reduce)
            {
                group.Add(spv::GroupOperation::Reduce);
            }
            for (const MirValue *operand : operands)
            {
                group.Add(Value(operand));
            }
            Emit(group);
        };
        auto groupScan = [&](Op op, spv::GroupOperation operation) {
            _spirv.Capability(spv::Capability::GroupNonUniform);
            _spirv.Capability(spv::Capability::GroupNonUniformArithmetic);
            Emit(SpirvInstruction(op).Add(Type(type)).Add(Value(&instruction)).Add(subgroup).Add(operation).Add(Value(operands[0])));
        };
        auto ballotCount = [&](spv::GroupOperation operation) {
            _spirv.Capability(spv::Capability::GroupNonUniform);
            _spirv.Capability(spv::Capability::GroupNonUniformBallot);
            MirTypeContext &types = _module->GetTypes();
            const uint32 ballot = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpGroupNonUniformBallot)
                     .Add(Type(types.GetVector(types.GetInt(32, false), 4)))
                     .Add(ballot)
                     .Add(subgroup)
                     .Add(Value(operands[0])));
            Emit(SpirvInstruction(Op::OpGroupNonUniformBallotBitCount).Add(Type(type)).Add(Value(&instruction)).Add(subgroup).Add(operation).Add(ballot));
        };
        auto barrier = [&](bool control, spv::Scope scope, uint32 semantics) {
            const uint32 scopeId = UintConstant(static_cast<uint32>(scope));
            const uint32 semanticsId = UintConstant(semantics);
            if (control)
            {
                Emit(SpirvInstruction(Op::OpControlBarrier).Add(UintConstant(static_cast<uint32>(spv::Scope::Workgroup))).Add(scopeId).Add(semanticsId));
            }
            else
            {
                Emit(SpirvInstruction(Op::OpMemoryBarrier).Add(scopeId).Add(semanticsId));
            }
        };
        const uint32 acquireRelease = Bits(spv::MemorySemanticsMask::AcquireRelease);
        auto atomic = [&](Op signedOp, Op unsignedOp) {
            const MirValue *pointer = operands[0];
            const bool workgroup = pointer->GetType()->GetAddressSpace() == MirAddressSpace::GroupShared;
            const uint32 scope = UintConstant(static_cast<uint32>(workgroup ? spv::Scope::Workgroup : spv::Scope::Device));
            const uint32 semantics = UintConstant(0);
            SpirvInstruction op(isSigned ? signedOp : unsignedOp);
            op.Add(Type(type)).Add(Value(&instruction)).Add(Value(pointer)).Add(scope).Add(semantics);
            for (usize i = 1; i < operands.Count(); ++i)
            {
                op.Add(Value(operands[i]));
            }
            Emit(op);
            if (type->GetWidth() == 64)
            {
                _spirv.Capability(spv::Capability::Int64Atomics);
            }
        };

        switch (instruction.GetIntrinsic())
        {
        case MirIntrinsic::Abs:
            glsl(isFloat ? GLSLstd450FAbs : GLSLstd450SAbs);
            if (!isFloat && !isSigned)
            {
                Error("abs of an unsigned integer");
            }
            break;
        case MirIntrinsic::Sign:
            glsl(isFloat ? GLSLstd450FSign : GLSLstd450SSign);
            break;
        case MirIntrinsic::Floor:
            glsl(GLSLstd450Floor);
            break;
        case MirIntrinsic::Ceil:
            glsl(GLSLstd450Ceil);
            break;
        case MirIntrinsic::Round:
            glsl(GLSLstd450RoundEven);
            break;
        case MirIntrinsic::Trunc:
            glsl(GLSLstd450Trunc);
            break;
        case MirIntrinsic::Fract:
            glsl(GLSLstd450Fract);
            break;
        case MirIntrinsic::Sqrt:
            glsl(GLSLstd450Sqrt);
            break;
        case MirIntrinsic::InverseSqrt:
            glsl(GLSLstd450InverseSqrt);
            break;
        case MirIntrinsic::Exp:
            glsl(GLSLstd450Exp);
            break;
        case MirIntrinsic::Exp2:
            glsl(GLSLstd450Exp2);
            break;
        case MirIntrinsic::Log:
            glsl(GLSLstd450Log);
            break;
        case MirIntrinsic::Log2:
            glsl(GLSLstd450Log2);
            break;
        case MirIntrinsic::Pow:
            glsl(GLSLstd450Pow);
            break;
        case MirIntrinsic::Sin:
            glsl(GLSLstd450Sin);
            break;
        case MirIntrinsic::Cos:
            glsl(GLSLstd450Cos);
            break;
        case MirIntrinsic::Tan:
            glsl(GLSLstd450Tan);
            break;
        case MirIntrinsic::Asin:
            glsl(GLSLstd450Asin);
            break;
        case MirIntrinsic::Acos:
            glsl(GLSLstd450Acos);
            break;
        case MirIntrinsic::Atan:
            glsl(GLSLstd450Atan);
            break;
        case MirIntrinsic::Atan2:
            glsl(GLSLstd450Atan2);
            break;
        case MirIntrinsic::Sinh:
            glsl(GLSLstd450Sinh);
            break;
        case MirIntrinsic::Cosh:
            glsl(GLSLstd450Cosh);
            break;
        case MirIntrinsic::Tanh:
            glsl(GLSLstd450Tanh);
            break;
        case MirIntrinsic::Min:
            glsl(isFloat ? GLSLstd450FMin : (isSigned ? GLSLstd450SMin : GLSLstd450UMin));
            break;
        case MirIntrinsic::Max:
            glsl(isFloat ? GLSLstd450FMax : (isSigned ? GLSLstd450SMax : GLSLstd450UMax));
            break;
        case MirIntrinsic::Clamp:
            glsl(isFloat ? GLSLstd450FClamp : (isSigned ? GLSLstd450SClamp : GLSLstd450UClamp));
            break;
        case MirIntrinsic::Lerp:
            glsl(GLSLstd450FMix);
            break;
        case MirIntrinsic::Step:
            glsl(GLSLstd450Step);
            break;
        case MirIntrinsic::SmoothStep:
            glsl(GLSLstd450SmoothStep);
            break;
        case MirIntrinsic::Fma:
            glsl(GLSLstd450Fma);
            break;
        case MirIntrinsic::Dot:
            Emit(SpirvInstruction(Op::OpDot).Add(Type(type)).Add(Value(&instruction)).Add(Value(operands[0])).Add(Value(operands[1])));
            break;
        case MirIntrinsic::Cross:
            glsl(GLSLstd450Cross);
            break;
        case MirIntrinsic::Length:
            glsl(GLSLstd450Length);
            break;
        case MirIntrinsic::Distance:
            glsl(GLSLstd450Distance);
            break;
        case MirIntrinsic::Normalize:
            glsl(GLSLstd450Normalize);
            break;
        case MirIntrinsic::Reflect:
            glsl(GLSLstd450Reflect);
            break;
        case MirIntrinsic::Refract:
            glsl(GLSLstd450Refract);
            break;
        case MirIntrinsic::FaceForward:
            glsl(GLSLstd450FaceForward);
            break;
        case MirIntrinsic::Determinant:
            glsl(GLSLstd450Determinant);
            break;
        case MirIntrinsic::Inverse:
            glsl(GLSLstd450MatrixInverse);
            break;
        case MirIntrinsic::Transpose:
            unaryOp(Op::OpTranspose);
            break;
        case MirIntrinsic::IsNan:
            unaryOp(Op::OpIsNan);
            break;
        case MirIntrinsic::IsInf:
            unaryOp(Op::OpIsInf);
            break;
        case MirIntrinsic::Any:
            unaryOp(Op::OpAny);
            break;
        case MirIntrinsic::All:
            unaryOp(Op::OpAll);
            break;
        case MirIntrinsic::CountBits:
            unaryOp(Op::OpBitCount);
            break;
        case MirIntrinsic::ReverseBits:
            unaryOp(Op::OpBitReverse);
            break;
        case MirIntrinsic::FirstBitHigh:
            glsl(isSigned ? GLSLstd450FindSMsb : GLSLstd450FindUMsb);
            break;
        case MirIntrinsic::FirstBitLow:
            glsl(GLSLstd450FindILsb);
            break;
        case MirIntrinsic::PackHalf2x16:
            glsl(GLSLstd450PackHalf2x16);
            break;
        case MirIntrinsic::UnpackHalf2x16:
            glsl(GLSLstd450UnpackHalf2x16);
            break;
        case MirIntrinsic::Ddx:
            unaryOp(Op::OpDPdx);
            break;
        case MirIntrinsic::Ddy:
            unaryOp(Op::OpDPdy);
            break;
        case MirIntrinsic::DdxFine:
            _spirv.Capability(spv::Capability::DerivativeControl);
            unaryOp(Op::OpDPdxFine);
            break;
        case MirIntrinsic::DdyFine:
            _spirv.Capability(spv::Capability::DerivativeControl);
            unaryOp(Op::OpDPdyFine);
            break;
        case MirIntrinsic::DdxCoarse:
            _spirv.Capability(spv::Capability::DerivativeControl);
            unaryOp(Op::OpDPdxCoarse);
            break;
        case MirIntrinsic::DdyCoarse:
            _spirv.Capability(spv::Capability::DerivativeControl);
            unaryOp(Op::OpDPdyCoarse);
            break;
        case MirIntrinsic::Fwidth:
            unaryOp(Op::OpFwidth);
            break;
        case MirIntrinsic::Sample:
        case MirIntrinsic::SampleBias:
        case MirIntrinsic::SampleLevel:
        case MirIntrinsic::SampleGrad:
        {
            const uint32 image = SampledImage(instruction, operands[0], operands[1], false);
            const MirType *texel = operands[0]->GetType()->GetElement();
            const MirType *scalar = texel->GetScalar();
            MirTypeContext &types = _module->GetTypes();
            const MirType *wide = types.GetVector(SampledScalar(scalar), 4);
            const uint32 raw = _spirv.NewId();
            const bool explicitLod = instruction.GetIntrinsic() == MirIntrinsic::SampleLevel ||
                                     instruction.GetIntrinsic() == MirIntrinsic::SampleGrad;
            SpirvInstruction sample(explicitLod ? Op::OpImageSampleExplicitLod : Op::OpImageSampleImplicitLod);
            sample.Add(Type(wide)).Add(raw).Add(image).Add(Value(operands[2]));
            switch (instruction.GetIntrinsic())
            {
            case MirIntrinsic::SampleBias:
                sample.Add(spv::ImageOperandsMask::Bias).Add(Value(operands[3]));
                break;
            case MirIntrinsic::SampleLevel:
                sample.Add(spv::ImageOperandsMask::Lod).Add(Value(operands[3]));
                break;
            case MirIntrinsic::SampleGrad:
                sample.Add(spv::ImageOperandsMask::Grad).Add(Value(operands[3])).Add(Value(operands[4]));
                break;
            default:
                break;
            }
            Emit(sample);
            ImageResult(instruction, texel, raw);
            break;
        }
        case MirIntrinsic::SampleCmp:
        case MirIntrinsic::SampleCmpLevel:
        {
            const uint32 image = SampledImage(instruction, operands[0], operands[1], true);
            const bool level = instruction.GetIntrinsic() == MirIntrinsic::SampleCmpLevel;
            SpirvInstruction sample(level ? Op::OpImageSampleDrefExplicitLod : Op::OpImageSampleDrefImplicitLod);
            sample.Add(Type(type)).Add(Value(&instruction)).Add(image).Add(Value(operands[2])).Add(Value(operands[3]));
            if (level)
            {
                sample.Add(spv::ImageOperandsMask::Lod).Add(Value(operands[4]));
            }
            Emit(sample);
            break;
        }
        case MirIntrinsic::Gather:
        {
            const uint32 image = SampledImage(instruction, operands[0], operands[1], false);
            const MirType *scalar = type->GetScalar();
            MirTypeContext &types = _module->GetTypes();
            const MirType *wide = types.GetVector(SampledScalar(scalar), 4);
            const uint32 raw = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpImageGather).Add(Type(wide)).Add(raw).Add(image).Add(Value(operands[2])).Add(Value(operands[3])));
            ImageResult(instruction, type, raw);
            break;
        }
        case MirIntrinsic::GatherCmp:
        {
            const uint32 image = SampledImage(instruction, operands[0], operands[1], true);
            Emit(SpirvInstruction(Op::OpImageDrefGather).Add(Type(type)).Add(Value(&instruction)).Add(image).Add(Value(operands[2])).Add(Value(operands[3])));
            break;
        }
        case MirIntrinsic::TextureLoad:
        {
            const MirType *textureType = operands[0]->GetType();
            const uint32 image = LoadDescriptor(operands[0], ImageType(textureType, false), false, instruction.NonUniform);
            const MirType *texel = textureType->GetElement();
            const MirType *scalar = texel->GetScalar();
            MirTypeContext &types = _module->GetTypes();
            const MirType *wide = types.GetVector(SampledScalar(scalar), 4);
            const uint32 raw = _spirv.NewId();
            if (textureType->IsWritable())
            {
                Emit(SpirvInstruction(Op::OpImageRead).Add(Type(wide)).Add(raw).Add(image).Add(Value(operands[1])));
            }
            else
            {
                SpirvInstruction fetch(Op::OpImageFetch);
                fetch.Add(Type(wide)).Add(raw).Add(image).Add(Value(operands[1]));
                if (operands.Count() > 2)
                {
                    fetch.Add(textureType->IsMultisampled() ? spv::ImageOperandsMask::Sample : spv::ImageOperandsMask::Lod).Add(Value(operands[2]));
                }
                Emit(fetch);
            }
            ImageResult(instruction, texel, raw);
            break;
        }
        case MirIntrinsic::TextureStore:
        {
            const MirType *textureType = operands[0]->GetType();
            const uint32 image = LoadDescriptor(operands[0], ImageType(textureType, false), false, instruction.NonUniform);
            Emit(SpirvInstruction(Op::OpImageWrite).Add(image).Add(Value(operands[1])).Add(WideTexel(operands[2])));
            break;
        }
        case MirIntrinsic::TextureDimensions:
        {
            _spirv.Capability(spv::Capability::ImageQuery);
            const MirType *textureType = operands[0]->GetType();
            const uint32 image = LoadDescriptor(operands[0], ImageType(textureType, false), false, instruction.NonUniform);
            if (textureType->IsWritable() || textureType->IsMultisampled())
            {
                Emit(SpirvInstruction(Op::OpImageQuerySize).Add(Type(type)).Add(Value(&instruction)).Add(image));
            }
            else
            {
                // The Size property asks for mip 0; GetSize(mip) for any.
                const uint32 mip = operands.Count() > 1 ? Value(operands[1]) : UintConstant(0);
                Emit(SpirvInstruction(Op::OpImageQuerySizeLod).Add(Type(type)).Add(Value(&instruction)).Add(image).Add(mip));
            }
            break;
        }
        case MirIntrinsic::TextureLevels:
        case MirIntrinsic::TextureSamples:
        {
            _spirv.Capability(spv::Capability::ImageQuery);
            const MirType *textureType = operands[0]->GetType();
            const uint32 image = LoadDescriptor(operands[0], ImageType(textureType, false), false, instruction.NonUniform);
            Emit(SpirvInstruction(instruction.GetIntrinsic() == MirIntrinsic::TextureLevels ? Op::OpImageQueryLevels : Op::OpImageQuerySamples)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(image));
            break;
        }
        case MirIntrinsic::CalculateLod:
        {
            _spirv.Capability(spv::Capability::ImageQuery);
            const uint32 image = SampledImage(instruction, operands[0], operands[1], false);
            MirTypeContext &types = _module->GetTypes();
            const uint32 both = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpImageQueryLod).Add(Type(types.GetVector(types.GetFloat(32), 2))).Add(both).Add(image).Add(Value(operands[2])));
            Emit(SpirvInstruction(Op::OpCompositeExtract).Add(Type(type)).Add(Value(&instruction)).Add(both).Add(1u));
            break;
        }
        case MirIntrinsic::AtomicLoad:
        {
            const bool workgroup = operands[0]->GetType()->GetAddressSpace() == MirAddressSpace::GroupShared;
            if (type->GetWidth() == 64)
            {
                _spirv.Capability(spv::Capability::Int64Atomics);
            }
            Emit(SpirvInstruction(Op::OpAtomicLoad)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(Value(operands[0]))
                     .Add(UintConstant(static_cast<uint32>(workgroup ? spv::Scope::Workgroup : spv::Scope::Device)))
                     .Add(UintConstant(0)));
            break;
        }
        case MirIntrinsic::AtomicStore:
        {
            const bool workgroup = operands[0]->GetType()->GetAddressSpace() == MirAddressSpace::GroupShared;
            if (operands[1]->GetType()->GetWidth() == 64)
            {
                _spirv.Capability(spv::Capability::Int64Atomics);
            }
            Emit(SpirvInstruction(Op::OpAtomicStore)
                     .Add(Value(operands[0]))
                     .Add(UintConstant(static_cast<uint32>(workgroup ? spv::Scope::Workgroup : spv::Scope::Device)))
                     .Add(UintConstant(0))
                     .Add(Value(operands[1])));
            break;
        }
        case MirIntrinsic::AtomicExchange:
            atomic(Op::OpAtomicExchange, Op::OpAtomicExchange);
            break;
        case MirIntrinsic::AtomicCompareExchange:
        {
            const bool workgroup = operands[0]->GetType()->GetAddressSpace() == MirAddressSpace::GroupShared;
            const uint32 scope = UintConstant(static_cast<uint32>(workgroup ? spv::Scope::Workgroup : spv::Scope::Device));
            Emit(SpirvInstruction(Op::OpAtomicCompareExchange)
                     .Add(Type(type))
                     .Add(Value(&instruction))
                     .Add(Value(operands[0]))
                     .Add(scope)
                     .Add(UintConstant(0))
                     .Add(UintConstant(0))
                     .Add(Value(operands[2]))
                     .Add(Value(operands[1])));
            break;
        }
        case MirIntrinsic::AtomicAdd:
            atomic(Op::OpAtomicIAdd, Op::OpAtomicIAdd);
            break;
        case MirIntrinsic::AtomicMin:
            atomic(Op::OpAtomicSMin, Op::OpAtomicUMin);
            break;
        case MirIntrinsic::AtomicMax:
            atomic(Op::OpAtomicSMax, Op::OpAtomicUMax);
            break;
        case MirIntrinsic::AtomicAnd:
            atomic(Op::OpAtomicAnd, Op::OpAtomicAnd);
            break;
        case MirIntrinsic::AtomicOr:
            atomic(Op::OpAtomicOr, Op::OpAtomicOr);
            break;
        case MirIntrinsic::AtomicXor:
            atomic(Op::OpAtomicXor, Op::OpAtomicXor);
            break;
        case MirIntrinsic::GroupBarrier:
            barrier(true, spv::Scope::Workgroup, acquireRelease | Bits(spv::MemorySemanticsMask::WorkgroupMemory));
            break;
        case MirIntrinsic::GroupMemoryBarrier:
            barrier(false, spv::Scope::Workgroup, acquireRelease | Bits(spv::MemorySemanticsMask::WorkgroupMemory));
            break;
        case MirIntrinsic::DeviceMemoryBarrier:
            barrier(false, spv::Scope::Device,
                    acquireRelease | Bits(spv::MemorySemanticsMask::UniformMemory) | Bits(spv::MemorySemanticsMask::ImageMemory));
            break;
        case MirIntrinsic::AllMemoryBarrier:
            barrier(false, spv::Scope::Device,
                    acquireRelease | Bits(spv::MemorySemanticsMask::UniformMemory) | Bits(spv::MemorySemanticsMask::ImageMemory) |
                        Bits(spv::MemorySemanticsMask::WorkgroupMemory));
            break;
        case MirIntrinsic::WaveIsFirstLane:
            _spirv.Capability(spv::Capability::GroupNonUniform);
            Emit(SpirvInstruction(Op::OpGroupNonUniformElect).Add(Type(type)).Add(Value(&instruction)).Add(subgroup));
            break;
        case MirIntrinsic::WaveActiveAllTrue:
            groupOp(Op::OpGroupNonUniformAll, spv::Capability::GroupNonUniformVote, false);
            break;
        case MirIntrinsic::WaveActiveAnyTrue:
            groupOp(Op::OpGroupNonUniformAny, spv::Capability::GroupNonUniformVote, false);
            break;
        case MirIntrinsic::WaveActiveAllEqual:
            groupOp(Op::OpGroupNonUniformAllEqual, spv::Capability::GroupNonUniformVote, false);
            break;
        case MirIntrinsic::WaveActiveBallot:
            groupOp(Op::OpGroupNonUniformBallot, spv::Capability::GroupNonUniformBallot, false);
            break;
        case MirIntrinsic::WaveReadLaneAt:
            groupOp(Op::OpGroupNonUniformShuffle, spv::Capability::GroupNonUniformShuffle, false);
            break;
        case MirIntrinsic::WaveReadFirstLane:
            groupOp(Op::OpGroupNonUniformBroadcastFirst, spv::Capability::GroupNonUniformBallot, false);
            break;
        case MirIntrinsic::WaveActiveSum:
            groupOp(isFloat ? Op::OpGroupNonUniformFAdd : Op::OpGroupNonUniformIAdd, spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveProduct:
            groupOp(isFloat ? Op::OpGroupNonUniformFMul : Op::OpGroupNonUniformIMul, spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveMin:
            groupOp(isFloat ? Op::OpGroupNonUniformFMin : (isSigned ? Op::OpGroupNonUniformSMin : Op::OpGroupNonUniformUMin),
                    spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveMax:
            groupOp(isFloat ? Op::OpGroupNonUniformFMax : (isSigned ? Op::OpGroupNonUniformSMax : Op::OpGroupNonUniformUMax),
                    spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveBitAnd:
            groupOp(Op::OpGroupNonUniformBitwiseAnd, spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveBitOr:
            groupOp(Op::OpGroupNonUniformBitwiseOr, spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveBitXor:
            groupOp(Op::OpGroupNonUniformBitwiseXor, spv::Capability::GroupNonUniformArithmetic, true);
            break;
        case MirIntrinsic::WaveActiveCountBits:
            ballotCount(spv::GroupOperation::Reduce);
            break;
        case MirIntrinsic::WavePrefixSum:
            groupScan(isFloat ? Op::OpGroupNonUniformFAdd : Op::OpGroupNonUniformIAdd, spv::GroupOperation::ExclusiveScan);
            break;
        case MirIntrinsic::WavePrefixProduct:
            groupScan(isFloat ? Op::OpGroupNonUniformFMul : Op::OpGroupNonUniformIMul, spv::GroupOperation::ExclusiveScan);
            break;
        case MirIntrinsic::WavePrefixCountBits:
            ballotCount(spv::GroupOperation::ExclusiveScan);
            break;
        case MirIntrinsic::SetMeshOutputCounts:
            Emit(SpirvInstruction(Op::OpSetMeshOutputsEXT).Add(Value(operands[0])).Add(Value(operands[1])));
            break;
        case MirIntrinsic::RayQueryInitialize:
        case MirIntrinsic::RayQueryProceed:
        case MirIntrinsic::RayQueryAbort:
        case MirIntrinsic::RayQueryConfirmIntersection:
        case MirIntrinsic::RayQueryGenerateIntersection:
        case MirIntrinsic::RayQueryGet:
            EmitRayQuery(instruction);
            break;
        case MirIntrinsic::TraceRay:
        case MirIntrinsic::CallShader:
        case MirIntrinsic::ReportHit:
        case MirIntrinsic::HitObjectTraceRay:
        case MirIntrinsic::HitObjectFromRayQuery:
        case MirIntrinsic::HitObjectMakeMiss:
        case MirIntrinsic::HitObjectMakeNop:
        case MirIntrinsic::HitObjectInvoke:
        case MirIntrinsic::HitObjectGet:
        case MirIntrinsic::ReorderThread:
            EmitRayTracing(instruction);
            break;
        case MirIntrinsic::CoopVectorLoad:
        case MirIntrinsic::CoopVectorStore:
        case MirIntrinsic::CoopVectorMatMul:
        case MirIntrinsic::CoopVectorMatMulAdd:
        case MirIntrinsic::CoopVectorOuterProductAccumulate:
        case MirIntrinsic::CoopVectorReduceSumAccumulate:
        case MirIntrinsic::CoopMatrixLoad:
        case MirIntrinsic::CoopMatrixStore:
        case MirIntrinsic::CoopMatrixMulAdd:
        case MirIntrinsic::CoopMatrixLength:
            EmitCooperative(instruction);
            break;
        case MirIntrinsic::DebugDeclare:
            if (_sourceDebugInfo && _debugFunction != 0)
            {
                EmitDebugDeclare(instruction);
            }
            break;
        default:
            Error("{0} has no SPIR-V form yet", GetMirIntrinsicName(instruction.GetIntrinsic()));
            break;
        }
    }

    void EmitInstruction(const MirInstruction &instruction)
    {
        if (!instruction.IsTerminator() && instruction.GetOp() != MirOp::Phi)
        {
            SetLine(instruction.Location);
        }
        const Span<MirValue *const> operands = instruction.GetOperands();
        const MirType *type = instruction.GetType();
        auto result = [&] { return Value(&instruction); };
        auto binary = [&](Op op) {
            Emit(SpirvInstruction(op).Add(Type(type)).Add(result()).Add(Value(operands[0])).Add(Value(operands[1])));
        };
        const MirType *operandType = operands.IsEmpty() ? type : operands[0]->GetType();
        const bool isFloat = IsFloat(operandType);
        const bool isSigned = IsSigned(operandType);
        const bool isBool = IsBool(operandType);
        if (instruction.Precise && isFloat)
        {
            switch (instruction.GetOp())
            {
            case MirOp::Add:
            case MirOp::Sub:
            case MirOp::Mul:
            case MirOp::Div:
            case MirOp::Rem:
            case MirOp::Negate:
            case MirOp::MatrixTimesVector:
            case MirOp::VectorTimesMatrix:
            case MirOp::MatrixTimesMatrix:
            case MirOp::MatrixTimesScalar:
                _spirv.Decorate(result(), spv::Decoration::NoContraction);
                break;
            default:
                break;
            }
        }
        switch (instruction.GetOp())
        {
        case MirOp::Add:
            binary(isFloat ? Op::OpFAdd : Op::OpIAdd);
            break;
        case MirOp::Sub:
            binary(isFloat ? Op::OpFSub : Op::OpISub);
            break;
        case MirOp::Mul:
            binary(isFloat ? Op::OpFMul : Op::OpIMul);
            break;
        case MirOp::Div:
            binary(isFloat ? Op::OpFDiv : (isSigned ? Op::OpSDiv : Op::OpUDiv));
            break;
        case MirOp::Rem:
            binary(isFloat ? Op::OpFRem : (isSigned ? Op::OpSRem : Op::OpUMod));
            break;
        case MirOp::Negate:
            Emit(SpirvInstruction(isFloat ? Op::OpFNegate : Op::OpSNegate).Add(Type(type)).Add(result()).Add(Value(operands[0])));
            break;
        case MirOp::BitAnd:
            binary(Op::OpBitwiseAnd);
            break;
        case MirOp::BitOr:
            binary(Op::OpBitwiseOr);
            break;
        case MirOp::BitXor:
            binary(Op::OpBitwiseXor);
            break;
        case MirOp::BitNot:
            Emit(SpirvInstruction(Op::OpNot).Add(Type(type)).Add(result()).Add(Value(operands[0])));
            break;
        case MirOp::ShiftLeft:
            binary(Op::OpShiftLeftLogical);
            break;
        case MirOp::ShiftRight:
            binary(isSigned ? Op::OpShiftRightArithmetic : Op::OpShiftRightLogical);
            break;
        case MirOp::LogicalAnd:
            binary(Op::OpLogicalAnd);
            break;
        case MirOp::LogicalOr:
            binary(Op::OpLogicalOr);
            break;
        case MirOp::LogicalNot:
            Emit(SpirvInstruction(Op::OpLogicalNot).Add(Type(type)).Add(result()).Add(Value(operands[0])));
            break;
        case MirOp::Equal:
            binary(isBool ? Op::OpLogicalEqual : (isFloat ? Op::OpFOrdEqual : Op::OpIEqual));
            break;
        case MirOp::NotEqual:
            binary(isBool ? Op::OpLogicalNotEqual : (isFloat ? Op::OpFUnordNotEqual : Op::OpINotEqual));
            break;
        case MirOp::Less:
            binary(isFloat ? Op::OpFOrdLessThan : (isSigned ? Op::OpSLessThan : Op::OpULessThan));
            break;
        case MirOp::LessEqual:
            binary(isFloat ? Op::OpFOrdLessThanEqual : (isSigned ? Op::OpSLessThanEqual : Op::OpULessThanEqual));
            break;
        case MirOp::Greater:
            binary(isFloat ? Op::OpFOrdGreaterThan : (isSigned ? Op::OpSGreaterThan : Op::OpUGreaterThan));
            break;
        case MirOp::GreaterEqual:
            binary(isFloat ? Op::OpFOrdGreaterThanEqual : (isSigned ? Op::OpSGreaterThanEqual : Op::OpUGreaterThanEqual));
            break;
        case MirOp::MatrixTimesVector:
            Emit(SpirvInstruction(Op::OpVectorTimesMatrix).Add(Type(type)).Add(result()).Add(Value(operands[1])).Add(Value(operands[0])));
            break;
        case MirOp::VectorTimesMatrix:
            Emit(SpirvInstruction(Op::OpMatrixTimesVector).Add(Type(type)).Add(result()).Add(Value(operands[1])).Add(Value(operands[0])));
            break;
        case MirOp::MatrixTimesMatrix:
            Emit(SpirvInstruction(Op::OpMatrixTimesMatrix).Add(Type(type)).Add(result()).Add(Value(operands[1])).Add(Value(operands[0])));
            break;
        case MirOp::MatrixTimesScalar:
            binary(Op::OpMatrixTimesScalar);
            break;
        case MirOp::VectorTimesScalar:
            binary(Op::OpVectorTimesScalar);
            break;
        case MirOp::Convert:
        {
            const MirType *from = operandType->GetScalar();
            const MirType *to = type->GetScalar();
            Op op = Op::OpCopyObject;
            if (from->Is(MirTypeKind::Float) && to->Is(MirTypeKind::Float))
            {
                op = Op::OpFConvert;
            }
            else if (from->Is(MirTypeKind::Float))
            {
                op = to->IsSigned() ? Op::OpConvertFToS : Op::OpConvertFToU;
            }
            else if (to->Is(MirTypeKind::Float))
            {
                op = from->IsSigned() ? Op::OpConvertSToF : Op::OpConvertUToF;
            }
            else if (from->GetWidth() == to->GetWidth())
            {
                op = Op::OpBitcast;
            }
            else
            {
                op = from->IsSigned() ? Op::OpSConvert : Op::OpUConvert;
            }
            if (op == Op::OpSConvert || op == Op::OpUConvert)
            {
                MirTypeContext &types = _module->GetTypes();
                const MirType *sameSign = types.GetInt(to->GetWidth(), from->IsSigned());
                const MirType *sameSignType = type->Is(MirTypeKind::Vector) ? types.GetVector(sameSign, type->GetCount()) : sameSign;
                if (sameSignType != type)
                {
                    const uint32 widened = _spirv.NewId();
                    Emit(SpirvInstruction(op).Add(Type(sameSignType)).Add(widened).Add(Value(operands[0])));
                    Emit(SpirvInstruction(Op::OpBitcast).Add(Type(type)).Add(result()).Add(widened));
                    break;
                }
            }
            Emit(SpirvInstruction(op).Add(Type(type)).Add(result()).Add(Value(operands[0])));
            break;
        }
        case MirOp::Bitcast:
            if (type->IsHandle() || operands[0]->GetType()->IsHandle())
            {
                Emit(SpirvInstruction(Op::OpCopyObject).Add(Type(type)).Add(result()).Add(Value(operands[0])));
                break;
            }
            Emit(SpirvInstruction(Op::OpBitcast).Add(Type(type)).Add(result()).Add(Value(operands[0])));
            break;
        case MirOp::CompositeConstruct:
        {
            SpirvInstruction construct(Op::OpCompositeConstruct);
            construct.Add(Type(type)).Add(result());
            for (const MirValue *operand : operands)
            {
                construct.Add(Value(operand));
                if (type->Is(MirTypeKind::CoopMatrix))
                {
                    break; // One value for all the elements.
                }
            }
            Emit(construct);
            break;
        }
        case MirOp::CompositeExtract:
        {
            SpirvInstruction extract(Op::OpCompositeExtract);
            extract.Add(Type(type)).Add(result()).Add(Value(operands[0]));
            for (const int64 literal : instruction.GetLiterals())
            {
                extract.Add(static_cast<uint32>(literal));
            }
            Emit(extract);
            break;
        }
        case MirOp::CompositeInsert:
        {
            SpirvInstruction insert(Op::OpCompositeInsert);
            insert.Add(Type(type)).Add(result()).Add(Value(operands[1])).Add(Value(operands[0]));
            for (const int64 literal : instruction.GetLiterals())
            {
                insert.Add(static_cast<uint32>(literal));
            }
            Emit(insert);
            break;
        }
        case MirOp::VectorExtractDynamic:
            binary(Op::OpVectorExtractDynamic);
            break;
        case MirOp::VectorInsertDynamic:
            Emit(SpirvInstruction(Op::OpVectorInsertDynamic).Add(Type(type)).Add(result()).Add(Value(operands[0])).Add(Value(operands[1])).Add(Value(operands[2])));
            break;
        case MirOp::VectorShuffle:
            if (!type->Is(MirTypeKind::Vector))
            {
                const auto component = static_cast<uint32>(instruction.GetLiterals()[0]);
                const uint32 firstCount = operands[0]->GetType()->GetCount();
                Emit(SpirvInstruction(Op::OpCompositeExtract)
                         .Add(Type(type))
                         .Add(result())
                         .Add(Value(operands[component < firstCount ? 0 : 1]))
                         .Add(component < firstCount ? component : component - firstCount));
            }
            else
            {
                SpirvInstruction shuffle(Op::OpVectorShuffle);
                shuffle.Add(Type(type)).Add(result()).Add(Value(operands[0])).Add(Value(operands[1]));
                for (const int64 component : instruction.GetLiterals())
                {
                    shuffle.Add(static_cast<uint32>(component));
                }
                Emit(shuffle);
            }
            break;
        case MirOp::Select:
            Emit(SpirvInstruction(Op::OpSelect).Add(Type(type)).Add(result()).Add(Value(operands[0])).Add(Value(operands[1])).Add(Value(operands[2])));
            break;
        case MirOp::Variable:
        {
            SpirvInstruction(Op::OpVariable).Add(Type(type)).Add(result()).Add(spv::StorageClass::Function).WriteTo(_variables);
            _spirv.Name(result(), instruction.GetName());
            break;
        }
        case MirOp::Load:
            EmitLoad(instruction);
            break;
        case MirOp::Store:
            EmitStore(instruction);
            break;
        case MirOp::AccessChain:
            EmitAccessChain(instruction);
            break;
        case MirOp::BufferPointer:
        {
            // A buffer value is the device address of its first element (§4.5, §11.3).
            const uint32 pointer = PhysicalPointer(BufferBlock(type->GetElement(), !operands[0]->GetType()->IsWritable()));
            if (!instruction.Restrict)
            {
                Emit(SpirvInstruction(Op::OpConvertUToPtr).Add(pointer).Add(result()).Add(Value(operands[0])));
                _bufferRoots.Add(&instruction);
                break;
            }
            // `[NoAlias]`: Vulkan states a pointer's aliasing on the variable that holds it, decorated RestrictPointer.
            const uint32 converted = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpConvertUToPtr).Add(pointer).Add(converted).Add(Value(operands[0])));
            const uint32 holder = _spirv.NewId();
            SpirvInstruction(Op::OpVariable)
                .Add(FunctionPointer(pointer))
                .Add(holder)
                .Add(spv::StorageClass::Function)
                .WriteTo(_variables);
            _spirv.Decorate(holder, spv::Decoration::RestrictPointer);
            Emit(SpirvInstruction(Op::OpStore).Add(holder).Add(converted));
            Emit(SpirvInstruction(Op::OpLoad).Add(pointer).Add(result()).Add(holder));
            _bufferRoots.Add(&instruction);
            break;
        }
        case MirOp::ArrayLength:
        {
            const MirValue *pointer = operands[0];
            const uint32 block = BufferBlock(pointer->GetType()->GetElement());
            Emit(SpirvInstruction(Op::OpUntypedArrayLengthKHR).Add(Type(type)).Add(result()).Add(block).Add(Value(pointer)).Add(0u));
            break;
        }
        case MirOp::Call:
        {
            SpirvInstruction call(Op::OpFunctionCall);
            call.Add(Type(type));
            call.Add(type->Is(MirTypeKind::Void) ? _spirv.NewId() : result());
            call.Add(Value(operands[0]));
            for (usize i = 1; i < operands.Count(); ++i)
            {
                call.Add(Value(operands[i]));
            }
            Emit(call);
            break;
        }
        case MirOp::Phi:
        {
            SpirvInstruction phi(Op::OpPhi);
            phi.Add(Type(type)).Add(result());
            for (usize i = 0; i < operands.Count(); ++i)
            {
                phi.Add(Value(operands[i])).Add(*_blocks.Find(instruction.GetTargets()[i]));
            }
            Emit(phi);
            break;
        }
        case MirOp::Intrinsic:
            EmitIntrinsic(instruction);
            break;
        case MirOp::Branch:
            Emit(SpirvInstruction(Op::OpBranch).Add(*_blocks.Find(instruction.GetTargets()[0])));
            break;
        case MirOp::CondBranch:
            Emit(SpirvInstruction(Op::OpBranchConditional)
                     .Add(Value(operands[0]))
                     .Add(*_blocks.Find(instruction.GetTargets()[0]))
                     .Add(*_blocks.Find(instruction.GetTargets()[1])));
            break;
        case MirOp::Switch:
        {
            SpirvInstruction branch(Op::OpSwitch);
            branch.Add(Value(operands[0])).Add(*_blocks.Find(instruction.GetTargets()[0]));
            const bool wide = operands[0]->GetType()->GetWidth() == 64;
            for (usize i = 0; i < instruction.GetLiterals().Count(); ++i)
            {
                const auto value = static_cast<uint64>(instruction.GetLiterals()[i]);
                branch.Add(static_cast<uint32>(value));
                if (wide)
                {
                    branch.Add(static_cast<uint32>(value >> 32));
                }
                branch.Add(*_blocks.Find(instruction.GetTargets()[i + 1]));
            }
            Emit(branch);
            break;
        }
        case MirOp::Return:
            if (operands.IsEmpty())
            {
                Emit(SpirvInstruction(Op::OpReturn));
            }
            else
            {
                Emit(SpirvInstruction(Op::OpReturnValue).Add(Value(operands[0])));
            }
            break;
        case MirOp::Discard:
            Emit(SpirvInstruction(Op::OpTerminateInvocation));
            break;
        case MirOp::IgnoreHit:
            Emit(SpirvInstruction(Op::OpIgnoreIntersectionKHR));
            break;
        case MirOp::AcceptHitAndEndSearch:
            Emit(SpirvInstruction(Op::OpTerminateRayKHR));
            break;
        case MirOp::EmitMeshTasks:
            Emit(SpirvInstruction(Op::OpEmitMeshTasksEXT)
                     .Add(Value(operands[0]))
                     .Add(Value(operands[1]))
                     .Add(Value(operands[2]))
                     .Add(Value(operands[3])));
            break;
        case MirOp::Unreachable:
            Emit(SpirvInstruction(Op::OpUnreachable));
            break;
        default:
            Error("{0} has no SPIR-V form yet", GetMirOpName(instruction.GetOp()));
            break;
        }
    }

    void EmitLoad(const MirInstruction &instruction)
    {
        const MirValue *pointer = instruction.GetOperand(0);
        const MirType *type = instruction.GetType();
        const MirAddressSpace space = pointer->GetType()->GetAddressSpace();
        uint32 address = Value(pointer);
        if (space == MirAddressSpace::Specialization)
        {
            Emit(SpirvInstruction(Op::OpCopyObject).Add(Type(type)).Add(Value(&instruction)).Add(address));
            return;
        }
        if (pointer->GetValueKind() == MirValueKind::Global && _argumentBlocks.Contains(address))
        {
            const uint32 member = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpAccessChain)
                     .Add(Pointer(type, StorageClassOf(space), true))
                     .Add(member)
                     .Add(address)
                     .Add(UintConstant(0)));
            address = member;
        }
        if (space == MirAddressSpace::Buffer && Contains16Bit(type))
        {
            _spirv.Capability(spv::Capability::StorageBuffer16BitAccess);
        }
        if (pointer->GetValueKind() == MirValueKind::Global &&
            DeclaredType(static_cast<const MirGlobal *>(pointer)) != type)
        {
            const uint32 loaded = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpLoad)
                     .Add(Type(DeclaredType(static_cast<const MirGlobal *>(pointer))))
                     .Add(loaded)
                     .Add(address));
            Emit(SpirvInstruction(Op::OpTranspose).Add(Type(type)).Add(Value(&instruction)).Add(loaded));
            return;
        }
        if (space == MirAddressSpace::Buffer && HasLayoutForm(type))
        {
            List<uint32> path;
            LoadMembers(Value(&instruction), pointer, path, type);
            return;
        }
        if (IsLayoutPointer(pointer->GetType()))
        {
            const uint32 loaded = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpLoad).Add(Type(type, true)).Add(loaded).Add(address));
            Emit(SpirvInstruction(Op::OpCopyLogical).Add(Type(type)).Add(Value(&instruction)).Add(loaded));
            return;
        }
        SpirvInstruction load(Op::OpLoad);
        load.Add(Type(type)).Add(Value(&instruction)).Add(address);
        Emit(space == MirAddressSpace::Buffer ? Aligned(load, type) : load);
    }

    void EmitStore(const MirInstruction &instruction)
    {
        const MirValue *pointer = instruction.GetOperand(0);
        const MirValue *value = instruction.GetOperand(1);
        uint32 stored = Value(value);
        if (pointer->GetType()->GetAddressSpace() == MirAddressSpace::Buffer && Contains16Bit(value->GetType()))
        {
            _spirv.Capability(spv::Capability::StorageBuffer16BitAccess);
        }
        if (pointer->GetType()->GetAddressSpace() == MirAddressSpace::Buffer && HasLayoutForm(value->GetType()))
        {
            List<uint32> path;
            StoreMembers(pointer, path, value->GetType(), stored);
            return;
        }
        if (IsLayoutPointer(pointer->GetType()))
        {
            const uint32 copy = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpCopyLogical).Add(Type(value->GetType(), true)).Add(copy).Add(stored));
            stored = copy;
        }
        SpirvInstruction store(Op::OpStore);
        store.Add(Value(pointer)).Add(stored);
        Emit(pointer->GetType()->GetAddressSpace() == MirAddressSpace::Buffer ? Aligned(store, value->GetType())
                                                                                : store);
    }

    /// *access* with the Aligned memory operand that loads and stores through device addresses need: the alignment
    /// of *type* by the layout rules (§10), which every element and member in GPU memory has.
    static SpirvInstruction &Aligned(SpirvInstruction &access, const MirType *type)
    {
        return access.Add(static_cast<uint32>(spv::MemoryAccessMask::Aligned)).Add(type->GetAlignment());
    }

    /// Loads the struct or array at buffer pointer *pointer* into *result* one member at a time, down to scalars,
    /// vectors and matrices, and builds the logical value with OpCompositeConstruct; *path* holds the member indices so
    /// far. Buffers never go through OpCopyLogical: NVIDIA's pipeline compiler (driver 610) crashes on whole-aggregate
    /// stores and miscompiles a nested member taken from a whole-aggregate load (it reads at the outer struct's start).
    void LoadMembers(uint32 result, const MirValue *pointer, List<uint32> &path, const MirType *type)
    {
        if (!HasLayoutForm(type))
        {
            const uint32 source = _spirv.NewId();
            EmitBufferChain(source, pointer, path.AsSpan());
            Emit(Aligned(SpirvInstruction(Op::OpLoad).Add(Type(type)).Add(result).Add(source), type));
            return;
        }
        const bool isStruct = type->Is(MirTypeKind::Struct);
        const uint32 count = isStruct ? static_cast<uint32>(type->GetFields().Count()) : type->GetCount();
        List<uint32> parts;
        for (uint32 index = 0; index < count; ++index)
        {
            const MirType *element = isStruct ? type->GetFields()[index].Type : type->GetElement();
            const uint32 part = _spirv.NewId();
            path.Add(index);
            LoadMembers(part, pointer, path, element);
            path.Pop();
            parts.Add(part);
        }
        SpirvInstruction construct(Op::OpCompositeConstruct);
        construct.Add(Type(type)).Add(result);
        for (const uint32 part : parts)
        {
            construct.Add(part);
        }
        Emit(construct);
    }

    /// Stores the struct or array *value* at buffer pointer *pointer* one member at a time, the counterpart of
    /// LoadMembers; each member's pointer is one chain from the buffer root (EmitBufferChain).
    void StoreMembers(const MirValue *pointer, List<uint32> &path, const MirType *type, uint32 value)
    {
        if (!HasLayoutForm(type))
        {
            const uint32 target = _spirv.NewId();
            EmitBufferChain(target, pointer, path.AsSpan());
            Emit(Aligned(SpirvInstruction(Op::OpStore).Add(target).Add(value), type));
            return;
        }
        const bool isStruct = type->Is(MirTypeKind::Struct);
        const uint32 count = isStruct ? static_cast<uint32>(type->GetFields().Count()) : type->GetCount();
        for (uint32 index = 0; index < count; ++index)
        {
            const MirType *element = isStruct ? type->GetFields()[index].Type : type->GetElement();
            const uint32 part = _spirv.NewId();
            Emit(SpirvInstruction(Op::OpCompositeExtract).Add(Type(element)).Add(part).Add(value).Add(index));
            path.Add(index);
            StoreMembers(pointer, path, element, part);
            path.Pop();
        }
    }

    /// The buffer pointer *pointer* followed by the constant member indices *tail*, as one OpUntypedAccessChainKHR from
    /// the buffer root: MIR's chains of chains are folded together, and indices into structs are mapped onto their
    /// flattened form (FlatStruct), where a nested struct takes no index of its own.
    void EmitBufferChain(uint32 result, const MirValue *pointer, Span<const uint32> tail)
    {
        List<const MirInstruction *> chains;
        const MirValue *base = pointer;
        while (base->GetValueKind() == MirValueKind::Instruction &&
               static_cast<const MirInstruction *>(base)->GetOp() == MirOp::AccessChain)
        {
            chains.Add(static_cast<const MirInstruction *>(base));
            base = static_cast<const MirInstruction *>(base)->GetOperand(0);
        }
        List<uint32> indices;
        const MirType *current = base->GetType()->GetElement();
        if (base->GetValueKind() == MirValueKind::Instruction &&
            _bufferRoots.Contains(static_cast<const MirInstruction *>(base)))
        {
            // The root points at the Block `{ T data[]; }`.
            indices.Add(UintConstant(0));
        }
        // Struct indices are constants; a nested struct's index adds to the next one instead of being emitted.
        uint32 flat = 0;
        const auto step = [&](const MirValue *operand, uint32 constant) {
            if (current->Is(MirTypeKind::Struct))
            {
                const uint32 field = operand != nullptr
                                         ? static_cast<uint32>(static_cast<const MirConstant *>(operand)->GetBits())
                                         : constant;
                flat += Flat(current).First[field];
                current = current->GetFields()[field].Type;
                if (!current->Is(MirTypeKind::Struct))
                {
                    indices.Add(UintConstant(flat));
                    flat = 0;
                }
                return;
            }
            indices.Add(operand != nullptr ? Value(operand) : UintConstant(constant));
            current = current->GetElement();
        };
        for (usize index = chains.Count(); index-- > 0;)
        {
            const Span<MirValue *const> operands = chains[index]->GetOperands();
            for (usize operand = 1; operand < operands.Count(); ++operand)
            {
                step(operands[operand], 0);
            }
        }
        for (const uint32 index : tail)
        {
            step(nullptr, index);
        }
        const MirType *leaf = current;
        if (current->Is(MirTypeKind::Struct))
        {
            // A pointer to a nested struct: the address of its first member, where it begins in the flat form.
            indices.Add(UintConstant(flat));
            while (leaf->Is(MirTypeKind::Struct))
            {
                leaf = leaf->GetFields()[0].Type;
            }
        }
        // Typed pointers: NVIDIA's pipeline compiler crashes on untyped PhysicalStorageBuffer access chains.
        SpirvInstruction chain(Op::OpAccessChain);
        chain.Add(PhysicalPointer(BufferLayoutType(leaf))).Add(result).Add(Value(base)).Add(indices.AsSpan());
        Emit(chain);
    }

    void EmitAccessChain(const MirInstruction &instruction)
    {
        const Span<MirValue *const> operands = instruction.GetOperands();
        const MirValue *base = operands[0];
        const MirAddressSpace space = base->GetType()->GetAddressSpace();
        const MirType *resultPointee = instruction.GetType()->GetElement();
        if (space == MirAddressSpace::Buffer)
        {
            EmitBufferChain(Value(&instruction), &instruction, {});
            return;
        }
        const bool argumentBlock = base->GetValueKind() == MirValueKind::Global && _argumentBlocks.Contains(Value(base));
        const bool layout = space == MirAddressSpace::Uniform || space == MirAddressSpace::ShaderRecord;
        SpirvInstruction chain(Op::OpAccessChain);
        chain.Add(Pointer(resultPointee, StorageClassOf(space), layout)).Add(Value(&instruction)).Add(Value(base));
        if (argumentBlock)
        {
            chain.Add(UintConstant(0));
        }
        for (usize i = 1; i < operands.Count(); ++i)
        {
            chain.Add(Value(operands[i]));
        }
        Emit(chain);
    }

    void EmitEntryPoint(const MirEntryPoint &entry)
    {
        spv::ExecutionModel model = spv::ExecutionModel::GLCompute;
        switch (entry.Stage)
        {
        case MirStage::Compute:
            model = spv::ExecutionModel::GLCompute;
            break;
        case MirStage::Vertex:
            model = spv::ExecutionModel::Vertex;
            break;
        case MirStage::Pixel:
            model = spv::ExecutionModel::Fragment;
            break;
        case MirStage::Amplification:
            model = spv::ExecutionModel::TaskEXT;
            break;
        case MirStage::Mesh:
            model = spv::ExecutionModel::MeshEXT;
            break;
        case MirStage::RayGeneration:
            model = spv::ExecutionModel::RayGenerationKHR;
            break;
        case MirStage::ClosestHit:
            model = spv::ExecutionModel::ClosestHitKHR;
            break;
        case MirStage::AnyHit:
            model = spv::ExecutionModel::AnyHitKHR;
            break;
        case MirStage::Miss:
            model = spv::ExecutionModel::MissKHR;
            break;
        case MirStage::Intersection:
            model = spv::ExecutionModel::IntersectionKHR;
            break;
        case MirStage::Callable:
            model = spv::ExecutionModel::CallableKHR;
            break;
        }
        if (entry.Stage >= MirStage::RayGeneration)
        {
            UseRayTracing();
        }
        if (entry.Stage == MirStage::Amplification || entry.Stage == MirStage::Mesh)
        {
            _spirv.Capability(spv::Capability::MeshShadingEXT);
            _spirv.Extension("SPV_EXT_mesh_shader");
        }
        // RECOVERY-DECISION lang-spirv-D9: entry.Features adds nothing here: capabilities and extensions follow the
        // instructions the module uses (§11.3), and the shader pipeline checks the features against the profile.
        const uint32 function = Value(entry.Function);
        SpirvInstruction instruction(Op::OpEntryPoint);
        instruction.Add(model).Add(function).Add(entry.Name);
        for (const uint32 global : _usedGlobals)
        {
            instruction.Add(global);
        }
        instruction.WriteTo(_spirv.EntryPoints);
        if (entry.Stage == MirStage::Compute || entry.Stage == MirStage::Amplification || entry.Stage == MirStage::Mesh)
        {
            SpirvInstruction(Op::OpExecutionMode)
                .Add(function)
                .Add(spv::ExecutionMode::LocalSize)
                .Add(entry.GroupSize[0])
                .Add(entry.GroupSize[1])
                .Add(entry.GroupSize[2])
                .WriteTo(_spirv.ExecutionModes);
        }
        if (entry.Stage == MirStage::Mesh)
        {
            SpirvInstruction(Op::OpExecutionMode)
                .Add(function)
                .Add(spv::ExecutionMode::OutputVertices)
                .Add(entry.MaxVertices)
                .WriteTo(_spirv.ExecutionModes);
            SpirvInstruction(Op::OpExecutionMode)
                .Add(function)
                .Add(spv::ExecutionMode::OutputPrimitivesEXT)
                .Add(entry.MaxPrimitives)
                .WriteTo(_spirv.ExecutionModes);
            const spv::ExecutionMode topology = entry.Topology == MirMeshTopology::Triangle ? spv::ExecutionMode::OutputTrianglesEXT
                                                : entry.Topology == MirMeshTopology::Line   ? spv::ExecutionMode::OutputLinesEXT
                                                                                            : spv::ExecutionMode::OutputPoints;
            SpirvInstruction(Op::OpExecutionMode).Add(function).Add(topology).WriteTo(_spirv.ExecutionModes);
        }
        if (entry.Stage == MirStage::Pixel)
        {
            SpirvInstruction(Op::OpExecutionMode).Add(function).Add(spv::ExecutionMode::OriginUpperLeft).WriteTo(_spirv.ExecutionModes);
            if (_writesDepth)
            {
                SpirvInstruction(Op::OpExecutionMode).Add(function).Add(spv::ExecutionMode::DepthReplacing).WriteTo(_spirv.ExecutionModes);
            }
        }
    }

    MirModule *_module;
    List<String> *_errors;
    SpirvModuleBuilder _spirv;
    List<uint32> _body;
    List<uint32> _variables;
    usize _variablesAt = 0;
    HashMap<LayoutKey, uint32> _types;
    HashMap<PointerKey, uint32> _pointers;
    HashMap<uint32, uint32> _untypedPointers;
    HashMap<const MirConstant *, uint32> _constants;
    HashMap<const MirValue *, uint32> _values;
    HashMap<const MirBlock *, uint32> _blocks;
    HashMap<const MirType *, uint32> _rootBlocks;
    HashMap<const MirType *, uint32> _bufferBlocks;
    HashMap<const MirType *, uint32> _readOnlyBufferBlocks;
    HashMap<uint32, uint32> _functionPointers;
    HashMap<const MirType *, UniquePtr<FlatStruct>> _flatStructs;
    HashMap<uint32, uint32> _physicalPointers;
    HashMap<const MirType *, uint32> _bufferArrays;
    HashMap<uint32, uint32> _descriptorArrays;
    uint32 _resourceStride = 0;
    uint32 _samplerStride = 0;
    HashMap<uint64, uint32> _images;
    uint32 _accelerationStructureType = 0;
    uint32 _hitObjectAttributes = 0;
    /// The typed pointer to the Block of 32-bit words byte address buffers are viewed as by cooperative operations.
    uint32 _wordBufferPointer = 0;
    bool _sourceDebugInfo = false;
    uint32 _debugSet = 0;
    uint32 _debugNone = 0;
    uint32 _debugExpression = 0;
    uint32 _compilationUnit = 0;
    uint32 _debugFunction = 0;
    uint32 _currentFunction = 0;
    bool _scopeOpen = false;
    bool _definitionPending = false;
    MirLocation _lastLine;
    HashMap<uint32, uint32> _debugSources;
    HashMap<const MirType *, uint32> _debugTypes;
    HashMap<const MirFunction *, uint32> _debugFunctions;
    /// The Function variable of each declared source variable, by DebugFunction and variable index.
    HashMap<uint64, uint32> _debugLocals;
    HashMap<uint32, uint32> _sampledImageTypes;
    HashMap<uint32, uint32> _fileStrings;
    HashMap<const MirFunction *, MirStage> _entryStages;
    HashMap<const MirGlobal *, MirStage> _globalStages;
    HashSet<const MirInstruction *> _bufferRoots;
    List<uint32> _argumentBlocks;
    List<uint32> _usedGlobals;
    uint32 _resourceHeap = 0;
    uint32 _samplerHeap = 0;
    uint32 _samplerType = 0;
    uint32 _bufferType = 0;
    MirStage _stage = MirStage::Compute;
    bool _writesDepth = false;
};
} // namespace

bool EmitSpirvModule(MirModule &module, const MirTargetOptions &options, List<uint32> &words, List<String> &errors)
{
    const usize before = errors.Count();
    SpirvWriter writer(module, options, errors);
    return writer.Write(words) && errors.Count() == before;
}
} // namespace Marlin
