#pragma once

#include "Asset/Asset.h"
#include "Containers/FrozenArray.h"
#include "CoreTypes.h"
#include "Misc/Guid.h"
#include "String/StringView.h"
#include "ScriptVM/ScriptModuleAsset.gen.h"

namespace Marlin
{
/// The compiled script module of one package (MarlinScript §1.2, §11.1): the bytes WriteScriptModule gives, which
/// ScriptVM::AddModule reads. Its Guid is GetScriptModuleGuid of the package; like a global shader it is not an asset
/// of the content tree but loads like one, from GlobalStore in development and from packages when packaged.
MCLASS()
class SCRIPTVM_API ScriptModuleAsset final : public Asset
{
    MGENERATED_BODY()
public:
    MPROPERTY()
    FrozenString Package;
    MPROPERTY()
    FrozenArray<uint8> Bytecode;

    ScriptModuleAsset() = default;
    ~ScriptModuleAsset() override = default;
    MARLIN_NONCOPYABLE(ScriptModuleAsset);
};

/// The namespace of script module Guids.
inline constexpr Guid ScriptModuleNamespace{0x2b7e9c41u, 0x6d0a4f58u, 0xb31c8e92u, 0x57f4a0d6u};

/// The Guid the module of *package* loads under, the same in the derived data cache and in packages.
[[nodiscard]] SCRIPTVM_API Guid GetScriptModuleGuid(StringView package);
} // namespace Marlin
