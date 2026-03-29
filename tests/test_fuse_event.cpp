/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_event.cpp — Tests for fuse_post_event(), fuse_clear_event(), and
 *                       the FUSE_ACTIVATION_* event-driven scheduling system.
 *
 * Covers:
 *  Suite 1 (EventApiTest / EventApiNotInit):
 *   - fuse_post_event() / fuse_clear_event() before init → ERR_NOT_INITIALIZED
 *   - Invalid event_id (>= 32) → ERR_INVALID_ARG
 *   - Post event with no subscribers → FUSE_SUCCESS
 *   - Post event sets event_latch for subscribed module (LOADED state)
 *   - Post event does NOT set latch for unsubscribed module
 *   - fuse_clear_event() clears latch bit for subscribed module
 *   - fuse_clear_event() clears only the target bit, not others
 *
 *  Suite 2 (ActivationModeTest):
 *   - FUSE_ACTIVATION_MANUAL: fuse_tick() skips module; direct run_step works
 *   - FUSE_ACTIVATION_INTERVAL: fuse_tick() drives module by time
 *   - activation_mask == 0 defaults to INTERVAL (backward compatibility)
 *   - FUSE_ACTIVATION_EVENT: tick skips without event; runs when event posted
 *   - After event-triggered step, latch is cleared (edge-trigger semantics)
 *   - Second tick without new post → module does NOT run again
 *   - Second post after first tick → triggers again
 *   - Combined INTERVAL | EVENT: event path takes priority over interval
 *   - Broadcast: all subscribed modules run when event posted
 *   - Non-subscribed module does NOT run when unrelated event posted
 */

#include "fuse_test_helper.h"

/* =========================================================================
 * Local helper: build a policy with activation_mask and event_subscribe set.
 *
 * MakePolicy() in fuse_test_helper.h does not expose the new fields added in
 * fuse_types.h, so we extend it here without modifying the shared header.
 * ====================================================================== */
static fuse_policy_t MakeActivationPolicy(uint32_t caps,
                                          uint32_t pages,
                                          uint32_t stack,
                                          uint32_t heap,
                                          uint32_t quota,
                                          uint32_t interval,
                                          uint32_t activation_mask,
                                          uint32_t event_subscribe)
{
    fuse_policy_t p = MakePolicy(caps, pages, stack, heap, quota, interval);
    p.activation_mask = activation_mask;
    p.event_subscribe = event_subscribe;
    return p;
}

/* Capability set used by all event tests — grant everything available. */
static const uint32_t kAllCaps =
    FUSE_CAP_TEMP_SENSOR | FUSE_CAP_TIMER | FUSE_CAP_CAMERA | FUSE_CAP_LOG;

/* =========================================================================
 * Suite 3 (declared first): EventApiNotInit
 *
 * Raw ::testing::Test fixture — does NOT call fuse_init().  Used to verify
 * pre-init error returns.  Mirrors the TickNotInitialized pattern from
 * test_fuse_scheduling.cpp.
 * ====================================================================== */
class EventApiNotInit : public ::testing::Test {
protected:
    void SetUp() override {
        /* Tear down any stale WAMR state from a previous test. */
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx,        0, sizeof(g_ctx));
        std::memset(g_module_mem,  0, sizeof(g_module_mem));
        std::memset(g_log_mem,     0, sizeof(g_log_mem));
    }

    void TearDown() override {
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
};

/* -------------------------------------------------------------------------
 * Test 1: fuse_post_event() before fuse_init() returns ERR_NOT_INITIALIZED.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiNotInit, PostEvent_NotInitialized_ReturnsError)
{
    EXPECT_EQ(fuse_post_event(0u), FUSE_ERR_NOT_INITIALIZED);
}

/* -------------------------------------------------------------------------
 * Test 2: fuse_clear_event() before fuse_init() returns ERR_NOT_INITIALIZED.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiNotInit, ClearEvent_NotInitialized_ReturnsError)
{
    EXPECT_EQ(fuse_clear_event(0u), FUSE_ERR_NOT_INITIALIZED);
}

/* =========================================================================
 * Suite 1: EventApiTest
 *
 * Uses FuseTestBase — fuse_init() is called in SetUp(), fuse_stop() +
 * wasm_runtime_destroy() in TearDown().  Tests here do not require a WASM
 * module unless explicitly noted.
 * ====================================================================== */
