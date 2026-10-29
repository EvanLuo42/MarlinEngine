#include "ScriptVM/ScriptModuleAsset.h"

namespace Marlin
{
Guid GetScriptModuleGuid(StringView package)
{
    return Guid::FromName(ScriptModuleNamespace, package);
}
} // namespace Marlin
