#pragma once

#include "LanguageFrontend/Ast.h"
#include "LanguageFrontend/Diagnostics.h"
#include "Memory/Arena.h"

namespace Marlin
{
struct ParseOptions
{
    /// Leaves function, constructor, operator and property bodies unparsed (§1.3); ParseBody parses one later.
    bool SkipBodies = false;
};

/// Tokenizes and parses *file* into *arena*. Errors go to *diagnostics*; the tree holds whatever parsed, with
/// broken declarations and statements left out, so later stages can still report problems in the rest.
LANGUAGEFRONTEND_API AstFile *ParseFile(const SourceFile &file, Arena &arena, LanguageDiagnostics &diagnostics,
                                        const ParseOptions &options = {});

/// Parses a body a declaration scan skipped; does nothing if it is already parsed.
LANGUAGEFRONTEND_API void ParseBody(const AstFile &file, AstBody &body, Arena &arena,
                                    LanguageDiagnostics &diagnostics);
} // namespace Marlin
