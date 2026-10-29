#include "SceneRenderer/SceneMaterialRegistry.h"

#include "Asset/Assets.h"
#include "Concurrency/ScopeLock.h"
#include "Diagnostics/Log.h"
#include "Memory/MemoryOps.h"
#include "Renderer/RenderFrame.h"
#include "SceneRenderer/SceneTextureResidency.h"
#include "String/Format.h"

namespace Marlin
{
namespace
{
/// Where a Marlin.Shading.MaterialTexture2D keeps its feedback pointer: after its three words, 8-byte aligned.
constexpr uint32 FeedbackOffset = 16;
} // namespace

SceneMaterialRegistry::SceneMaterialRegistry(SceneTextureResidency *textures) : _textures(textures)
{
    // Slot 0 is the Default material's, held for good.
    (void)Acquire(DefaultMaterial);
}

SceneMaterialRegistry::~SceneMaterialRegistry()
{
    if (_textures == nullptr)
    {
        return;
    }
    for (const UniquePtr<Entry> &entry : _entries)
    {
        for (const AssetRef<Texture> &texture : entry->Textures)
        {
            _textures->ReleaseTexture(texture);
        }
    }
}

uint32 SceneMaterialRegistry::Acquire(const Guid &material)
{
    const Guid uuid = material.IsValid() ? material : DefaultMaterial;
    const ScopeLock lock(_lock);
    if (const uint32 *found = _byGuid.Find(uuid))
    {
        ++_entries[*found]->References;
        return *found;
    }
    uint32 slot = 0;
    if (!_freeSlots.IsEmpty())
    {
        slot = _freeSlots.Pop();
    }
    else
    {
        slot = static_cast<uint32>(_entries.Count());
        _entries.Add(MakeUnique<Entry>());
    }
    Entry &entry = *_entries[slot];
    entry = Entry();
    entry.Uuid = uuid;
    entry.Ref = Assets::Load<Material>(uuid);
    entry.References = 1;
    _byGuid.Add(uuid, slot);
    return slot;
}

void SceneMaterialRegistry::Release(uint32 slot)
{
    const ScopeLock lock(_lock);
    if (slot < _entries.Count() && _entries[slot]->References > 0)
    {
        --_entries[slot]->References;
    }
}

void SceneMaterialRegistry::Update()
{
    const ScopeLock lock(_lock);
    const uint32 stamp = _textures != nullptr ? _textures->GetResidentStamp() : 0;
    const bool texturesChanged = stamp != _textureStamp;
    _textureStamp = stamp;
    for (uint32 slot = 0; slot < _entries.Count(); ++slot)
    {
        Entry &entry = *_entries[slot];
        if (!entry.Uuid.IsValid())
        {
            continue;
        }
        // Nothing uses it: its element goes back to its permutation, its slot to the free list. Slot 0 stays.
        if (entry.References == 0 && slot != 0)
        {
            if (entry.Permutation != ~0u)
            {
                _permutations[entry.Permutation]->FreeElements.Add(entry.Element);
            }
            for (const AssetRef<Texture> &texture : entry.Textures)
            {
                if (_textures != nullptr)
                {
                    _textures->ReleaseTexture(texture);
                }
            }
            _byGuid.Remove(entry.Uuid);
            entry = Entry();
            _freeSlots.Add(slot);
            continue;
        }
        const Material *material = entry.Ref.Get();
        if (!entry.Loaded && !entry.Failed)
        {
            const AssetSlot *asset = entry.Ref.GetSlot();
            const AssetState state = asset != nullptr ? asset->GetState() : AssetState::Failed;
            if (state == AssetState::Loading || state == AssetState::Unloaded)
            {
                continue;
            }
            if (state == AssetState::Failed || material == nullptr || material->Shader.AsView().IsEmpty())
            {
                MARLIN_LOG(LogRenderer, Warning, "Material {0} failed to load; it draws as the Default material",
                           entry.Uuid);
                entry.Failed = true;
                continue;
            }
            Place(entry, *material);
            entry.Loaded = true;
            continue;
        }
        if (entry.Loaded && texturesChanged && material != nullptr && !entry.Textures.IsEmpty())
        {
            WriteBlock(entry, *material);
        }
    }
}

void SceneMaterialRegistry::Place(Entry &entry, const Material &material)
{
    List<ShaderVariantValue> variant;
    for (const MaterialSwitchValue &value : material.Variant)
    {
        variant.Add({String(value.Name.AsView()), value.Value});
    }
    const String key = MakeMaterialShaderKey("", material.Shader.AsView(), variant.AsSpan());
    uint32 index = ~0u;
    for (uint32 p = 0; p < _permutations.Count(); ++p)
    {
        index = _permutations[p]->Key == key ? p : index;
    }
    const auto stride = static_cast<uint32>(material.Parameters.Count());
    if (index == ~0u)
    {
        index = static_cast<uint32>(_permutations.Count());
        UniquePtr<Permutation> created = MakeUnique<Permutation>();
        created->Key = key;
        created->Shader = String(material.Shader.AsView());
        created->Variant = Move(variant);
        created->Stride = stride;
        _permutations.Add(Move(created));
    }
    Permutation &permutation = *_permutations[index];
    if (permutation.Stride != stride)
    {
        // Two conversions of the struct apart, such as during an edit: the older waits for its next conversion.
        MARLIN_LOG(LogRenderer, Warning, "Material {0} has {1} parameter bytes, its permutation {2}", entry.Uuid,
                   stride, permutation.Stride);
        entry.Failed = true;
        return;
    }
    uint32 element = 0;
    if (!permutation.FreeElements.IsEmpty())
    {
        element = permutation.FreeElements.Pop();
    }
    else
    {
        element = static_cast<uint32>(permutation.Parameters.Count() / Max(stride, 1u));
        permutation.Parameters.Resize(permutation.Parameters.Count() + stride, 0);
    }
    entry.Permutation = index;
    entry.Element = element;
    MARLIN_LOG(LogRenderer, Debug, "Material {0} draws with {1}, element {2} of permutation {3}", material.Name,
               permutation.Key, element, index);
    for (const FrozenString &bsdf : material.CustomBsdfs)
    {
        if (!permutation.CustomBsdfs.Contains(String(bsdf.AsView())))
        {
            permutation.CustomBsdfs.Add(String(bsdf.AsView()));
        }
    }
    for (const MaterialTextureBinding &binding : material.Textures)
    {
        if (binding.Texture.GetSlot() != nullptr && _textures != nullptr)
        {
            (void)_textures->AcquireTexture(binding.Texture);
            entry.Textures.Add(binding.Texture);
        }
    }
    WriteBlock(entry, material);
}

void SceneMaterialRegistry::WriteBlock(Entry &entry, const Material &material)
{
    Permutation &permutation = *_permutations[entry.Permutation];
    uint8 *block = permutation.Parameters.Data() + usize{entry.Element} * permutation.Stride;
    MemCopy(block, material.Parameters.Data(), material.Parameters.Count());
    // Each texture parameter (Marlin.Shading.MaterialTexture2D): its texture's descriptor index, or its default's, the
    // material sampler's, its default as the converter wrote it, then the address of its streaming feedback word.
    for (const MaterialTextureBinding &binding : material.Textures)
    {
        uint32 words[2] = {0, 0};
        uint64 feedback = 0;
        if (_textures != nullptr)
        {
            const SceneDefaultTexture fallback =
                binding.Default == MaterialDefaultTexture::Black        ? SceneDefaultTexture::Black
                : binding.Default == MaterialDefaultTexture::FlatNormal ? SceneDefaultTexture::FlatNormal
                                                                        : SceneDefaultTexture::White;
            words[0] = binding.Texture.GetSlot() != nullptr ? _textures->GetTextureIndex(binding.Texture)
                                                            : _textures->GetDefaultTexture(fallback);
            words[1] = _textures->GetMaterialSampler();
            feedback = binding.Texture.GetSlot() != nullptr ? _textures->GetFeedbackAddress(binding.Texture) : 0;
        }
        if (binding.Offset + sizeof(words) <= permutation.Stride)
        {
            MemCopy(block + binding.Offset, words, sizeof(words));
        }
        if (binding.Offset + FeedbackOffset + sizeof(feedback) <= permutation.Stride)
        {
            MemCopy(block + binding.Offset + FeedbackOffset, &feedback, sizeof(feedback));
        }
    }
}

void SceneMaterialRegistry::Extract(SceneMaterialFrame &frame) const
{
    const ScopeLock lock(_lock);
    frame.Slots.Clear();
    frame.Permutations.Clear();
    // A slot whose material is not placed draws with slot 0's, the Default material, once that is.
    const Entry &fallback = *_entries[0];
    for (const UniquePtr<Entry> &entry : _entries)
    {
        const Entry &drawn = entry->Permutation != ~0u ? *entry : fallback;
        frame.Slots.Add({drawn.Permutation, drawn.Element});
    }
    for (const UniquePtr<Permutation> &permutation : _permutations)
    {
        SceneMaterialFrame::Permutation &copy = frame.Permutations.AddDefaulted();
        copy.Shader = permutation->Shader;
        copy.Variant = permutation->Variant;
        copy.Stride = permutation->Stride;
        copy.Parameters = permutation->Parameters;
        copy.CustomBsdfs = permutation->CustomBsdfs;
    }
}
} // namespace Marlin
