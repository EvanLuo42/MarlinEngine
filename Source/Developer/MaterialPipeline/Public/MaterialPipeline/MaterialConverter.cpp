#include "MaterialPipeline/MaterialConverter.h"

#include "AssetPipeline/AssetCanonical.h"
#include "Concurrency/ScopeLock.h"
#include "Containers/HashMap.h"
#include "HAL/PlatformSync.h"
#include "Math/Float16.h"
#include "MarlinText/MarlinTextDocument.h"
#include "Memory/MemoryOps.h"
#include "Shader/GlobalShader.h"
#include "ShaderPipeline/ShaderCompiler.h"
#include "ShaderPipeline/ShaderSources.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
/// The name of the converter input that stands for the reflection of a material struct with a variant.
constexpr StringView ShaderInputPrefix = "MaterialShader:";

/// The scalar a parameter's components are and how many there are, from its type name: `half3` is three halves.
struct ParameterShape
{
    enum class Scalar : uint8
    {
        None,
        Half,
        Float,
        Int,
        Uint,
        Bool,
    };
    Scalar Kind = Scalar::None;
    uint32 Components = 0;
};

ParameterShape ShapeOf(StringView type)
{
    struct Name
    {
        StringView Text;
        ParameterShape::Scalar Kind;
    };
    constexpr Name Names[] = {{"half", ParameterShape::Scalar::Half},
                              {"float", ParameterShape::Scalar::Float},
                              {"uint", ParameterShape::Scalar::Uint},
                              {"int", ParameterShape::Scalar::Int},
                              {"bool", ParameterShape::Scalar::Bool}};
    for (const Name &name : Names)
    {
        if (!type.StartsWith(name.Text))
        {
            continue;
        }
        const StringView rest = type.Substring(name.Text.Length());
        if (rest.IsEmpty())
        {
            return {name.Kind, 1};
        }
        if (rest.Length() == 1 && rest[0] >= '2' && rest[0] <= '4')
        {
            return {name.Kind, static_cast<uint32>(rest[0] - '0')};
        }
    }
    return {};
}

uint32 ScalarSize(ParameterShape::Scalar kind)
{
    return kind == ParameterShape::Scalar::Half ? 2 : 4;
}

/// Writes *value*, a number or a tuple of them, into the parameter at *out*; false with *error* if it does not fit.
bool WriteParameter(const ShaderMaterialParameter &parameter, const MarlinTextValue &value, uint8 *out, String &error)
{
    const ParameterShape shape = ShapeOf(parameter.Type.AsView());
    if (shape.Kind == ParameterShape::Scalar::None)
    {
        error = Format("the parameter `{0}` is a `{1}`, which a material does not set", parameter.Name, parameter.Type);
        return false;
    }
    List<const MarlinTextValue *> components;
    if (value.Kind == MarlinTextValueKind::Tuple)
    {
        for (const MarlinTextValue &element : value.Elements)
        {
            components.Add(&element);
        }
    }
    else
    {
        components.Add(&value);
    }
    if (components.Count() != shape.Components)
    {
        error = Format("the parameter `{0}` is a `{1}`, which takes {2} value(s), not {3}", parameter.Name,
                       parameter.Type, shape.Components, components.Count());
        return false;
    }
    for (usize i = 0; i < components.Count(); ++i)
    {
        const MarlinTextValue &component = *components[i];
        uint8 *at = out + i * ScalarSize(shape.Kind);
        if (shape.Kind == ParameterShape::Scalar::Bool)
        {
            if (component.Kind != MarlinTextValueKind::Bool)
            {
                error = Format("the parameter `{0}` takes `true` or `false`", parameter.Name);
                return false;
            }
            const uint32 bit = component.Bool ? 1 : 0;
            MemCopy(at, &bit, sizeof(bit));
            continue;
        }
        if (!component.IsNumber())
        {
            error = Format("the parameter `{0}` takes numbers", parameter.Name);
            return false;
        }
        const float64 number = component.AsFloat();
        if (parameter.HasRange && (number < parameter.RangeMin || number > parameter.RangeMax))
        {
            error = Format("the parameter `{0}` is {1}, outside its range [{2}, {3}]", parameter.Name, number,
                           parameter.RangeMin, parameter.RangeMax);
            return false;
        }
        switch (shape.Kind)
        {
        case ParameterShape::Scalar::Half:
        {
            const uint16 bits = Float16(static_cast<float32>(number)).Bits;
            MemCopy(at, &bits, sizeof(bits));
            break;
        }
        case ParameterShape::Scalar::Float:
        {
            const auto single = static_cast<float32>(number);
            MemCopy(at, &single, sizeof(single));
            break;
        }
        case ParameterShape::Scalar::Int:
        case ParameterShape::Scalar::Uint:
        {
            if (component.Kind != MarlinTextValueKind::Integer ||
                (shape.Kind == ParameterShape::Scalar::Uint && component.Integer < 0))
            {
                error = Format("the parameter `{0}` takes {1} integers", parameter.Name,
                               StringView(shape.Kind == ParameterShape::Scalar::Uint ? "non-negative" : "whole"));
                return false;
            }
            const auto word = static_cast<uint32>(component.Integer);
            MemCopy(at, &word, sizeof(word));
            break;
        }
        default:
            break;
        }
    }
    return true;
}

