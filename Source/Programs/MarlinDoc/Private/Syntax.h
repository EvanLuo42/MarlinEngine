#pragma once

#include "Model.h"

namespace Marlin::Doc
{
/// The part of a declaration's text a signature shows, on one line: up to a *function*'s body (and, in C++, its
/// constructor initializers) or a *type*'s members, without comments or `*_API` macros.
String CutDeclaration(StringView text, bool function, bool type, bool cpp);
/// A parameter type as overloads are told apart by: whitespace collapsed and `Marlin::` left out.
String NormalizeType(StringView type);
/// *text* without any whitespace: what a link's parameter list is compared by.
String WithoutWhiteSpace(StringView text);

/// Gives each overload an id of its own, by its parameter types, and fills in every item's Children.
void FinishItems(Module &module);
} // namespace Marlin::Doc