class EventApiTest : public FuseTestBase {};

/* -------------------------------------------------------------------------
 * Test 3: fuse_post_event() with event_id >= 32 returns ERR_INVALID_ARG.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, PostEvent_InvalidId_ReturnsError)
{
    /* Boundary: exactly 32 is the first invalid value. */
    EXPECT_EQ(fuse_post_event(32u), FUSE_ERR_INVALID_ARG);

    /* Extreme: UINT32_MAX */
    EXPECT_EQ(fuse_post_event(UINT32_MAX), FUSE_ERR_INVALID_ARG);
}

/* -------------------------------------------------------------------------
 * Test 4: fuse_clear_event() with event_id >= 32 returns ERR_INVALID_ARG.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, ClearEvent_InvalidId_ReturnsError)
{
    EXPECT_EQ(fuse_clear_event(32u), FUSE_ERR_INVALID_ARG);
    EXPECT_EQ(fuse_clear_event(UINT32_MAX), FUSE_ERR_INVALID_ARG);
}

/* -------------------------------------------------------------------------
 * Test 5: fuse_post_event() with no modules loaded returns FUSE_SUCCESS.
 *
 * No modules are in_use so the broadcast loop is a no-op.  The API must
 * still return SUCCESS rather than propagating an internal "nothing to do"
 * condition as an error.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, PostEvent_NoSubscribers_Succeeds)
{
    EXPECT_EQ(fuse_post_event(7u), FUSE_SUCCESS);
    EXPECT_EQ(fuse_post_event(0u), FUSE_SUCCESS);
    EXPECT_EQ(fuse_post_event(31u), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 6: fuse_post_event() sets the event_latch for a subscribed module
 *         even when the module is in LOADED state (not yet RUNNING).
 *
 * fuse_post_event checks in_use only, not module state.  A module that is
 * LOADED and subscribes to event 3 must have bit 3 set in its event_latch
 * after the post.
 *
 * Requires AOT: the module slot is populated via fuse_module_load().
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, PostEvent_SetsEventLatchForSubscribedModule)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 3u));

    /* Suppress default ON_CALL timer noise — not needed for this test. */
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    /* Module is in LOADED state — post must still set the latch. */
    ASSERT_EQ(fuse_post_event(3u), FUSE_SUCCESS);

    uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                          memory_order_relaxed);
    EXPECT_NE(latch & (1u << 3u), 0u)
        << "event_latch bit 3 should be set after posting event 3";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 7: fuse_post_event() does NOT set latch bits for modules that do not
 *         subscribe to the posted event.
 *
 * Module subscribes to event 5 only.  Posting event 3 must leave the latch
 * completely clear (neither bit 3 nor bit 5 should be set).
 *
 * Requires AOT.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, PostEvent_DoesNotSetLatchForUnsubscribedModule)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 5u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    /* Post an event the module does NOT subscribe to. */
    ASSERT_EQ(fuse_post_event(3u), FUSE_SUCCESS);

    uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                          memory_order_relaxed);
    EXPECT_EQ(latch & (1u << 3u), 0u)
        << "event_latch bit 3 must NOT be set (module subscribes to event 5 only)";
    EXPECT_EQ(latch & (1u << 5u), 0u)
        << "event_latch bit 5 must NOT be set (event 5 was not posted)";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 8: fuse_clear_event() clears the event_latch bit for a subscribed
 *         module after it was set by fuse_post_event().
 *
 * Requires AOT.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, ClearEvent_ClearsLatchForSubscribedModule)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 2u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    /* Set the latch. */
    ASSERT_EQ(fuse_post_event(2u), FUSE_SUCCESS);
    {
        uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                              memory_order_relaxed);
        ASSERT_NE(latch & (1u << 2u), 0u)
            << "Precondition: event_latch bit 2 must be set before clear";
    }

    /* Clear it. */
    ASSERT_EQ(fuse_clear_event(2u), FUSE_SUCCESS);

    uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                          memory_order_relaxed);
    EXPECT_EQ(latch & (1u << 2u), 0u)
        << "event_latch bit 2 must be clear after fuse_clear_event(2)";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 9: fuse_clear_event() clears only the target bit, leaving other
 *         pending event bits intact.
 *
 * Module subscribes to events 1 and 2.  Post both.  Clear event 1.
 * Bit 1 must be cleared; bit 2 must remain set.
 *
 * Requires AOT.
 * ---------------------------------------------------------------------- */
