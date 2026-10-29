#include "Math/Random.h"

#include "HAL/PlatformSystem.h"

namespace Marlin
{
namespace
{
thread_local Random _threadRandom;
thread_local bool _threadRandomSeeded = false;
} // namespace

Random Random::FromSecureSeed()
{
    uint64 seed = 0;
    PlatformSystem::GetSecureRandomBytes(&seed, sizeof(seed));
    return Random(seed);
}

Random &ThreadRandom()
{
    if (MARLIN_UNLIKELY(!_threadRandomSeeded))
    {
        _threadRandom = Random::FromSecureSeed();
        _threadRandomSeeded = true;
    }
    return _threadRandom;
}
} // namespace Marlin
