/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_scheduling.cpp — Tests for step_interval_us enforcement in
 *                            fuse_module_run_step() and for fuse_tick().
 *
 * Covers:
 *  - step_interval_us == 0: no constraint, both consecutive steps succeed.
 *  - step_interval_us > 0 with no timer: check skipped, both steps succeed.
 *  - First step always runs (step_ever_run == false sentinel).
 *  - Second step too early: FUSE_ERR_INTERVAL_NOT_ELAPSED.
 *  - Second step at exactly the interval boundary: succeeds.
 *  - Second step past the interval: succeeds.
 *  - Rejected step does not update last_step_at_us.
 *  - fuse_tick() returns 0 when not initialized.
 *  - fuse_tick() returns 0 when runtime is stopped.
 *  - fuse_tick() returns 0 with no running modules.
 *  - fuse_tick() returns bit-mask for a due module.
 *  - fuse_tick() returns 0 for a module not yet due.
 *  - fuse_tick() skips a paused module.
 *  - fuse_tick() returns combined mask for multiple due modules.
 */

#include "fuse_test_helper.h"

/* -------------------------------------------------------------------------
 * Helper: build a policy with step_interval_us set explicitly.
 * MakePolicy() in fuse_test_helper.h does not expose the step_interval_us
 * parameter, so we extend it here.
 * ---------------------------------------------------------------------- */
static fuse_policy_t MakePolicyInterval(uint32_t caps,
                                        uint32_t pages,
                                        uint32_t stack,
                                        uint32_t heap,
                                        uint32_t quota,
                                        uint32_t interval_us)
{
    fuse_policy_t p = MakePolicy(caps, pages, stack, heap, quota);
    p.step_interval_us = interval_us;
    return p;
}

/* Convenient capability set used by all scheduling tests. */
static const uint32_t kAllCaps =
    FUSE_CAP_TEMP_SENSOR | FUSE_CAP_TIMER | FUSE_CAP_CAMERA | FUSE_CAP_LOG;

/* -------------------------------------------------------------------------
 * NullHalFixture — fixture that initialises FUSE with MakeHalNull().
 *
 * Used for StepInterval_NoTimerSkipsCheck where the absence of a timer
 * callback is the condition under test.  WAMR is initialised exactly once
 * per test here (no mid-test reinit), so wasm_runtime_destroy() is called
 * exactly once in TearDown().
 * ---------------------------------------------------------------------- */
class NullHalFixture : public ::testing::Test {
protected:
    MockHal mock_hal_;

    void SetUp() override {
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));

        /* Install mock so static thunks resolve, but supply a null HAL so
         * none of the function pointers are registered with FUSE. */
        MockHal::Install(&mock_hal_);

        fuse_hal_t hal = MockHal::MakeHalNull();
        fuse_stat_t rc = fuse_init(g_module_mem, kModuleMemSize,
                                   g_log_mem, kLogMemSize, &hal);
        ASSERT_EQ(rc, FUSE_SUCCESS) << "fuse_init failed in NullHalFixture";
    }

    void TearDown() override {
        (void)fuse_stop();
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
        MockHal::Uninstall();
    }

    /* Parallel to FuseTestBase::LoadAotOrSkip — loads an AOT binary or
     * marks the test as skipped when the file is not yet available. */
    bool LoadAotOrSkip(const std::string &aot_path,
                       const fuse_policy_t &policy,
                       fuse_module_id_t *out_id)
    {
        AotBinary bin(aot_path);
        if (!bin.IsAvailable()) {
            testing::internal::AssertHelper(
                testing::TestPartResult::kSkip,
                __FILE__, __LINE__,
                (std::string("AOT binary not found: ") + aot_path +
                 " -- build wamrc first").c_str())
                = testing::Message();
            return false;
        }
        fuse_stat_t rc = fuse_module_load(bin.Data(), bin.Size(),
                                          &policy, out_id);
        EXPECT_EQ(rc, FUSE_SUCCESS) << "fuse_module_load failed for " << aot_path;
        return (rc == FUSE_SUCCESS);
    }
};

/* =========================================================================
 * Suite: SchedulingTest
 * All timing / scheduling tests that use the full HAL mock (timer wired).
 * ====================================================================== */
class SchedulingTest : public FuseTestBase {};

/* -------------------------------------------------------------------------
 * Test 1: step_interval_us == 0 — no constraint, both consecutive steps succeed.
 *
 * With interval == 0 the guard is entirely skipped regardless of the timer
 * value.  The timer is still called (to capture step_start_us for logging),
 * but its value has no effect on the scheduling decision.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_ZeroMeansNoConstraint)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 2: step_interval_us > 0 but timer_get_timestamp == NULL.
 *
 * When no timer is installed, the interval check is skipped entirely.
 * Both consecutive steps must succeed despite the 1 s interval.
 *
 * Uses NullHalFixture so that hal.timer_get_timestamp is NULL throughout.
 * ---------------------------------------------------------------------- */
