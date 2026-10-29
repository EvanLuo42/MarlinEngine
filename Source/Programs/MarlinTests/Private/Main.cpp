#include "Containers/List.h"
#include "Engine/Engine.h"
#include "String/StringView.h"
#include "Testing/Testing.h"

int main(int argc, const char *const *argv)
{
    using namespace Marlin;
    if (!ProgramHost::Initialize("MarlinTests", ProgramFeatures::Vfs))
    {
        return 1;
    }
    List<StringView> arguments;
    for (int i = 1; i < argc; ++i)
    {
        arguments.Add(argv[i]);
    }
    const int32 code = Testing::RunTests(arguments.AsSpan());
    ProgramHost::Shutdown();
    return code;
}
