#include "Testing/Testing.h"

namespace Marlin::Tests
{
MARLIN_TEST(Testing, ExpectPasses)
{
    MARLIN_EXPECT(1 + 1 == 2);
    MARLIN_REQUIRE(true);
}

MARLIN_TEST_FIXTURE(CountingFixture)
{
    int32 Value = 0;

    void SetUp() override
    {
        Value = 41;
    }
};

MARLIN_TEST_F(CountingFixture, Testing, FixtureSetsUp)
{
    ++Value;
    MARLIN_EXPECTF(Value == 42, "Value is {0}", Value);
}

MARLIN_TEST(Testing, SlowIsOptIn, Slow)
{
    MARLIN_EXPECT(true);
}
} // namespace Marlin::Tests
