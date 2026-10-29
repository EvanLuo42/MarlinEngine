#include "GlobalStoreAssetStore/AssetLocate.h"

#include "Diagnostics/Log.h"
#include "Engine/Engine.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformThread.h"
#include "HAL/PlatformTime.h"
#include "Remote/RemoteClient.h"
#include "String/Ascii.h"
#include "String/Format.h"
#include "String/Number.h"
#include "String/StringBuilder.h"

namespace Marlin
{
String FormatResultKey(const Hash128 &key)
{
    constexpr char Digits[] = "0123456789abcdef";
    char text[32];
    for (usize i = 0; i < 16; ++i)
    {
        text[i] = Digits[(key.High >> (60 - i * 4)) & 0xF];
        text[16 + i] = Digits[(key.Low >> (60 - i * 4)) & 0xF];
    }
    return String(StringView(text, 32));
}

bool ParseResultKey(StringView text, Hash128 &key)
{
    if (text.Length() != 32)
    {
        return false;
    }
    Hash128 parsed;
    for (usize i = 0; i < 32; ++i)
    {
        const char c = text[i];
        const uint64 digit = c >= '0' && c <= '9'   ? static_cast<uint64>(c - '0')
                             : c >= 'a' && c <= 'f' ? static_cast<uint64>(c - 'a' + 10)
                             : c >= 'A' && c <= 'F' ? static_cast<uint64>(c - 'A' + 10)
                                                    : 16;
        if (digit == 16)
        {
            return false;
        }
        uint64 &half = i < 16 ? parsed.High : parsed.Low;
        half = (half << 4) | digit;
    }
    key = parsed;
    return true;
}
} // namespace Marlin

namespace Marlin::AssetIndexer
{
namespace
{
uint64 Milliseconds()
{
    return PlatformTime::Ticks() * 1000 / PlatformTime::TicksPerSecond();
}
} // namespace

String PipeNameFor(StringView projectRoot)
{
    uint32 hash = 0x811C9DC5u;
    for (const char c : projectRoot)
    {
        const char folded = c == '\\' ? '/' : (MARLIN_PLATFORM_WINDOWS ? Ascii::ToLower(c) : c);
        hash = (hash ^ static_cast<uint8>(folded)) * 0x01000193u;
    }
    char digits[16];
    const usize count = FormatUnsigned(hash, digits, 16);
    StringBuilder<64> name("Marlin.AssetIndexer.");
    name.Append(8 - count, '0').Append(StringView(digits, count));
    return name.ToString();
}

bool Connect(RemoteClient &client, StringView projectRoot, StringView projectFile, bool launch, uint32 timeout)
{
    const String pipe = PipeNameFor(projectRoot);
    if (client.ConnectPipe(pipe.AsView(), 200))
    {
        return true;
    }
    if (!launch)
    {
        return false;
    }
    const String executable = Format("{0}/MarlinAssetIndexer{1}", Engine::GetEnvironment().ExecutableDirectory,
                                     MARLIN_PLATFORM_WINDOWS ? ".exe" : "");
    // Started for this client, it exits once no client has used it for a while (06 §2).
    const String arguments = Format("-Project=\"{0}\" -ExitWhenIdle", projectFile);
    ProcessDesc desc;
    desc.Executable = executable.CStr();
    desc.Arguments = arguments.CStr();
    desc.Flags = ProcessFlags::Hidden;
    ProcessHandle process;
    if (!PlatformProcess::Create(desc, process))
    {
        MARLIN_LOG(LogCore, Error, "Cannot start {0}", executable);
        return false;
    }
    PlatformProcess::Close(process);
    const uint64 deadline = Milliseconds() + timeout;
    while (Milliseconds() < deadline)
    {
        if (client.ConnectPipe(pipe.AsView(), 200))
        {
            return true;
        }
        PlatformThread::Sleep(50);
    }
    MARLIN_LOG(LogCore, Error, "MarlinAssetIndexer did not answer on {0}", pipe);
    return false;
}
} // namespace Marlin::AssetIndexer
