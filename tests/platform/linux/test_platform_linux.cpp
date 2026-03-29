/*
 * tests/platform/linux/test_platform_linux.cpp — Linux platform integration tests
 *
 * Exercises the Linux POSIX platform implementation (platform/linux/platform.c).
 * These tests verify the basic contract of the platform interface without
 * requiring a loaded FUSE module or WAMR context.
 *
 * Label: platform
 */

#include "gtest/gtest.h"

extern "C" {
#include "platform/platform.h"
}

class PlatformLinuxTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        fuse_platform_init();
    }
};

/* fuse_platform_get_timestamp_us() must return a value greater than zero on
 * any system that has been running for at least one microsecond. */
TEST_F(PlatformLinuxTest, TimestampIsPositive)
{
    uint64_t ts = fuse_platform_get_timestamp_us();
    EXPECT_GT(ts, 0u);
}

/* Calling get_timestamp_us() twice with a sleep in between must yield a
 * strictly increasing sequence, and the delta must be plausible. */
TEST_F(PlatformLinuxTest, TimestampIsMonotonic)
{
    uint64_t t1 = fuse_platform_get_timestamp_us();
    fuse_platform_sleep_us(10000u); /* 10 ms */
    uint64_t t2 = fuse_platform_get_timestamp_us();
    EXPECT_GT(t2, t1);
    EXPECT_GE(t2 - t1, 5000u);    /* at least 5 ms elapsed */
    EXPECT_LE(t2 - t1, 100000u);  /* sanity: no more than 100 ms */
}

/* A 10 ms sleep must advance the timestamp by at least 8 ms to account for
 * scheduler jitter on a loaded CI host. */
TEST_F(PlatformLinuxTest, SleepUs)
{
    uint64_t t1 = fuse_platform_get_timestamp_us();
    fuse_platform_sleep_us(10000u); /* 10 ms */
    uint64_t t2 = fuse_platform_get_timestamp_us();
    EXPECT_GE(t2 - t1, 8000u);
}

/* Arm a long-duration quota (500 ms) and cancel it immediately.  The timer
 * must not fire during the test; neither call must crash. */
TEST_F(PlatformLinuxTest, QuotaArmCancelNocrash)
{
    fuse_platform_quota_arm(0u, 500000u);  /* 500 ms — will not fire */
    fuse_platform_quota_cancel(0u);
}

/* Cancelling when no timer is armed must not crash. */
TEST_F(PlatformLinuxTest, QuotaCancelWithoutArmNocrash)
{
    fuse_platform_quota_cancel(0u);
}

/* A second call to fuse_platform_init() must not crash or corrupt state. */
TEST_F(PlatformLinuxTest, DoubleInitNocrash)
{
    fuse_platform_init();
}

/* Arming twice (re-arm before the first timer fires) must not crash. */
TEST_F(PlatformLinuxTest, DoubleArmNocrash)
{
    fuse_platform_quota_arm(0u, 500000u);
    fuse_platform_quota_arm(0u, 500000u); /* re-arm: replaces the previous timer */
    fuse_platform_quota_cancel(0u);
}
