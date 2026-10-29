#pragma once

#include "Asset/AssetRef.h"
#include "Concurrency/SpinLock.h"
#include "Containers/HashMap.h"
#include "Containers/List.h"
#include "CoreTypes.h"
#include "Material/Material.h"
#include "Memory/UniquePtr.h"
#include "Misc/Guid.h"
#include "Shader/GlobalShader.h"
#include "String/String.h"

namespace Marlin
{
class SceneTextureResidency;

/// The material table as one frame renders it (Documentation/Architecture/07 §4, MarlinLanguage.md §9.4): every
/// material slot's permutation and its element in that permutation's table, and each permutation's struct, variant and
/// table of parameter blocks, texture indices written in.
struct SceneMaterialFrame
{
    /// A material slot: the permutation it draws with and its element in that permutation's table.
    struct Slot
    {
        uint32 Permutation = 0;
        uint32 Element = 0;
    };
    /// A `[Material]` struct with one variant: what a material pass is compiled for, and the materials that use it.
    struct Permutation
    {
        String Shader;
        List<ShaderVariantValue> Variant;
        /// The bytes of one element, and the elements back to back.
        uint32 Stride = 0;
        List<uint8> Parameters;
        /// The custom BSDFs its materials' closures can carry, by qualified name.
        List<String> CustomBsdfs;
    };
    List<Slot> Slots;
    List<Permutation> Permutations;
};

/// The materials the scene draws with (Documentation/Architecture/07 §4, §5.2): each material asset a mesh slot or an
/// entity names gets a material slot, which the GPU's section table names and Material Resolve looks up. Materials of
/// the same struct and variant share a permutation, one material pass and one table of their parameter blocks.
/// Owned by SceneRendererSubsystem; slot 0 is the engine's Default material, which every slot draws with until its
/// own has loaded.
class SCENERENDERER_API SceneMaterialRegistry
{
public:
    /// The engine's Default material (Content/Materials/Default.masset).
    static constexpr Guid DefaultMaterial{0x2a527f2cu, 0x9f274e67u, 0x8ae93dbcu, 0xf5eb0ee6u};

    SceneMaterialRegistry(SceneTextureResidency *textures);
    ~SceneMaterialRegistry();
    MARLIN_NONCOPYABLE(SceneMaterialRegistry);

    // Thread-safe, for the extract systems and the mesh table.

    /// The slot of *material*, adding a reference; an invalid Guid is the Default material's slot.
    [[nodiscard]] uint32 Acquire(const Guid &material);
    void Release(uint32 slot);

    // On the main thread.

    /// Takes the materials that finished loading into their permutations, refreshes texture indices, and frees the
    /// slots nothing references any more.
    void Update();
    /// The table as it stands, for the render job.
    void Extract(SceneMaterialFrame &frame) const;

private:
    struct Entry
    {
        Guid Uuid;
        AssetRef<Material> Ref;
        uint32 References = 0;
        /// The permutation and element it draws with once loaded; ~0u before.
        uint32 Permutation = ~0u;
        uint32 Element = ~0u;
        bool Loaded = false;
        bool Failed = false;
        List<AssetRef<Texture>> Textures;
    };
    struct Permutation
    {
        String Key;
        String Shader;
        List<ShaderVariantValue> Variant;
        uint32 Stride = 0;
        List<uint8> Parameters;
        List<uint32> FreeElements;
        List<String> CustomBsdfs;
    };

    /// Places a loaded material in its permutation and writes its block.
    void Place(Entry &entry, const Material &material);
    void WriteBlock(Entry &entry, const Material &material);

    SceneTextureResidency *_textures = nullptr;
    mutable SpinLock _lock;
    List<UniquePtr<Entry>> _entries;
    HashMap<Guid, uint32> _byGuid;
    List<uint32> _freeSlots;
    List<UniquePtr<Permutation>> _permutations;
    uint32 _textureStamp = ~0u;
};
} // namespace Marlin
