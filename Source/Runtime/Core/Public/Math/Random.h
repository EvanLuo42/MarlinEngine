//! Deterministic pseudo-random number generation.
//!
//! `Random` is a `xoshiro256**` generator seeded through SplitMix64. `ThreadRandom` provides a per-thread instance.
//! The generator is fast and deterministic, but not suitable for cryptography.

#pragma once

#include "Containers/Span.h"
#include "CoreTypes.h"
#include "Templates/Bit.h"
#include "Templates/TypeTraits.h"
#include "Templates/Utility.h"

namespace Marlin
{
/// Advances `state` once and returns the resulting SplitMix64 value.
///
/// The output spreads a seed over the state of a generator; it is not a generator by itself.
[[nodiscard]] constexpr uint64 SplitMix64(uint64 &state)
{
    state += 0x9E3779B97F4A7C15ull;
    uint64 value = state;
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

/// A fast, deterministic pseudo-random generator (`xoshiro256**`).
///
/// The raw 64-bit sequence is identical for a seed on every platform. `NextGaussian` also uses the platform's `log`,
/// so its values may differ. Use `PlatformSystem::GetSecureRandomBytes` for cryptographic randomness.
///
/// # Thread safety
/// A `Random` owns mutable state and is not synchronized. Use one instance per thread.
///
/// # Examples
///
/// ```cpp
/// Random a(7);
/// Random b(7);
/// const bool same = a.NextUInt64() == b.NextUInt64(); // -> true
/// ```
class Random
{
public:
    /// Creates a generator and seeds it with `seed`; `seed` defaults to 0.
    constexpr explicit Random(uint64 seed = 0)
    {
        Seed(seed);
    }

    /// Creates a generator seeded from the operating system's secure random source.
    [[nodiscard]] CORE_API static Random FromSecureSeed();

    /// Re-seeds the generator; the same seed always produces the same sequence.
    ///
    /// Resets the state, so the next value is the first value for that seed.
    constexpr void Seed(uint64 seed)
    {
        for (uint64 &word : _state)
        {
            word = SplitMix64(seed);
        }
    }

    /// Returns the next 64 random bits.
    constexpr uint64 NextUInt64()
    {
        const uint64 result = RotateLeft(_state[1] * 5, 7) * 9;
        const uint64 shifted = _state[1] << 17;
        _state[2] ^= _state[0];
        _state[3] ^= _state[1];
        _state[1] ^= _state[2];
        _state[0] ^= _state[3];
        _state[2] ^= shifted;
        _state[3] = RotateLeft(_state[3], 45);
        return result;
    }

    /// Returns the next 32 random bits, taken from the high half of a 64-bit step.
    constexpr uint32 NextUInt32()
    {
        return static_cast<uint32>(NextUInt64() >> 32);
    }

    /// Returns a uniform value greater than or equal to 0 and less than `bound`.
    ///
    /// The result has no modulo bias.
    ///
    /// # Preconditions
    /// `bound` is greater than zero. The behavior is undefined when it is zero.
    constexpr uint64 NextBelow(uint64 bound)
    {
        Product128 product = Multiply128(NextUInt64(), bound);
        if (product.Low < bound)
        {
            const uint64 threshold = (0 - bound) % bound;
            while (product.Low < threshold)
            {
                product = Multiply128(NextUInt64(), bound);
            }
        }
        return product.High;
    }

    /// Returns a uniform value from `min` through `max`, with both ends included.
    ///
    /// # Preconditions
    /// `min <= max`. The values are not checked.
    template <Integral T> constexpr T NextInRange(T min, T max)
    {
        using TUnsigned = MakeUnsigned<T>;
        const auto span = static_cast<uint64>(static_cast<TUnsigned>(max) - static_cast<TUnsigned>(min));
        const uint64 offset = span == ~uint64{0} ? NextUInt64() : NextBelow(span + 1);
        return static_cast<T>(static_cast<TUnsigned>(min) + static_cast<TUnsigned>(offset));
    }

    /// Returns a uniform `float32` in [0, 1).
    constexpr float32 NextFloat()
    {
        return static_cast<float32>(NextUInt64() >> 40) * 0x1.0p-24f;
    }

    /// Returns a uniform `float32` greater than or equal to `min` and less than `max`.
    ///
    /// `min` must not exceed `max`.
    constexpr float32 NextFloat(float32 min, float32 max)
    {
        return min + (max - min) * NextFloat();
    }

    /// Returns a uniform `float64` in [0, 1).
    constexpr float64 NextDouble()
    {
        return static_cast<float64>(NextUInt64() >> 11) * 0x1.0p-53;
    }

    /// Returns a uniform `float64` greater than or equal to `min` and less than `max`.
    ///
    /// `min` must not exceed `max`.
    constexpr float64 NextDouble(float64 min, float64 max)
    {
        return min + (max - min) * NextDouble();
    }

    /// Returns a random boolean, with equal probability for either value.
    constexpr bool NextBool()
    {
        return (NextUInt64() >> 63) != 0;
    }

    /// Returns a normally distributed `float64` with mean 0 and standard deviation 1.
    ///
    /// Uses rejection sampling, so a call consumes a variable number of 64-bit values. The platform's `log` can make
    /// the result differ between platforms.
    float64 NextGaussian()
    {
        while (true)
        {
            const float64 x = NextDouble(-1, 1);
            const float64 y = NextDouble(-1, 1);
            const float64 radius = x * x + y * y;
            if (radius > 0 && radius < 1)
            {
                return x * __builtin_sqrt(-2 * __builtin_log(radius) / radius);
            }
        }
    }

    /// Fills `buffer` with `size` pseudo-random bytes.
    ///
    /// # Safety
    /// `buffer` points to at least `size` writable bytes.
    constexpr void NextBytes(void *buffer, usize size)
    {
        usize offset = 0;
        while (offset < size)
        {
            uint64 value = NextUInt64();
            for (usize i = 0; i < 8 && offset < size; ++i)
            {
                static_cast<uint8 *>(buffer)[offset++] = static_cast<uint8>(value);
                value >>= 8;
            }
        }
    }

    /// Shuffles `values` into a uniformly random permutation.
    ///
    /// The permutation is generated with the Fisher-Yates algorithm.
    template <typename T> constexpr void Shuffle(Span<T> values)
    {
        for (usize i = values.Count(); i > 1; --i)
        {
            Swap(values[i - 1], values[static_cast<usize>(NextBelow(i))]);
        }
    }

    /// Advances the generator by 2^128 steps.
    ///
    /// Jumping gives non-overlapping streams from one seed for parallel work; computing the jump takes 256
    /// generator steps.
    constexpr void Jump()
    {
        constexpr uint64 Polynomial[4] = {
            0x180EC6D33CFD0ABAull,
            0xD5A61266F0C9392Cull,
            0xA9582618E03FC9AAull,
            0x39ABDC4529B1661Cull,
        };
        uint64 next[4] = {};
        for (const uint64 word : Polynomial)
        {
            for (uint32 bit = 0; bit < 64; ++bit)
            {
                if ((word & (uint64{1} << bit)) != 0)
                {
                    for (usize i = 0; i < 4; ++i)
                    {
                        next[i] ^= _state[i];
                    }
                }
                (void)NextUInt64();
            }
        }
        for (usize i = 0; i < 4; ++i)
        {
            _state[i] = next[i];
        }
    }

private:
    uint64 _state[4] = {};
};

/// Returns this thread's generator.
///
/// The generator is seeded from the operating system's secure random source on first use, so its sequence is not
/// reproducible; seed a `Random` for anything that must replay.
///
/// # Thread safety
/// Each thread has its own generator, so calls from different threads do not interact.
CORE_API Random &ThreadRandom();
} // namespace Marlin
