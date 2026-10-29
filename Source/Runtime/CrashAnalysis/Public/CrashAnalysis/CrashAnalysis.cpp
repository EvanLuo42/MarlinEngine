#include "CrashAnalysis/CrashAnalysis.h"

#include "Concurrency/ScopeLock.h"
#include "CrashAnalysis/Minidump.h"
#include "CrashSymbols.h"
#include "HAL/PlatformSync.h"
#include "Hash/Xxh3.h"
#include "Memory/UniquePtr.h"
#include "PeImage.h"
#include "String/Format.h"
#include "String/StringBuilder.h"
#include "X64Unwinder.h"

namespace Marlin::CrashAnalyzer
{
namespace
{
constexpr uint32 SignatureFrames = 5;
/// The bytes of images kept between analyses.
constexpr usize ImageCacheBudget = usize{256} << 20;

Mutex _analysisLock;

/// The images earlier analyses loaded, least recently used first, so a server analyzing the reports of one build
/// reads each image once. An image is known by its path, time stamp and size, as PeImage::Load checks; only loaded
/// images are kept, so one that appears later is found then. Used under _analysisLock.
class ImageCache
{
public:
    /// The image at *path* with this time stamp and size; null if there is none.
    const PeImage *Load(const String &path, uint32 timeStamp, uint32 size)
    {
        for (usize i = 0; i < _images.Count(); ++i)
        {
            if (_images[i].TimeStamp == timeStamp && _images[i].Size == size && _images[i].Path == path)
            {
                Cached cached = Move(_images[i]);
                _images.RemoveAt(i);
                return _images.Add(Move(cached)).Image.Get();
            }
        }
        UniquePtr<PeImage> image = MakeUnique<PeImage>();
        if (!image->Load(path.AsView(), timeStamp, size))
        {
            return nullptr;
        }
        _bytes += image->GetSize();
        return _images.Add({path, timeStamp, size, Move(image)}).Image.Get();
    }

    /// Lets go of the least recently used images beyond the budget; the images it returned are no longer used.
    void Trim()
    {
        usize drop = 0;
        while (drop < _images.Count() && _bytes > ImageCacheBudget)
        {
            _bytes -= _images[drop++].Image->GetSize();
        }
        _images.RemoveAt(0, drop);
    }

private:
    struct Cached
    {
        String Path;
        uint32 TimeStamp = 0;
        uint32 Size = 0;
        UniquePtr<PeImage> Image;
    };

