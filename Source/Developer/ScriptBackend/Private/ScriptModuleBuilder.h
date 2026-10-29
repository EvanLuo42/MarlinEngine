#pragma once

#include "Containers/HashMap.h"
#include "LanguageIR/MirModule.h"
#include "LanguageIR/MirTarget.h"
#include "ScriptVM/ScriptModuleData.h"

namespace Marlin
{
/// Builds the tables of a script module from MIR: strings, shapes, types, statics, imports and exports; functions
/// are compiled by CompileScriptFunction.
class ScriptModuleBuilder
{
public:
    ScriptModuleBuilder(MirModule &module, const MirTargetOptions &options, ScriptModuleData &data,
                        List<String> &errors);
    MARLIN_NONCOPYABLE(ScriptModuleBuilder);

    bool Build();

    [[nodiscard]] MirModule &GetModule() const
    {
        return *_module;
    }

    [[nodiscard]] const MirTargetOptions &GetOptions() const
    {
        return *_options;
    }

    uint32 AddString(StringView text);
    /// The module's index for string *index* of the MIR module's string table.
    uint32 MirString(uint32 index);
    uint32 ShapeOf(const MirType *type);
    [[nodiscard]] uint32 FunctionIndexOf(const MirFunction *function) const;
    /// The byte offset of a static field in the static area, or ~0u when *global* is not one.
    [[nodiscard]] uint32 StaticOffsetOf(const MirGlobal *global) const;
    void Error(String message);

private:
    void AddTypes();
    void AddStatics();
    void AddLinkage();
    [[nodiscard]] uint64 SignatureHash(const MirType *type) const;

    MirModule *_module;
    const MirTargetOptions *_options;
    ScriptModuleData *_data;
    List<String> *_errors;
    HashMap<String, uint32> _strings;
    HashMap<const MirType *, uint32> _shapes;
    HashMap<const MirFunction *, uint32> _functions;
    HashMap<const MirGlobal *, uint32> _statics;
    HashMap<const MirType *, uint32> _referenceTypes;
    bool _ok = true;
};

/// Compiles *function*, which has a body, into *record*: allocates its registers, selects instructions and
/// records its safepoints and line table (MarlinScript §9.3, §14).
void CompileScriptFunction(ScriptModuleBuilder &builder, MirFunction &function, ScriptFunctionRecord &record);
} // namespace Marlin