/// The material and variant a converter input names.
bool ParseShaderInput(StringView name, StringView &material, List<ShaderVariantValue> &variant)
{
    return name.StartsWith(ShaderInputPrefix) && ParseShaderKey(name.Substring(ShaderInputPrefix.Length()), material, variant);
}

/// The reflections this process made, by material and variant, with what each depended on, so that checking a
/// material's key does not compile its struct again: only when a file the reflection used changed, or a new or
/// changed file declares a name it looked up (ShaderNameRecord), does it reflect again.
class ReflectionCache
{
public:
    static ReflectionCache &Get()
    {
        static ReflectionCache cache;
        return cache;
    }

    bool Reflect(StringView material, Span<const ShaderVariantValue> variant, ShaderMaterialReflection &reflection,
                 String &errors)
    {
        const String key = MakeMaterialShaderKey("Reflect", material, variant);
        List<ShaderSourceDependency> current;
        Hash128 currentHash;
        const bool listed = _sources.GetSourceSet(current, currentHash);
        if (listed)
        {
            const ScopeLock lock(_lock);
            if (Entry *entry = _entries.Find(key); entry != nullptr && IsCurrent(*entry, current, currentHash))
            {
                entry->SourceSetHash = currentHash;
                reflection = entry->Reflection;
                errors = entry->Errors;
                return entry->Ok;
            }
        }
        Entry entry;
        entry.Ok = ReflectMaterialShader(material, variant, entry.Reflection, entry.Errors);
        entry.Names.Names = entry.Reflection.Names;
        entry.Names.SourceSet = entry.Reflection.SourceSet;
        entry.SourceSetHash = ShaderSourceTracker::HashSourceSet(entry.Reflection.SourceSet.AsSpan());
        reflection = entry.Reflection;
        errors = entry.Errors;
        const bool ok = entry.Ok;
        const ScopeLock lock(_lock);
        _entries.Set(key, Move(entry));
        return ok;
    }

private:
    struct Entry
    {
        bool Ok = false;
        ShaderMaterialReflection Reflection;
        String Errors;
        ShaderNameRecord Names;
        /// The source set it was last known right for.
        Hash128 SourceSetHash;
    };

    /// Under _lock.
    bool IsCurrent(const Entry &entry, Span<const ShaderSourceDependency> current, const Hash128 &currentHash)
    {
        if (entry.SourceSetHash == currentHash)
        {
            return true;
        }
        HashMap<StringView, Hash128> now;
        for (const ShaderSourceDependency &file : current)
        {
            now.Set(file.Path.AsView(), file.Hash);
        }
        for (const ShaderSourceDependency &used : entry.Reflection.Dependencies)
        {
            const Hash128 *hash = now.Find(used.Path.AsView());
            if (hash == nullptr || *hash != used.Hash)
            {
                return false;
            }
        }
        return _sources.KeepsNames(entry.Names, entry.Reflection.Dependencies.AsSpan(), current);
    }

    Mutex _lock;
    ShaderSourceTracker _sources;
    HashMap<String, Entry> _entries;
};

bool Reflect(StringView material, Span<const ShaderVariantValue> variant, ShaderMaterialReflection &reflection,
             String &errors)
{
    return ReflectionCache::Get().Reflect(material, variant, reflection, errors);
}

