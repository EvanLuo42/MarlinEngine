#pragma once

#include "Containers/List.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "ScriptVM/ScriptModuleData.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
struct ScriptCompileOptions
{
    /// Whether `Debug.Print` and `Debug.Assert` generate code (MarlinScript §10.3).
    bool DebugOutput = false;
    /// Whether functions keep line tables for fault stacks and debuggers.
    bool DebugInfo = true;
};

/// A source file a compile read, as a virtual path, with the hash of its text as it was read.
struct ScriptSourceDependency
{
    String Path;
    Hash128 Hash;
};

struct ScriptCompileResult
{
    bool Succeeded = false;
    String Errors;
    ScriptModuleData Module;
    /// Every `.mscript` and `.mcommon` file of the source set, sorted by path: a package sees all of them
    /// (MarlinScript §1.3), so a new file or an edit to any of them can change it.
    List<ScriptSourceDependency> Dependencies;
};

/// The source roots of scripts: /Engine, and /Project if one is mounted.
SCRIPTPIPELINE_API void GetScriptSourceRoots(List<String> &roots);

/// Every `.mscript` and `.mcommon` file under the source roots, as virtual paths sorted by path.
SCRIPTPIPELINE_API void GetScriptSourceFiles(List<String> &paths);

/// The script packages of the mounted source set (MarlinScript §1.2): its mount roots, `Engine` and `Project` if one
/// is mounted. A package may have no `.mscript` files; its module is then empty.
SCRIPTPIPELINE_API void GetScriptPackages(List<String> &packages);

/// Whether *files*, as GetScriptSourceFiles lists them, have `.mscript` files of *package*.
[[nodiscard]] SCRIPTPIPELINE_API bool HasScripts(StringView package, Span<const String> files);

/// Compiles the script package *package* of the mounted source set: front end, the MIR script pipeline, and the
/// ScriptBackend's bytecode module.
SCRIPTPIPELINE_API ScriptCompileResult CompileScriptPackage(StringView package, const ScriptCompileOptions &options);
} // namespace Marlin