TEST_F(NullHalFixture, StepInterval_NoTimerSkipsCheck)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u);

    /* EXPECT_CALL not set — timer must never be called (no pointer wired). */

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* Without a timer, the interval guard is skipped → both calls succeed. */
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 3: First step always runs regardless of interval because
 * step_ever_run == false (module has never stepped).
 *
 * Timer returns 500 µs, which is less than the 1 000 000 µs interval.
 * The first call must still succeed because the "never stepped" sentinel
 * (step_ever_run == false) bypasses the elapsed-time comparison entirely.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_FirstStepAlwaysRuns)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(500u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* First step: step_ever_run == false → interval guard bypassed → SUCCESS. */
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 4: Second step called too early returns FUSE_ERR_INTERVAL_NOT_ELAPSED.
 *
 * Timer sequence (all calls including fuse_log_write timestamps):
 *   fuse_module_load log  → 0      (log timestamp only)
 *   fuse_module_start log → 0      (log timestamp only)
 *   step 1 start          → 1000   last_step_at_us = 1000
 *   step 2 check          → 1500   1500 - 1000 = 500 < 1 000 000 → rejected
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_TooEarlyReturnsError)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u);

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    ::testing::InSequence seq;
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillOnce(::testing::Return(0u))      /* fuse_module_load log write  */
        .WillOnce(::testing::Return(0u))      /* fuse_module_start log write */
        .WillOnce(::testing::Return(1000u))   /* step 1 start; last_step_at_us = 1000 */
        .WillOnce(::testing::Return(1500u))   /* step 2 check: 500 < 1000000 → rejected */
        .WillRepeatedly(::testing::Return(1500u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_ERR_INTERVAL_NOT_ELAPSED);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 5: Second step called exactly at the interval boundary succeeds.
 *
 * Implementation: (step_start_us - last_step_at_us) < step_interval_us
 * uses strict less-than, so equality means the step is allowed.
 *
 * Note on t=0: step_start_us is stored unconditionally on success.  Using
 * t=0 for step 1 leaves last_step_at_us = 0 and step_ever_run = true.
 * Step 2 would then check (t2 - 0) < interval, which at t2 = 1000 gives
 * (1000 < 1000) → false → allowed.  We use t=1 to keep the arithmetic
 * clearer and avoid ambiguity with the initial zero state.
 *
 * Timer sequence (all calls including fuse_log_write timestamps):
 *   fuse_module_load log  → 0       (log timestamp only)
 *   fuse_module_start log → 0       (log timestamp only)
 *   step 1 start          → 1       last_step_at_us = 1, step_ever_run = true
 *   step 2 check          → 1001    1001 - 1 = 1000 == 1000 → allowed
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_ExactlyAtIntervalRuns)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000u);

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    ::testing::InSequence seq;
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillOnce(::testing::Return(0u))      /* fuse_module_load log write  */
        .WillOnce(::testing::Return(0u))      /* fuse_module_start log write */
        .WillOnce(::testing::Return(1u))      /* step 1 start; last_step_at_us = 1 */
        .WillOnce(::testing::Return(1001u))   /* step 2 check: 1001-1=1000 ≥ 1000 → allowed */
        .WillRepeatedly(::testing::Return(1001u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 6: Second step called well past the interval succeeds.
 *
 * Timer sequence (all calls including fuse_log_write timestamps):
 *   fuse_module_load log  → 0       (log timestamp only)
 *   fuse_module_start log → 0       (log timestamp only)
 *   step 1 start          → 1       last_step_at_us = 1
 *   step 2 check          → 2001    2001 - 1 = 2000 > 1000 → allowed
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_AfterIntervalElapsedRuns)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000u);

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    ::testing::InSequence seq;
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillOnce(::testing::Return(0u))      /* fuse_module_load log write  */
        .WillOnce(::testing::Return(0u))      /* fuse_module_start log write */
        .WillOnce(::testing::Return(1u))      /* step 1 start; last_step_at_us = 1 */
        .WillOnce(::testing::Return(2001u))   /* step 2 check: 2001-1=2000 > 1000 → allowed */
        .WillRepeatedly(::testing::Return(2001u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 7: A rejected step must NOT update last_step_at_us.
 *
 * We choose timer values such that step 3 is >= interval from step 1's
 * timestamp but < interval from the rejected step 2's timestamp.  If the
 * rejection incorrectly updated last_step_at_us, step 3 would be rejected.
 *
 * Invariants:
 *   interval = 1 000 000 µs
 *   t1 = 1000      → last_step_at_us after step 1 = 1000
 *   t2 = 1500      → 1500 - 1000 = 500 < 1 000 000 → REJECTED
 *                    last_step_at_us must stay 1000
 *   t3 = 1001100   → 1001100 - 1000 = 1000100 >= 1 000 000 (passes from t1)
 *                  → 1001100 - 1500 = 999600  <  1 000 000 (would fail from t2)
 *
 * Step 3 must succeed, proving last_step_at_us was not updated at step 2.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, StepInterval_NotElapsedDoesNotUpdateTimestamp)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u);

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    ::testing::InSequence seq;
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillOnce(::testing::Return(0u))         /* fuse_module_load log write  */
        .WillOnce(::testing::Return(0u))         /* fuse_module_start log write */
        .WillOnce(::testing::Return(1000u))      /* step 1 start; sets last_step_at_us = 1000 */
        .WillOnce(::testing::Return(1500u))      /* step 2 check → rejected; last_step_at_us unchanged */
        .WillOnce(::testing::Return(1001100u))   /* step 3 check/start → passes from t1 but not t2 */
        .WillRepeatedly(::testing::Return(1001100u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_ERR_INTERVAL_NOT_ELAPSED);
    /* Step 3 passes only if last_step_at_us was not updated by the rejection. */
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* =========================================================================
 * fuse_tick() — "not initialized" test.
 *
 * Must NOT call fuse_init() so g_ctx.initialized remains false.
 * Mirrors the pattern used in ModuleLoadBeforeInit from test_fuse_module.cpp.
 * ====================================================================== */
class TickNotInitialized : public ::testing::Test {
protected:
    void SetUp() override {
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));
    }

    void TearDown() override {
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
};

