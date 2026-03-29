/*
 * tests/platform/freertos/test_platform_freertos.cpp
 *   — FreeRTOS POSIX simulator platform integration tests
 *
 * All FreeRTOS API calls (xTaskCreate, vTaskDelay, …) require the scheduler
 * to be running.  The custom main() below starts the scheduler with a task
 * that calls RUN_ALL_TESTS() so the full GoogleTest suite executes inside the
 * FreeRTOS context.
 *
 * The PlatformFreeRTOS test suite validates the platform interface contract
 * (monotonic timestamps, quota arm/cancel safety) using a secondary task so
 * that vTaskDelay() is available for the sleep assertions.
 *
 * Label: platform
 */

#include "gtest/gtest.h"

#include "FreeRTOS.h"
#include "task.h"

extern "C" {
#include "platform/platform.h"
}

/* ---------------------------------------------------------------------------
 * Test results communicated from the worker task to the assertion task.
 * Using plain int flags rather than FreeRTOS primitives keeps the test
 * harness self-contained and avoids scheduler-ordering subtleties.
 * --------------------------------------------------------------------------- */
static volatile int g_test_result = 0; /* accumulates error count */
static volatile int g_tests_done  = 0; /* set to 1 when worker finishes */

static void run_tests_task(void *param)
{
    (void)param;
    fuse_platform_init();

    /* --- Timestamp is positive --- */
    uint64_t ts = fuse_platform_get_timestamp_us();
    if (ts == 0u) {
        ++g_test_result;
    }

    /* --- Timestamp is monotonic across a sleep --- */
    uint64_t t1 = fuse_platform_get_timestamp_us();
    fuse_platform_sleep_us(20000u); /* 20 ms */
    uint64_t t2 = fuse_platform_get_timestamp_us();
    if (t2 <= t1) {
        ++g_test_result;
    }

    /* --- Quota arm then cancel — must not crash --- */
    fuse_platform_quota_arm(0u, 500000u);
    fuse_platform_quota_cancel(0u);

    /* --- Double cancel — must not crash --- */
    fuse_platform_quota_cancel(0u);

    /* --- Re-arm (arm twice without cancel) — must not crash --- */
    fuse_platform_quota_arm(0u, 500000u);
    fuse_platform_quota_arm(0u, 500000u);
    fuse_platform_quota_cancel(0u);

    g_tests_done = 1;
    vTaskDelete(NULL);
}

TEST(PlatformFreeRTOS, AllChecks)
{
    g_test_result = 0;
    g_tests_done  = 0;

    xTaskCreate(run_tests_task, "fuse_chk", 4096, NULL, 2, NULL);

    /* Poll until the worker task finishes, with a 500 ms guard timeout. */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500u);
    while (!g_tests_done && (xTaskGetTickCount() < deadline)) {
        vTaskDelay(pdMS_TO_TICKS(10u));
    }

    EXPECT_EQ(g_tests_done, 1);
    EXPECT_EQ(g_test_result, 0);
}

/* ---------------------------------------------------------------------------
 * Custom main: initialise GoogleTest then hand control to the FreeRTOS
 * scheduler.  RUN_ALL_TESTS() is called from within a scheduler task so that
 * FreeRTOS APIs (vTaskDelay, xTimerCreate, …) are available to test code.
 * --------------------------------------------------------------------------- */
static int g_gtest_result = 0;

static void gtest_task(void *param)
{
    (void)param;
    g_gtest_result = RUN_ALL_TESTS();
    /* Brief delay to allow any pending timer callbacks to drain before exit. */
    vTaskDelay(pdMS_TO_TICKS(100u));
    exit(g_gtest_result);
}

int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    xTaskCreate(gtest_task, "gtest_main", 65536, NULL, 1, NULL);
    vTaskStartScheduler();
    return 1; /* unreachable */
}
