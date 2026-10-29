#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "LanguageFrontend/Source.h"
#include "String/StringView.h"

namespace Marlin
{
/// The names a compilation looked up outside local scopes, so that a file it did not use can be checked for
/// declarations that would change what it found (§1.3). A name is a container's qualified name, such as
/// `Marlin.Shading` (empty for the global namespace), with a member name, hashed by HashLanguageName. Both lists are
/// sorted.
struct LanguageNameDependencies
{
    /// Names whose lookup found no namespace: any declaration of them can change the result.
    List<uint64> Names;
    /// Names whose lookup found a namespace: another declaration of that namespace only adds to it, so only
    /// declarations of other kinds can change the result.
    List<uint64> NamespaceNames;
    /// Whether the extensions of some type were matched: any extension declaration may apply to it.
    bool Extensions = false;

    /// Adds the names of *other*, as when one result covers several compilations.
    LANGUAGEFRONTEND_API void Merge(const LanguageNameDependencies &other);
};

/// The names one file declares, as LanguageNameDependencies hashes them. Both lists are sorted.
struct LanguageDeclaredNames
{
    /// Declarations other than namespaces: types, functions, fields, enum members, aliases and the members of
    /// types.
    List<uint64> Names;
    /// Namespace declarations, each part of a dotted name.
    List<uint64> Namespaces;
    /// Whether the file declares an extension.
    bool Extensions = false;
};

/// The hash of member *name* of the container with qualified name *container*.
[[nodiscard]] LANGUAGEFRONTEND_API uint64 HashLanguageName(StringView container, StringView name);

/// The names the file *path* with *text* declares, from a scan of its declarations (bodies are not parsed). A file
/// that does not parse declares what parsed.
LANGUAGEFRONTEND_API void GetDeclaredNames(StringView path, String text, LanguageDeclaredNames &names);

/// Whether adding or changing a file that declares *declared* can change what a compilation that looked up
/// *dependencies* found, without that compilation having used the file.
[[nodiscard]] LANGUAGEFRONTEND_API bool AffectsNames(const LanguageDeclaredNames &declared,
                                                     const LanguageNameDependencies &dependencies);
} // namespace Marlin