/* -------------------------------------------------------------------------
 * Test 8: fuse_tick() returns 0 when FUSE has not been initialized.
 * ---------------------------------------------------------------------- */
TEST_F(TickNotInitialized, FuseTick_NotInitializedReturnsZero)
{
    EXPECT_EQ(fuse_tick(), 0u);
}

/* -------------------------------------------------------------------------
 * Test 9: fuse_tick() returns 0 when the runtime has been stopped.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_RuntimeStoppedReturnsZero)
{
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    ASSERT_EQ(fuse_stop(), FUSE_SUCCESS);
    EXPECT_EQ(fuse_tick(), 0u);

    /* Restart so FuseTestBase::TearDown()'s fuse_stop() does not error. */
    EXPECT_EQ(fuse_restart(), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 10: fuse_tick() returns 0 when no modules are loaded.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_NoRunningModulesReturnsZero)
{
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    EXPECT_EQ(fuse_tick(), 0u);
}

/* -------------------------------------------------------------------------
 * Test 11: fuse_tick() returns the bitmask bit for the module that just ran.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_RunsDueModuleReturnsMask)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();
    EXPECT_NE(mask & (1u << mid), 0u)
        << "Expected bit " << mid << " set in returned mask " << mask;

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 12: fuse_tick() returns 0 for a module whose interval has not elapsed.
 *
 * Timer sequence:
 *   tick 1: step_start_us = 1   → first step (step_ever_run = false), runs;
 *                                  last_step_at_us set to 1, step_ever_run = true
 *   tick 2: step_start_us = 500 → 500 - 1 = 499 < 1 000 000 → not due → 0
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_SkipsNotDueModule)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u);

    ::testing::InSequence seq;
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillOnce(::testing::Return(1u))
        .WillRepeatedly(::testing::Return(500u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* First tick: step_ever_run == false → first-step sentinel → runs. */
    uint32_t mask1 = fuse_tick();
    EXPECT_NE(mask1 & (1u << mid), 0u)
        << "First tick should have run module " << mid;

    /* Second tick immediately after: interval not elapsed → skipped → 0. */
    uint32_t mask2 = fuse_tick();
    EXPECT_EQ(mask2, 0u) << "Second tick should return 0 (interval not elapsed)";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 13: fuse_tick() skips a module that is in PAUSED state.
 *
 * Load mod_step_only, start it, then pause it.  fuse_tick() must return 0
 * because the module is no longer in RUNNING state.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_SkipsPausedModule)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_pause(mid), FUSE_SUCCESS);

    /* Module is PAUSED — tick must skip it and return 0. */
    EXPECT_EQ(fuse_tick(), 0u);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 14: fuse_tick() returns a combined mask when multiple due modules ran.
 * ---------------------------------------------------------------------- */
TEST_F(SchedulingTest, FuseTick_MultipleDueModulesReturnsCombinedMask)
{
    const fuse_policy_t policy = MakePolicyInterval(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t mid1 = FUSE_INVALID_MODULE_ID;
    fuse_module_id_t mid2 = FUSE_INVALID_MODULE_ID;

    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid1),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid2),
              FUSE_SUCCESS);

    ASSERT_EQ(fuse_module_start(mid1), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid2), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();

    EXPECT_NE(mask & (1u << mid1), 0u)
        << "Expected bit " << mid1 << " set in mask " << mask;
    EXPECT_NE(mask & (1u << mid2), 0u)
        << "Expected bit " << mid2 << " set in mask " << mask;

    EXPECT_EQ(fuse_module_unload(mid1), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(mid2), FUSE_SUCCESS);
}
