#pragma once

#include "AssetPipeline/AssetConverter.h"
#include "CoreTypes.h"
#include "Material/Material.h"
#include "MaterialPipeline/MaterialConverter.gen.h"

namespace Marlin
{
/// Converts a material source (Documentation/Architecture/07 §4.2, MarlinLanguage.md §1.6):
///
///     Material { !UUID = "…" !Name = "Rock" Shader = </Project/Language/Shaders/Materials/Rock:Project.Materials.Rock#id>
///                Variant = { UseMoss = false } Roughness = 0.3 BaseColor = <Content/Rock_D:Rock_D#uuid> }
///     MaterialInstance : <Rock#uuid> { !UUID = "…" !Name = "Rock_Wet" Roughness = 0.1 }
///
/// The shader link names the `[Material]` struct by its qualified name, and its `#id` is the struct's `[Id]`. The other
/// fields are the struct's parameters, `Variant` its switches; an instance overrides its parent's and keeps its
/// shader. The struct is reflected through the shader pipeline, so its defaults fill what the source leaves out, and a
/// change to it that changes the reflection converts the material again. MaterialConverter and
/// MaterialInstanceConverter share it.
MATERIALPIPELINE_API bool ConvertMaterial(ConversionContext &context, bool instance);
/// Writes the material converters' inputs: the reflection of a `[Material]` struct with a variant.
MATERIALPIPELINE_API bool WriteMaterialInput(StringView name, StringWriter &text);

MCLASS()
class MATERIALPIPELINE_API MaterialConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "Material";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Material>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 2;
    }
    bool Convert(ConversionContext &context) override
    {
        return ConvertMaterial(context, false);
    }
    bool WriteInput(StringView name, StringWriter &text) const override
    {
        return WriteMaterialInput(name, text);
    }
};

MCLASS()
class MATERIALPIPELINE_API MaterialInstanceConverter final : public AssetConverter
{
    MGENERATED_BODY()
public:
    [[nodiscard]] StringView GetSourceType() const override
    {
        return "MaterialInstance";
    }
    [[nodiscard]] const TypeInfo &GetRuntimeType() const override
    {
        return TypeOf<Material>();
    }
    [[nodiscard]] uint32 GetVersion() const override
    {
        return 2;
    }
    bool Convert(ConversionContext &context) override
    {
        return ConvertMaterial(context, true);
    }
    bool WriteInput(StringView name, StringWriter &text) const override
    {
        return WriteMaterialInput(name, text);
    }
};
} // namespace Marlin