void WriteReflection(const ShaderMaterialReflection &reflection, StringWriter &text)
{
    AssetCanonical::WriteText(text, reflection.Name.AsView());
    AssetCanonical::WriteText(text, reflection.Id.AsView());
    AssetCanonical::WriteNumber(text, reflection.Size);
    for (const uint8 byte : reflection.Defaults)
    {
        AssetCanonical::WriteNumber(text, byte);
    }
    for (const ShaderMaterialParameter &parameter : reflection.Parameters)
    {
        AssetCanonical::WriteText(text, parameter.Name.AsView());
        AssetCanonical::WriteText(text, parameter.Type.AsView());
        AssetCanonical::WriteNumber(text, parameter.Offset);
        AssetCanonical::WriteNumber(text, parameter.Size);
        AssetCanonical::WriteNumber(text, parameter.IsTexture ? 1u + parameter.DefaultTexture : 0u);
        AssetCanonical::WriteText(text, parameter.HasRange
                                            ? Format("{0} {1}", parameter.RangeMin, parameter.RangeMax).AsView()
                                            : StringView());
        AssetCanonical::WriteNumber(text, parameter.IsColor ? 1 : 0);
    }
    for (const ShaderMaterialSwitch &entry : reflection.Switches)
    {
        AssetCanonical::WriteText(text, entry.Name.AsView());
        AssetCanonical::WriteNumber(text, static_cast<uint64>(entry.Default));
        AssetCanonical::WriteNumber(text, entry.ValueCount);
    }
    for (const String &bsdf : reflection.CustomBsdfs)
    {
        AssetCanonical::WriteText(text, bsdf.AsView());
    }
}
} // namespace

bool WriteMaterialInput(StringView name, StringWriter &text)
{
    StringView material;
    List<ShaderVariantValue> variant;
    if (!ParseShaderInput(name, material, variant))
    {
        return false;
    }
    ShaderMaterialReflection reflection;
    String errors;
    if (!Reflect(material, variant.AsSpan(), reflection, errors))
    {
        // A struct that no longer reflects is an input that changed: the conversion reports why.
        AssetCanonical::WriteText(text, errors.AsView());
        return true;
    }
    WriteReflection(reflection, text);
    return true;
}

