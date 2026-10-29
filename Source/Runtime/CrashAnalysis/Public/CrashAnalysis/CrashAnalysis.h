#pragma once

#include "Containers/List.h"
#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Hash/Hash128.h"
#include "String/String.h"
#include "String/StringView.h"

namespace Marlin
{
class StringWriter;
struct MinidumpModule;

struct CrashFrame
{
    uint64 Address = 0;
    /// The module's file name; empty outside every module.
    String Module;
    /// Empty without symbols.
    String Function;
    String File;
    uint32 Line = 0;
    /// From the start of the function, or of the module without symbols.
    uint64 Offset = 0;
};

struct CrashThreadStack
{
    uint32 Id = 0;
    /// The thread the report is about: the one that crashed, or the hung main thread.
    bool Focus = false;
    List<CrashFrame> Frames;
};

struct CrashModuleInfo
{
    String Name;
    uint64 Base = 0;
    uint32 Size = 0;
    bool HasImage = false;
    bool HasSymbols = false;
};

struct CrashAnalysisDesc
{
    StringView Minidump;
    /// A symbol store `marlin symbols` wrote; empty to look beside the modules' original paths, as on the machine
    /// that crashed.
    StringView SymbolStore;
    /// The thread the signature comes from; zero for the exception's thread, or the first without an exception.
    uint32 FocusThread = 0;
    uint32 MaxFrames = 64;
};

struct CrashAnalysisResult
{
    bool HasException = false;
    uint32 ExceptionCode = 0;
    uint64 ExceptionAddress = 0;
    uint32 FocusThread = 0;
    List<CrashThreadStack> Threads;
    List<CrashModuleInfo> Modules;
    /// The frames the signature is made of, one per line.
    String SignatureText;
    Hash128 Signature;
};

/// Analyzes minidumps (Documentation/Architecture/03 §7.1): every thread's stack, walked and symbolized, and a
/// signature that the same bug gives every time. Symbol loading is not thread-safe on every platform, so analyses
/// run one at a time.
namespace CrashAnalyzer
{
/// Fails only if the dump cannot be read; frames without symbols keep their module and offset.
CRASHANALYSIS_API bool Analyze(const CrashAnalysisDesc &desc, CrashAnalysisResult &result, String &error);
/// The top five frames of *frames* outside the crash machinery, hashed; *text* gets them.
[[nodiscard]] CRASHANALYSIS_API Hash128 ComputeSignature(Span<const CrashFrame> frames, String &text);
/// 32 hexadecimal digits.
[[nodiscard]] CRASHANALYSIS_API String FormatSignature(const Hash128 &signature);
CRASHANALYSIS_API void WriteText(StringWriter &output, const CrashAnalysisResult &result);

/// Where a symbol store keeps *module*'s image and debug file: `<store>/<file>/<key>/<file>`.
[[nodiscard]] CRASHANALYSIS_API String GetImagePath(StringView store, const MinidumpModule &module);
/// Empty if the module names no debug file.
[[nodiscard]] CRASHANALYSIS_API String GetDebugFilePath(StringView store, const MinidumpModule &module);
} // namespace CrashAnalyzer
} // namespace Marlin