    List<Cached> _images;
    usize _bytes = 0;
};

ImageCache &GetImageCache()
{
    static ImageCache cache;
    return cache;
}

// RECOVERY-DECISION D15: "abort" and "raise" complete the list; the other names come from the recovered strings.
/// Functions of the crash handling itself, which every crash of a kind shares; the signature starts below them.
constexpr StringView NoiseFunctions[] = {
    "KiUserExceptionDispatcher",
    "RtlRaiseException",
    "RaiseException",
    "_CxxThrowException",
    "__chkstk",
    "abort",
    "raise",
    "_purecall",
    "_invalid_parameter",
    "_invoke_watson",
    "Marlin::Private::AssertFailed",
    "Marlin::Private::FatalError",
    "Marlin::Private::FailFormatted",
    "Marlin::Private::WriteLog",
    "Marlin::Private::`anonymous namespace'::Fail",
    "Marlin::`anonymous namespace'::RaiseFailure",
    "Marlin::`anonymous namespace'::OnPureCall",
    "Marlin::`anonymous namespace'::OnInvalidParameter",
    "Marlin::`anonymous namespace'::OnAbort",
};

bool IsNoise(const CrashFrame &frame)
{
    for (const StringView function : NoiseFunctions)
    {
        if (frame.Function == function)
        {
            return true;
        }
    }
    return false;
}

String FileNameOf(StringView path)
{
    const usize slash = path.LastIndexOf('/');
    const usize backslash = path.LastIndexOf('\\');
    usize start = 0;
    if (slash != StringView::NotFound)
    {
        start = slash + 1;
    }
    if (backslash != StringView::NotFound && backslash + 1 > start)
    {
        start = backslash + 1;
    }
    return String(path.Substring(start));
}

void AppendHexBytes(StringWriter &output, Span<const uint8> bytes)
{
    constexpr char Digits[] = "0123456789ABCDEF";
    for (const uint8 byte : bytes)
    {
        output.Append(Digits[byte >> 4]);
        output.Append(Digits[byte & 0xF]);
    }
}
} // namespace

bool Analyze(const CrashAnalysisDesc &desc, CrashAnalysisResult &result, String &error)
{
    const ScopeLock lock(_analysisLock);
    result = CrashAnalysisResult();
    Minidump dump;
    if (!dump.Open(desc.Minidump, error))
    {
        return false;
    }
    if (dump.GetArch() != MinidumpArch::X64)
    {
        error = "Only x64 stacks can be walked";
        return false;
    }
    result.HasException = dump.HasException();
    result.ExceptionCode = dump.GetExceptionCode();
    result.ExceptionAddress = dump.GetExceptionAddress();

    // The images give the unwind data that the dump leaves out: from the symbol store, else where they were.
    const Span<const MinidumpModule> modules = dump.GetModules();
    ImageCache &cache = GetImageCache();
    List<const PeImage *> images;
    List<String> imagePaths;
    for (const MinidumpModule &module : modules)
    {
        String path = desc.SymbolStore.IsEmpty() ? module.Path : GetImagePath(desc.SymbolStore, module);
        const PeImage *image = cache.Load(path, module.TimeStamp, module.Size);
        if (image == nullptr && !desc.SymbolStore.IsEmpty())
        {
            path = module.Path;
            image = cache.Load(path, module.TimeStamp, module.Size);
        }
        images.Add(image);
        const bool found = image != nullptr;
        CrashModuleInfo &info = result.Modules.AddDefaulted();
        info.Name = FileNameOf(module.Path);
        info.Base = module.Base;
        info.Size = module.Size;
        info.HasImage = found;
        imagePaths.Add(found ? Move(path) : String());
    }
    Private::LoadSymbols(dump, desc, imagePaths.AsSpan(), result);

    const auto read = [&](uint64 address, void *buffer, usize size) { return dump.ReadMemory(address, buffer, size); };
    const auto find = [&](uint64 address, uint64 &base) -> const PeImage * {
        const MinidumpModule *module = dump.FindModule(address);
        if (module == nullptr)
        {
            return nullptr;
        }
        const auto index = static_cast<usize>(module - modules.Data());
        base = module->Base;
        return images[index];
    };

    const Span<const MinidumpThread> threads = dump.GetThreads();
    result.FocusThread = desc.FocusThread != 0 ? desc.FocusThread
                         : dump.HasException() ? dump.GetExceptionThread()
                         : threads.IsEmpty()   ? 0
                                               : threads[0].Id;
    for (const MinidumpThread &thread : threads)
    {
        CrashThreadStack &stack = result.Threads.AddDefaulted();
        stack.Id = thread.Id;
        stack.Focus = thread.Id == result.FocusThread;
        const bool crashed = dump.HasException() && thread.Id == dump.GetExceptionThread();
        X64Registers registers;
        if (!ReadX64Context(crashed ? dump.GetExceptionContext() : thread.Context, registers))
        {
            continue;
        }
        for (uint32 depth = 0; depth < desc.MaxFrames && registers.Rip != 0; ++depth)
        {
            CrashFrame &frame = stack.Frames.AddDefaulted();
            frame.Address = registers.Rip;
            if (const MinidumpModule *module = dump.FindModule(registers.Rip))
            {
                frame.Module = FileNameOf(module->Path);
                frame.Offset = registers.Rip - module->Base;
            }
            // A return address points after the call; the call itself is the line that matters.
            const uint64 lookup = depth == 0 ? registers.Rip : registers.Rip - 1;
            Private::Symbolize(lookup, frame);
            if (!frame.Function.IsEmpty())
            {
                frame.Offset += registers.Rip - lookup;
            }
            const uint64 stackPointer = registers.Gpr[4];
            if (!UnwindX64(registers, read, find) || registers.Gpr[4] <= stackPointer)
            {
                break;
            }
        }
    }
    Private::UnloadSymbols();
    cache.Trim();

    for (const CrashThreadStack &stack : result.Threads)
    {
        if (stack.Focus)
        {
            result.Signature = ComputeSignature(stack.Frames.AsSpan(), result.SignatureText);
        }
    }
    return true;
}

Hash128 ComputeSignature(Span<const CrashFrame> frames, String &text)
{
    StringBuilder<1024> lines;
    usize first = 0;
    while (first < frames.Count() && IsNoise(frames[first]))
    {
        ++first;
    }
    uint32 count = 0;
    for (usize i = first; i < frames.Count() && count < SignatureFrames; ++i, ++count)
    {
        const CrashFrame &frame = frames[i];
        if (!frame.Function.IsEmpty())
        {
            FormatTo(lines, "{0}!{1}+0x{2:X}", frame.Module, frame.Function, frame.Offset);
        }
        else
        {
            FormatTo(lines, "{0}+0x{1:X}", frame.Module, frame.Offset);
        }
        lines.Append('\n');
    }
    text = lines.ToString();
    return Xxh3Hash128(text.Data(), text.Length());
}

String FormatSignature(const Hash128 &signature)
{
    return Format("{0:X16}{1:X16}", signature.High, signature.Low);
}

void WriteText(StringWriter &output, const CrashAnalysisResult &result)
{
    if (result.HasException)
    {
        FormatTo(output, "Exception 0x{0:X8} at 0x{1:X16}, thread {2}\n", result.ExceptionCode, result.ExceptionAddress,
                 result.FocusThread);
    }
    FormatTo(output, "Signature {0}\n", FormatSignature(result.Signature));
    output.Append(result.SignatureText);
    for (const CrashThreadStack &stack : result.Threads)
    {
        output.Append('\n');
        FormatTo(output, "Thread {0}{1}\n", stack.Id, stack.Focus ? StringView(" (focus)") : StringView());
        for (usize i = 0; i < stack.Frames.Count(); ++i)
        {
            const CrashFrame &frame = stack.Frames[i];
            FormatTo(output, "  #{0:D2} 0x{1:X16} ", i, frame.Address);
            if (!frame.Function.IsEmpty())
            {
                output.Append(frame.Module).Append('!').Append(frame.Function);
                FormatTo(output, "+0x{0:X}", frame.Offset);
            }
            else
            {
                FormatTo(output, "{0}+0x{1:X}", frame.Module, frame.Offset);
            }
            if (!frame.File.IsEmpty())
            {
                output.Append(' ').Append(frame.File).Append(':');
                FormatTo(output, "{0}", frame.Line);
            }
            output.Append('\n');
        }
    }
    output.Append("\nModules\n");
    for (const CrashModuleInfo &module : result.Modules)
    {
        FormatTo(output, "  0x{0:X16} {1:X8} {2}{3}{4}\n", module.Base, module.Size, module.Name,
                 module.HasImage ? StringView() : StringView(" (no image)"),
                 module.HasSymbols ? StringView() : StringView(" (no symbols)"));
    }
}

String GetImagePath(StringView store, const MinidumpModule &module)
{
    return Format("{0}/{1}/{2:X8}{3:X}/{1}", store, FileNameOf(module.Path), module.TimeStamp, module.Size);
}

String GetDebugFilePath(StringView store, const MinidumpModule &module)
{
    if (module.DebugFile.IsEmpty() || module.DebugId.IsEmpty())
    {
        return {};
    }
    StringBuilder<512> path(store);
    path.Append('/').Append(module.DebugFile).Append('/');
    AppendHexBytes(path, module.DebugId.AsSpan());
    FormatTo(path, "{0:X}", module.DebugAge);
    path.Append('/').Append(module.DebugFile);
    return path.ToString();
}
} // namespace Marlin::CrashAnalyzer
