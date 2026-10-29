#include "Engine/Engine.h"
#include "Splash/SplashSubsystem.h"

int main()
{
    Marlin::LaunchDesc desc;
    desc.TargetName = "Editor";
    desc.Splash = Marlin::Splash::GetStartupSplash();
    return Marlin::LaunchMain(desc);
}
