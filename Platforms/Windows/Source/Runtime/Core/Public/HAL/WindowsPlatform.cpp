#include "HAL/Platform.h"

#include "HAL/PlatformCpu.h"
#include "HAL/PlatformCrash.h"
#include "HAL/PlatformDebug.h"
#include "HAL/PlatformProcess.h"

#include "HAL/MicrosoftHeaders.h"

namespace Marlin::Platform
{
namespace
{
UINT _previousConsoleCodePage = 0;
UINT _previousConsoleOutputCodePage = 0;
bool _socketsStarted = false;
} // namespace

void Initialize()
{
    if (!PlatformCpu::SupportsRequiredIsa())
    {
        PlatformDebug::Output("This CPU lacks AVX2, which this build requires.\n");
        MessageBoxW(nullptr, L"This CPU lacks AVX2, which this build requires.", L"Marlin", MB_OK | MB_ICONERROR);
        PlatformProcess::Exit(1);
    }

    _previousConsoleCodePage = GetConsoleCP();
    _previousConsoleOutputCodePage = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    ULONG stackGuarantee = PlatformCrash::HandlerStackReserve;
    SetThreadStackGuarantee(&stackGuarantee);

    WSADATA data;
    _socketsStarted = WSAStartup(MAKEWORD(2, 2), &data) == 0;
}

void Shutdown()
{
    if (_socketsStarted)
    {
        WSACleanup();
        _socketsStarted = false;
    }
    if (_previousConsoleCodePage != 0)
    {
        SetConsoleCP(_previousConsoleCodePage);
    }
    if (_previousConsoleOutputCodePage != 0)
    {
        SetConsoleOutputCP(_previousConsoleOutputCodePage);
    }
}
} // namespace Marlin::Platform
