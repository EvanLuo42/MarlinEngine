#include "Sema/SemaBuiltins.h"

#include "LanguageIR/MirPrinter.h"

namespace Marlin
{
namespace
{
struct BuiltinTypeName
{
    StringView Name;
    BuiltinTypeId Id;
};

constexpr BuiltinTypeName BuiltinTypeNames[] = {
    {"IScalar", BuiltinTypeId::IScalar},
    {"IScalarOrVector", BuiltinTypeId::IScalarOrVector},
    {"INumeric", BuiltinTypeId::INumeric},
    {"IInteger", BuiltinTypeId::IInteger},
    {"IFloatingPoint", BuiltinTypeId::IFloatingPoint},
    {"ILogical", BuiltinTypeId::ILogical},
    {"IComparable", BuiltinTypeId::IComparable},
    {"IDifferentiable", BuiltinTypeId::IDifferentiable},
    {"ScalarOf", BuiltinTypeId::ScalarOf},
    {"Vector", BuiltinTypeId::Vector},
    {"Matrix", BuiltinTypeId::Matrix},
    {"String", BuiltinTypeId::String},
    {"Range", BuiltinTypeId::Range},
    {"Feature", BuiltinTypeId::Feature},
    {"Texture1D", BuiltinTypeId::Texture1D},
    {"Texture1DArray", BuiltinTypeId::Texture1DArray},
    {"Texture2D", BuiltinTypeId::Texture2D},
    {"Texture2DArray", BuiltinTypeId::Texture2DArray},
    {"Texture2DMultisample", BuiltinTypeId::Texture2DMultisample},
    {"Texture3D", BuiltinTypeId::Texture3D},
    {"TextureCube", BuiltinTypeId::TextureCube},
    {"TextureCubeArray", BuiltinTypeId::TextureCubeArray},
    {"Ptr", BuiltinTypeId::Ptr},
    {"Span", BuiltinTypeId::Span},
    {"Byte", BuiltinTypeId::Byte},
    {"Sampler", BuiltinTypeId::Sampler},
    {"ComparisonSampler", BuiltinTypeId::ComparisonSampler},
    {"AccelerationStructure", BuiltinTypeId::AccelerationStructure},
    {"RayQuery", BuiltinTypeId::RayQuery},
    {"HitObject", BuiltinTypeId::HitObject},
    {"CoopVector", BuiltinTypeId::CoopVector},
    {"CoopMatrix", BuiltinTypeId::CoopMatrix},
    {"MatrixRef", BuiltinTypeId::MatrixRef},
    {"VectorRef", BuiltinTypeId::VectorRef},
    {"Vertices", BuiltinTypeId::Vertices},
    {"Indices", BuiltinTypeId::Indices},
    {"Primitives", BuiltinTypeId::Primitives},
    {"Object", BuiltinTypeId::Object},
    {"Array", BuiltinTypeId::Array},
};
} // namespace

BuiltinTypeId FindBuiltinType(StringView name)
{
    for (const BuiltinTypeName &entry : BuiltinTypeNames)
    {
        if (entry.Name == name)
        {
            return entry.Id;
        }
    }
    return BuiltinTypeId::None;
}

IntrinsicTarget FindIntrinsic(StringView name)
{
    if (name == "PtrRead")
    {
        return {IntrinsicKind::PtrRead};
    }
    if (name == "PtrWrite")
    {
        return {IntrinsicKind::PtrWrite};
    }
    if (name == "PtrOffset")
    {
        return {IntrinsicKind::PtrOffset};
    }
    if (name == "PtrAdd")
    {
        return {IntrinsicKind::PtrAdd};
    }
    if (name == "PtrIsNull")
    {
        return {IntrinsicKind::PtrIsNull};
    }
    if (name == "PtrCast")
    {
        return {IntrinsicKind::PtrCast};
    }
    if (name == "PtrEqual")
    {
        return {IntrinsicKind::PtrEqual};
    }
    if (name == "PtrNotEqual")
    {
        return {IntrinsicKind::PtrNotEqual};
    }
    if (name == "DebugPrint")
    {
        return {IntrinsicKind::DebugPrint, MirIntrinsic::DebugPrint};
    }
    if (name == "DebugAssert")
    {
        return {IntrinsicKind::DebugAssert, MirIntrinsic::DebugPrint};
    }
    if (name == "DifferentialAdd")
    {
        return {IntrinsicKind::DifferentialAdd};
    }
    if (name == "DifferentialScale")
    {
        return {IntrinsicKind::DifferentialScale};
    }
    if (name == "Require")
    {
        return {IntrinsicKind::Require, MirIntrinsic::Require};
    }
    if (name == "ToWords")
    {
        return {IntrinsicKind::ToWords};
    }
    if (name == "FromWords")
    {
        return {IntrinsicKind::FromWords};
    }
    for (auto i = static_cast<uint32>(MirIntrinsic::None) + 1; i < static_cast<uint32>(MirIntrinsic::DebugPrint);
         ++i)
    {
        const auto intrinsic = static_cast<MirIntrinsic>(i);
        if (GetMirIntrinsicName(intrinsic) == name)
        {
            return {IntrinsicKind::Mir, intrinsic};
        }
    }
    for (auto i = static_cast<uint32>(MirOp::Add); i <= static_cast<uint32>(MirOp::EmitMeshTasks); ++i)
    {
        const auto op = static_cast<MirOp>(i);
        if (op != MirOp::Intrinsic && GetMirOpName(op) == name)
        {
            return {IntrinsicKind::Op, MirIntrinsic::None, op};
        }
    }
    return {};
}

MirBuiltin FindSystemValue(StringView name)
{
    for (auto i = static_cast<uint32>(MirBuiltin::None) + 1; i <= static_cast<uint32>(MirBuiltin::HelperLane); ++i)
    {
        const auto builtin = static_cast<MirBuiltin>(i);
        if (GetMirBuiltinName(builtin) == name)
        {
            return builtin;
        }
    }
    return MirBuiltin::None;
}

CompileTimeFunction FindCompileTimeFunction(StringView name)
{
    if (name == "DeviceSupports")
    {
        return CompileTimeFunction::DeviceSupports;
    }
    if (name == "FeatureFromName")
    {
        return CompileTimeFunction::FeatureFromName;
    }
    if (name == "Implements")
    {
        return CompileTimeFunction::Implements;
    }
    if (name == "TypeId")
    {
        return CompileTimeFunction::TypeId;
    }
    return CompileTimeFunction::None;
}
} // namespace Marlin
