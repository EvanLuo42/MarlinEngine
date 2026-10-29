#include "Engine/Engine.h"

int main()
{
    Marlin::LaunchDesc desc;
    desc.TargetName = "Game";
    return Marlin::LaunchMain(desc);
}
