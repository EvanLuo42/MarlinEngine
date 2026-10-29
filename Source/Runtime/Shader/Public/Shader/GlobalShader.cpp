#include "Shader/GlobalShader.h"

#include "String/Format.h"
#include "String/Number.h"

namespace Marlin
{
RhiShaderStage ToRhiShaderStage(ShaderStage stage)
{
    switch (stage)
    {
    case ShaderStage::Compute:
        return RhiShaderStage::Compute;
    case ShaderStage::Vertex:
        return RhiShaderStage::Vertex;
    case ShaderStage::Pixel:
        return RhiShaderStage::Pixel;
    case ShaderStage::Task:
        return RhiShaderStage::Task;
    case ShaderStage::Mesh:
        return RhiShaderStage::Mesh;
    case ShaderStage::RayGeneration:
        return RhiShaderStage::RayGeneration;
    case ShaderStage::Miss:
        return RhiShaderStage::Miss;
    case ShaderStage::ClosestHit:
        return RhiShaderStage::ClosestHit;
    case ShaderStage::AnyHit:
        return RhiShaderStage::AnyHit;
    case ShaderStage::Intersection:
        return RhiShaderStage::Intersection;
    case ShaderStage::Callable:
        return RhiShaderStage::Callable;
    }
    return RhiShaderStage::None;
}

String MakeMaterialShaderKey(StringView entryPoint, StringView material, Span<const ShaderVariantValue> variant)
{
    List<const ShaderVariantValue *> sorted;
    for (const ShaderVariantValue &value : variant)
    {
        sorted.Add(&value);
    }
    sorted.Sort([](const ShaderVariantValue *a, const ShaderVariantValue *b) { return a->Name < b->Name; });
    String key = Format("{0}|{1}|", entryPoint, material);
    for (const ShaderVariantValue *value : sorted)
    {
        key.Append(Format("{0}={1};", value->Name, value->Value).AsView());
    }
    return key;
}

String MakeSpecializedShaderKey(StringView key, Span<const ShaderSpecializationValue> values)
{
    List<const ShaderSpecializationValue *> sorted;
    for (const ShaderSpecializationValue &value : values)
    {
        sorted.Add(&value);
    }
    sorted.Sort(
        [](const ShaderSpecializationValue *a, const ShaderSpecializationValue *b) { return a->Name < b->Name; });
    String result = Format("{0}#", GetShaderKeyAsset(key));
    for (const ShaderSpecializationValue *value : sorted)
    {
        result.Append(Format("{0}={1};", value->Name, value->Bits).AsView());
    }
    return result;
}

StringView GetShaderKeyAsset(StringView key)
{
    const usize hash = key.IndexOf('#');
    return hash == StringView::NotFound ? key : key.Substring(0, hash);
}

StringView GetShaderKeyEntryPoint(StringView key)
{
    key = GetShaderKeyAsset(key);
    const usize bar = key.IndexOf('|');
    return bar == StringView::NotFound ? key : key.Substring(0, bar);
}

bool ParseShaderKeySpecializations(StringView key, List<ShaderSpecializationValue> &values)
{
    values.Clear();
    const usize hash = key.IndexOf('#');
    if (hash == StringView::NotFound)
    {
        return true;
    }
    StringView items = key.Substring(hash + 1);
    while (!items.IsEmpty())
    {
        const usize end = items.IndexOf(';');
        const StringView item = end == StringView::NotFound ? items : items.Substring(0, end);
        items = end == StringView::NotFound ? StringView() : items.Substring(end + 1);
        const usize equals = item.IndexOf('=');
        int64 number = 0;
        if (equals == StringView::NotFound || equals == 0 || !TryParse(item.Substring(equals + 1), number) ||
            number < 0 || number > 0xffffffffll)
        {
            return false;
        }
        ShaderSpecializationValue &value = values.AddDefaulted();
        value.Name = String(item.Substring(0, equals));
        value.Bits = static_cast<uint32>(number);
    }
    return true;
}

bool ParseShaderKey(StringView key, StringView &material, List<ShaderVariantValue> &variant)
{
    material = {};
    variant.Clear();
    key = GetShaderKeyAsset(key);
    const usize first = key.IndexOf('|');
    if (first == StringView::NotFound)
    {
        return true;
    }
    const StringView rest = key.Substring(first + 1);
    const usize second = rest.IndexOf('|');
    if (second == StringView::NotFound)
    {
        return false;
    }
    material = rest.Substring(0, second);
    StringView switches = rest.Substring(second + 1);
    while (!switches.IsEmpty())
    {
        const usize end = switches.IndexOf(';');
        const StringView item = end == StringView::NotFound ? switches : switches.Substring(0, end);
        switches = end == StringView::NotFound ? StringView() : switches.Substring(end + 1);
        const usize equals = item.IndexOf('=');
        if (equals == StringView::NotFound)
        {
            return false;
        }
        ShaderVariantValue &value = variant.AddDefaulted();
        value.Name = String(item.Substring(0, equals));
        int64 number = 0;
        if (!TryParse(item.Substring(equals + 1), number))
        {
            return false;
        }
        value.Value = number;
    }
    return !material.IsEmpty();
}

const ShaderVariant *GlobalShader::FindVariant(StringView profile) const
{
    for (const ShaderVariant &variant : Variants)
    {
        for (const FrozenString &name : variant.Profiles)
        {
            if (name.AsView() == profile)
            {
                return &variant;
            }
        }
    }
    return nullptr;
}

Guid GetGlobalShaderGuid(StringView entryPoint)
{
    return Guid::FromName(GlobalShaderNamespace, entryPoint);
}
} // namespace Marlin