bool ConvertMaterial(ConversionContext &context, bool instance)
{
    const MarlinTextObject &source = context.GetSource();
    // The object, then the ones it overrides: an instance's parent, the parent's parent, ….
    List<const MarlinTextObject *> chain;
    chain.Add(&source);
    while (chain.Last()->GetParent() != nullptr)
    {
        if (chain.Count() >= 32)
        {
            context.Error("the materials it overrides go more than 32 deep, or around in a circle");
            return false;
        }
        const MarlinTextObject *parent = context.ReadDependency(chain.Last()->GetParent()->LinkGuid);
        if (parent == nullptr)
        {
            context.Error(Format("the material `{0}` overrides cannot be found", chain.Last()->GetParent()->LinkName)
                              .AsView());
            return false;
        }
        if (parent->GetTypeName() != "Material" && parent->GetTypeName() != "MaterialInstance")
        {
            context.Error(
                Format("it overrides `{0}`, a {1}, not a material", parent->GetName(), parent->GetTypeName()).AsView());
            return false;
        }
        chain.Add(parent);
    }
    auto find = [&](StringView key) -> const MarlinTextField * {
        for (const MarlinTextObject *object : chain)
        {
            if (const MarlinTextField *field = object->Find(key))
            {
                return field;
            }
        }
        return nullptr;
    };
    if (instance && chain.Count() == 1)
    {
        context.Error("a MaterialInstance overrides a material: write `MaterialInstance : <Parent#UUID> { … }`");
        return false;
    }
    if (instance && source.Find("Shader") != nullptr)
    {
        context.Error("a MaterialInstance draws with its parent's shader; make it a Material to name another");
        return false;
    }
    const MarlinTextField *shader = find("Shader");
    if (shader == nullptr || shader->Value.Kind != MarlinTextValueKind::ObjectLink || shader->Value.LinkName.IsEmpty())
    {
        context.Error("`Shader` names the `[Material]` struct, such as "
                      "`</Project/Language/Shaders/Materials/Rock:Project.Materials.Rock#id>`");
        return false;
    }
    const StringView material = shader->Value.LinkName;

    // The switches: the farthest object's first, each nearer one overriding.
    List<ShaderVariantValue> variant;
    for (usize i = chain.Count(); i-- > 0;)
    {
        const MarlinTextField *field = chain[i]->Find("Variant");
        if (field == nullptr)
        {
            continue;
        }
        if (field->Value.Kind != MarlinTextValueKind::Block)
        {
            context.Error("`Variant` is a block of switches, such as `Variant = { UseMoss = false }`");
            return false;
        }
        for (const MarlinTextField &entry : field->Value.Fields)
        {
            int64 value = 0;
            if (entry.Value.Kind == MarlinTextValueKind::Bool)
            {
                value = entry.Value.Bool ? 1 : 0;
            }
            else if (entry.Value.Kind == MarlinTextValueKind::Integer)
            {
                value = entry.Value.Integer;
            }
            else
            {
                context.Error(Format("the switch `{0}` takes `true`, `false` or a number", entry.Key).AsView());
                return false;
            }
            ShaderVariantValue *existing = nullptr;
            for (ShaderVariantValue &known : variant)
            {
                existing = known.Name.AsView() == entry.Key ? &known : existing;
            }
            if (existing == nullptr)
            {
                existing = &variant.AddDefaulted();
                existing->Name = String(entry.Key);
            }
            existing->Value = value;
        }
    }

    context.ReadConverterInput(
        Format("{0}{1}", ShaderInputPrefix, MakeMaterialShaderKey("Reflect", material, variant.AsSpan())).AsView());
    ShaderMaterialReflection reflection;
    String errors;
    if (!Reflect(material, variant.AsSpan(), reflection, errors))
    {
        context.Error(Format("the shader `{0}` does not reflect:\n{1}", material, errors).AsView());
        return false;
    }
    Guid id;
    if (shader->Value.LinkGuid.IsValid() && Guid::TryParse(reflection.Id.AsView(), id) && id != shader->Value.LinkGuid)
    {
        context.Error(Format("the shader link's id is not `{0}`'s `[Id(\"{1}\")]`", material, reflection.Id).AsView());
        return false;
    }

    // The fields besides the shader and the switches are parameters.
    for (const MarlinTextObject *object : chain)
    {
        for (const MarlinTextField &field : object->GetFields())
        {
            if (field.IsDirective || field.Key == "Shader" || field.Key == "Variant")
            {
                continue;
            }
            bool known = false;
            for (const ShaderMaterialParameter &parameter : reflection.Parameters)
            {
                known = known || parameter.Name.AsView() == field.Key;
            }
            if (!known)
            {
                context.Error(Format("`{0}` is not a parameter of `{1}`", field.Key, material).AsView());
                return false;
            }
        }
    }
    List<uint8> parameters = Move(reflection.Defaults);
    List<MaterialTextureBinding> textures;
    for (const ShaderMaterialParameter &parameter : reflection.Parameters)
    {
        const MarlinTextField *field = find(parameter.Name.AsView());
        if (parameter.IsTexture)
        {
            MaterialTextureBinding &binding = textures.AddDefaulted();
            binding.Offset = parameter.Offset;
            binding.Default = static_cast<MaterialDefaultTexture>(parameter.DefaultTexture);
            if (field != nullptr)
            {
                if (field->Value.Kind != MarlinTextValueKind::ObjectLink || !field->Value.LinkGuid.IsValid())
                {
                    context.Error(Format("the texture parameter `{0}` takes a texture link", parameter.Name).AsView());
                    return false;
                }
                binding.Texture = AssetRef<Texture>::FromGuid(field->Value.LinkGuid);
            }
            continue;
        }
        if (field == nullptr)
        {
            continue;
        }
        String error;
        if (!WriteParameter(parameter, field->Value, parameters.Data() + parameter.Offset, error))
        {
            context.Error(error.AsView());
            return false;
        }
    }

    List<MaterialSwitchValue> switches;
    for (const ShaderMaterialSwitch &entry : reflection.Switches)
    {
        MaterialSwitchValue &value = switches.AddDefaulted();
        value.Name = FrozenString(entry.Name.AsView());
        value.Value = entry.Default;
        for (const ShaderVariantValue &chosen : variant)
        {
            value.Value = chosen.Name == entry.Name ? chosen.Value : value.Value;
        }
    }
    Material result;
    result.Name = FrozenString(source.GetName());
    result.Shader = FrozenString(material);
    result.Variant = FrozenArray<MaterialSwitchValue>(switches.AsSpan());
    result.Parameters = FrozenArray<uint8>(parameters.AsSpan());
    result.Textures = FrozenArray<MaterialTextureBinding>(textures.AsSpan());
    List<FrozenString> bsdfs;
    for (const String &bsdf : reflection.CustomBsdfs)
    {
        bsdfs.Add(FrozenString(bsdf.AsView()));
    }
    result.CustomBsdfs = FrozenArray<FrozenString>(bsdfs.AsSpan());
    context.SetResult(result);
    return !context.HasFailed();
}
} // namespace Marlin
