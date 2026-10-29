#include "EditorShell/EditingContext.h"

namespace Marlin
{
EditingContext::EditingContext(const EditingContextType *type) : Title(CreateSignal(String())), _type(type)
{
    if (type != nullptr)
    {
        Icon = String(type->GetIcon());
        IconColor = String(type->GetIconColor());
    }
}
} // namespace Marlin