TEST_F(EventApiTest, ClearEvent_OnlyClears_TargetBit)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 1u) | (1u << 2u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_post_event(1u), FUSE_SUCCESS);
    ASSERT_EQ(fuse_post_event(2u), FUSE_SUCCESS);

    {
        uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                              memory_order_relaxed);
        ASSERT_NE(latch & (1u << 1u), 0u)
            << "Precondition: bit 1 must be set";
        ASSERT_NE(latch & (1u << 2u), 0u)
            << "Precondition: bit 2 must be set";
    }

    ASSERT_EQ(fuse_clear_event(1u), FUSE_SUCCESS);

    uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                          memory_order_relaxed);
    EXPECT_EQ(latch & (1u << 1u), 0u)
        << "event_latch bit 1 must be cleared";
    EXPECT_NE(latch & (1u << 2u), 0u)
        << "event_latch bit 2 must remain set (was not cleared)";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* =========================================================================
 * Suite 2: ActivationModeTest
 *
 * All tests require AOT binaries.  Use LoadAotOrSkip and return early if the
 * binary is unavailable.
 * ====================================================================== */
class ActivationModeTest : public FuseTestBase {};

/* -------------------------------------------------------------------------
 * Test 10: FUSE_ACTIVATION_MANUAL — fuse_tick() skips the module entirely.
 *
 * A RUNNING MANUAL module must never be stepped by fuse_tick().
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, ManualActivation_TickDoesNotRunModule)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_MANUAL, /*event_subscribe=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* Tick must skip MANUAL module — return mask should be 0. */
    uint32_t mask = fuse_tick();
    EXPECT_EQ(mask, 0u) << "fuse_tick() must not run MANUAL module";

    /* Module must remain RUNNING (not trapped/paused by tick side-effect). */
    fuse_module_state_t state;
    ASSERT_EQ(fuse_module_stat(mid, &state), FUSE_SUCCESS);
    EXPECT_EQ(state, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 11: FUSE_ACTIVATION_INTERVAL — fuse_tick() runs the module when the
 *          interval constraint is satisfied.
 *
 * With step_interval_us == 0 and any timer value the module always runs.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, IntervalActivation_TickRunsModule)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_INTERVAL, /*event_subscribe=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();
    EXPECT_NE(mask & (1u << mid), 0u)
        << "INTERVAL module bit " << mid << " must be set in tick mask";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 12: activation_mask == 0 defaults to INTERVAL behaviour.
 *
 * Legacy modules that pre-date the activation_mask field have a zero mask.
 * fuse_tick() must treat them identically to FUSE_ACTIVATION_INTERVAL.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, LegacyZeroActivationMask_TickRunsAsInterval)
{
    /* Build policy via MakePolicy so activation_mask is zero ({}  init). */
    const fuse_policy_t policy = MakePolicy(kAllCaps, 1u, 8192u, 8192u,
                                            /*quota=*/0u, /*interval=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();
    EXPECT_NE(mask & (1u << mid), 0u)
        << "Legacy zero activation_mask should run like INTERVAL";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 13: FUSE_ACTIVATION_EVENT — fuse_tick() does NOT run the module when
 *          no event has been posted.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_TickDoesNotRunWithoutEvent)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 7u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* No event posted — tick must return 0 for this module. */
    uint32_t mask = fuse_tick();
    EXPECT_EQ(mask, 0u)
        << "EVENT module must not run without a pending event";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 14: FUSE_ACTIVATION_EVENT — fuse_tick() runs the module when the
 *          subscribed event has been posted.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_TickRunsWhenEventPosted)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 7u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    ASSERT_EQ(fuse_post_event(7u), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();
    EXPECT_NE(mask & (1u << mid), 0u)
        << "EVENT module must run after subscribed event is posted";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 15: After an event-triggered step, the triggering latch bit is cleared.
 *
 * fuse_tick() clears only the bits it acted on, so that events posted during
 * the step are not lost.  This test verifies that event 4's bit is zero after
 * the step completes.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_LatchClearedAfterStep)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 4u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);
    ASSERT_EQ(fuse_post_event(4u), FUSE_SUCCESS);

    /* Confirm latch set before tick. */
    {
        uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                              memory_order_relaxed);
        ASSERT_NE(latch & (1u << 4u), 0u)
            << "Precondition: latch bit 4 must be set before tick";
    }

    uint32_t mask = fuse_tick();
    ASSERT_NE(mask & (1u << mid), 0u) << "Module must have run this tick";

    /* Latch must be cleared now. */
    uint32_t latch = atomic_load_explicit(&g_ctx.modules[mid].event_latch,
                                          memory_order_relaxed);
    EXPECT_EQ(latch & (1u << 4u), 0u)
        << "event_latch bit 4 must be cleared after event-triggered step";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 16: Edge-trigger semantics — one post triggers exactly one step.
 *
 * After tick 1 consumes the event and clears the latch, tick 2 (with no new
 * post) must return 0.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_EdgeTrigger_SecondTickDoesNotRunWithoutNewPost)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 4u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);
    ASSERT_EQ(fuse_post_event(4u), FUSE_SUCCESS);

    /* Tick 1: event pending → module runs. */
    uint32_t mask1 = fuse_tick();
    EXPECT_NE(mask1 & (1u << mid), 0u)
        << "Tick 1 must run module (event pending)";

    /* Tick 2: no new event → module must NOT run. */
    uint32_t mask2 = fuse_tick();
    EXPECT_EQ(mask2, 0u)
        << "Tick 2 must return 0 — no new event was posted (edge-trigger)";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 17: A second post after the first tick triggers the module again.
 *
 * Confirms that the event system is re-armable: posting again after the
 * latch was cleared by tick 1 causes tick 2 to run the module.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_SecondPostAfterFirstTick_TriggersAgain)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 4u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* First post + tick. */
    ASSERT_EQ(fuse_post_event(4u), FUSE_SUCCESS);
    uint32_t mask1 = fuse_tick();
    EXPECT_NE(mask1 & (1u << mid), 0u) << "Tick 1 must run module";

    /* Re-arm with a second post. */
    ASSERT_EQ(fuse_post_event(4u), FUSE_SUCCESS);
    uint32_t mask2 = fuse_tick();
    EXPECT_NE(mask2 & (1u << mid), 0u)
        << "Tick 2 must run module after second post";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 18: Combined INTERVAL | EVENT — event path takes priority over interval.
 *
 * Policy: step_interval_us = 1 000 000 µs (1 s), timer always returns 0.
 *
 * Tick 1 (step_ever_run=false → first-step sentinel bypasses interval): runs
 *         via INTERVAL path (no event pending).
 *         After step: last_step_at_us = 0, step_ever_run = true.
 *
 * Tick 2 (no event, interval: 0 - 0 = 0 < 1 000 000 → not elapsed): 0.
 *
 * Post event 1.
 * Tick 3 (event pending → event path with bypass_interval=true): runs.
 *         last_step_at_us = 0 (timer still 0).
 *
 * Tick 4 (no event, interval: 0 - 0 = 0 < 1 000 000 → not elapsed): 0.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, EventActivation_CombinedWithInterval_EventTakesPriority)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/1000000u,
        FUSE_ACTIVATION_EVENT | FUSE_ACTIVATION_INTERVAL,
        /*event_subscribe=*/(1u << 1u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* Tick 1: first-step sentinel fires via INTERVAL (step_ever_run == false). */
    uint32_t mask1 = fuse_tick();
    EXPECT_NE(mask1 & (1u << mid), 0u)
        << "Tick 1: first step must run via INTERVAL (step_ever_run sentinel)";

    /* Tick 2: interval not elapsed, no event → 0. */
    uint32_t mask2 = fuse_tick();
    EXPECT_EQ(mask2, 0u)
        << "Tick 2: interval not elapsed and no event → must return 0";

    /* Post event 1 — event path bypasses interval check. */
    ASSERT_EQ(fuse_post_event(1u), FUSE_SUCCESS);

    /* Tick 3: event pending → module runs despite interval not elapsed. */
    uint32_t mask3 = fuse_tick();
    EXPECT_NE(mask3 & (1u << mid), 0u)
        << "Tick 3: event must trigger run even though interval has not elapsed";

    /* Tick 4: latch cleared by tick 3, interval still not elapsed → 0. */
    uint32_t mask4 = fuse_tick();
    EXPECT_EQ(mask4, 0u)
        << "Tick 4: no new event and interval still not elapsed → must return 0";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 19: FUSE_ACTIVATION_MANUAL — direct fuse_module_run_step() still works.
 *
 * fuse_tick() skips MANUAL modules, but the host may drive them directly
 * via fuse_module_run_step().  That direct call must succeed.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, ManualActivation_DirectRunStepWorks)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_MANUAL, /*event_subscribe=*/0u);

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_id_t mid = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), policy, &mid)) return;

    ASSERT_EQ(fuse_module_start(mid), FUSE_SUCCESS);

    /* Tick must skip the module. */
    EXPECT_EQ(fuse_tick(), 0u) << "fuse_tick() must not run MANUAL module";

    /* But direct invocation must succeed. */
    EXPECT_EQ(fuse_module_run_step(mid), FUSE_SUCCESS)
        << "fuse_module_run_step() must succeed for MANUAL module";

    EXPECT_EQ(fuse_module_unload(mid), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 20: Broadcast semantics — all subscribed modules receive the event.
 *
 * Load two EVENT-activated modules, both subscribing to event 0.  Post event 0.
 * fuse_tick() must return a mask with BOTH module bits set.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, MultipleModules_BroadcastEvent_BothRun)
{
    const fuse_policy_t policy = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 0u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        testing::internal::AssertHelper(
            testing::TestPartResult::kSkip,
            __FILE__, __LINE__,
            "AOT binary not found: mod_step_only.aot -- build wamrc first")
            = testing::Message();
        return;
    }

    fuse_module_id_t mid1 = FUSE_INVALID_MODULE_ID;
    fuse_module_id_t mid2 = FUSE_INVALID_MODULE_ID;

    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid1),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy, &mid2),
              FUSE_SUCCESS);

    ASSERT_NE(mid1, mid2) << "Two separate module IDs must be assigned";

    ASSERT_EQ(fuse_module_start(mid1), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid2), FUSE_SUCCESS);

    ASSERT_EQ(fuse_post_event(0u), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();

    EXPECT_NE(mask & (1u << mid1), 0u)
        << "Module " << mid1 << " must have run (subscribed to event 0)";
    EXPECT_NE(mask & (1u << mid2), 0u)
        << "Module " << mid2 << " must have run (subscribed to event 0)";

    EXPECT_EQ(fuse_module_unload(mid1), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(mid2), FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * Test 21: Only the module subscribed to the posted event runs.
 *
 * Module A subscribes to event 0.  Module B subscribes to event 1.
 * Post only event 0.  fuse_tick() must set module A's bit only.
 * Module B must not run.
 * ---------------------------------------------------------------------- */
TEST_F(ActivationModeTest, MultipleModules_OnlySubscribedModuleRuns)
{
    const fuse_policy_t policy_a = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 0u));

    const fuse_policy_t policy_b = MakeActivationPolicy(
        kAllCaps, 1u, 8192u, 8192u, /*quota=*/0u, /*interval=*/0u,
        FUSE_ACTIVATION_EVENT, /*event_subscribe=*/(1u << 1u));

    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        testing::internal::AssertHelper(
            testing::TestPartResult::kSkip,
            __FILE__, __LINE__,
            "AOT binary not found: mod_step_only.aot -- build wamrc first")
            = testing::Message();
        return;
    }

    fuse_module_id_t mid_a = FUSE_INVALID_MODULE_ID;
    fuse_module_id_t mid_b = FUSE_INVALID_MODULE_ID;

    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy_a, &mid_a),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &policy_b, &mid_b),
              FUSE_SUCCESS);

    ASSERT_EQ(fuse_module_start(mid_a), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(mid_b), FUSE_SUCCESS);

    /* Post only event 0 — module B (event 1) must not trigger. */
    ASSERT_EQ(fuse_post_event(0u), FUSE_SUCCESS);

    uint32_t mask = fuse_tick();

    EXPECT_NE(mask & (1u << mid_a), 0u)
        << "Module A (subscribes event 0) must have run";
    EXPECT_EQ(mask & (1u << mid_b), 0u)
        << "Module B (subscribes event 1) must NOT run when only event 0 is posted";

    EXPECT_EQ(fuse_module_unload(mid_a), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(mid_b), FUSE_SUCCESS);
}
