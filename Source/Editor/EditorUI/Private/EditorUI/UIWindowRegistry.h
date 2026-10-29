#pragma once

#include "CoreTypes.h"

namespace Marlin
{
class UIElementNode;
class UIWindowRoot;

namespace Private
{
void RegisterWindow(UIWindowRoot *window);
void UnregisterWindow(UIWindowRoot *window);
/// Every window forgets *node*: hover, press, focus and pending work.
void NotifyNodeDestroyed(UIElementNode *node);
} // namespace Private
} // namespace Marlin
