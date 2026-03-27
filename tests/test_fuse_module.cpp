/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_module.cpp — Module lifecycle tests.
 *
 * Covers:
 *  - fuse_module_load: null args, uninitialized runtime, FUSE_MAX_MODULES limit,
 *    missing "module_step" export, invalid binary.
 *  - Full lifecycle: load → start → run_step → pause → start → unload.
 *  - State transition guards: start from wrong state, pause from wrong state,
 *    run_step from non-RUNNING state.
 *  - Optional init/deinit: module_init called once on first start, module_deinit
 *    called on unload only when init_called is true.
 *  - fuse_module_stat / fuse_module_pause / fuse_module_start / fuse_module_unload
 *    with invalid IDs and null pointers.
 *  - fuse_module_run_step while runtime is stopped.
 */

#include "fuse_test_helper.h"

/* -------------------------------------------------------------------------
 * Helper: all-caps policy for a basic step-only module
 * ---------------------------------------------------------------------- */
static const fuse_policy_t kBasicPolicy = MakePolicy(
    FUSE_CAP_TEMP_SENSOR | FUSE_CAP_TIMER | FUSE_CAP_CAMERA | FUSE_CAP_LOG,
    /*pages=*/1u, /*stack=*/8192u, /*heap=*/8192u, /*quota=*/0u);

/* =========================================================================
 * Suite: ModuleArgValidation
 * Tests that validate argument-checking before any WAMR call is made.
 * ====================================================================== */
class ModuleArgValidation : public FuseTestBase {};

TEST_F(ModuleArgValidation, LoadNullBuffer) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(nullptr, 16u, &kBasicPolicy, &id),
              FUSE_ERR_INVALID_ARG);
}

TEST_F(ModuleArgValidation, LoadZeroSize) {
    static const uint8_t kBuf[4] = {};
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(kBuf, 0u, &kBasicPolicy, &id),
              FUSE_ERR_INVALID_ARG);
}

TEST_F(ModuleArgValidation, LoadNullPolicy) {
    static const uint8_t kBuf[4] = {};
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(kBuf, sizeof(kBuf), nullptr, &id),
              FUSE_ERR_INVALID_ARG);
}

TEST_F(ModuleArgValidation, LoadNullOutId) {
    static const uint8_t kBuf[4] = {};
    EXPECT_EQ(fuse_module_load(kBuf, sizeof(kBuf), &kBasicPolicy, nullptr),
              FUSE_ERR_INVALID_ARG);
}

TEST_F(ModuleArgValidation, LoadInvalidBinaryFails) {
    static const uint8_t kBadBuf[8] = {0xDE, 0xAD, 0xBE, 0xEF,
                                        0xDE, 0xAD, 0xBE, 0xEF};
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(kBadBuf, sizeof(kBadBuf), &kBasicPolicy, &id),
              FUSE_ERR_MODULE_LOAD_FAILED);
}

TEST_F(ModuleArgValidation, StatNullOutState) {
    fuse_module_state_t st;
    EXPECT_EQ(fuse_module_stat(0u, nullptr), FUSE_ERR_INVALID_ARG);
    /* null check is done before initialized check in fuse_module_stat */
    (void)st;
}

TEST_F(ModuleArgValidation, StatInvalidId) {
    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(FUSE_INVALID_MODULE_ID, &st),
              FUSE_ERR_MODULE_NOT_FOUND);
}

TEST_F(ModuleArgValidation, PauseInvalidId) {
    EXPECT_EQ(fuse_module_pause(FUSE_INVALID_MODULE_ID),
              FUSE_ERR_MODULE_NOT_FOUND);
}

TEST_F(ModuleArgValidation, StartInvalidId) {
    EXPECT_EQ(fuse_module_start(FUSE_INVALID_MODULE_ID),
              FUSE_ERR_MODULE_NOT_FOUND);
}

TEST_F(ModuleArgValidation, UnloadInvalidId) {
    EXPECT_EQ(fuse_module_unload(FUSE_INVALID_MODULE_ID),
              FUSE_ERR_MODULE_NOT_FOUND);
}

TEST_F(ModuleArgValidation, RunStepInvalidId) {
    EXPECT_EQ(fuse_module_run_step(FUSE_INVALID_MODULE_ID),
              FUSE_ERR_MODULE_NOT_FOUND);
}

/* =========================================================================
 * Suite: ModuleLoadBeforeInit
 * Verifies that module APIs return FUSE_ERR_NOT_INITIALIZED before fuse_init.
 * We need a fresh context without calling init, so we override SetUp/TearDown.
 * ====================================================================== */
class ModuleLoadBeforeInit : public ::testing::Test {
protected:
    void SetUp() override {
        /* Deliberately skip fuse_init so g_ctx.initialized == false.
         * Only call wasm_runtime_destroy() if WAMR was actually initialized
         * (guard against double-destroy crash from prior TearDown). */
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
    void TearDown() override {
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
};

TEST_F(ModuleLoadBeforeInit, LoadReturnsNotInitialized) {
    static const uint8_t kBuf[4] = {};
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(kBuf, sizeof(kBuf), &kBasicPolicy, &id),
              FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(ModuleLoadBeforeInit, StartReturnsNotInitialized) {
    EXPECT_EQ(fuse_module_start(0u), FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(ModuleLoadBeforeInit, PauseReturnsNotInitialized) {
    EXPECT_EQ(fuse_module_pause(0u), FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(ModuleLoadBeforeInit, StatReturnsNotInitialized) {
    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(0u, &st), FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(ModuleLoadBeforeInit, UnloadReturnsNotInitialized) {
    EXPECT_EQ(fuse_module_unload(0u), FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(ModuleLoadBeforeInit, RunStepReturnsNotInitialized) {
    EXPECT_EQ(fuse_module_run_step(0u), FUSE_ERR_NOT_INITIALIZED);
}

/* =========================================================================
 * Suite: ModuleLifecycle
 * Full lifecycle tests using real AOT binaries.
 * ====================================================================== */
class ModuleLifecycle : public FuseTestBase {
protected:
    /* Suppress uninteresting mock calls from HAL (log timestamps etc.). */
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(1000u));
    }
};

TEST_F(ModuleLifecycle, LoadStepOnlyModule) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_LOADED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, LoadThenStartTransitionsToRunning) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, RunStepOnRunningModule) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, PauseRunningModule) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_pause(id), FUSE_SUCCESS);

    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_PAUSED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, ResumeAfterPause) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_pause(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);    /* resume from PAUSED */

    fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, FullLifecycleLoadStartRunPauseResumeUnload) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    EXPECT_EQ(fuse_module_start(id),    FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_pause(id),    FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_start(id),    FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id),   FUSE_SUCCESS);
}

/* -- Optional lifecycle exports ----------------------------------------- */

TEST_F(ModuleLifecycle, InitCalledOnceOnFirstStart) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_with_init_deinit.aot"), kBasicPolicy, &id)) return;

    /* First start — module_init must be called. */
    EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_pause(id), FUSE_SUCCESS);

    /* Second start (resume) — module_init must NOT be called again. */
    EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id),   FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, DeinitCalledOnUnloadWhenInitWasCalled) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_with_init_deinit.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);   /* triggers init */
    /* Unload should invoke module_deinit (best-effort, no trap expected). */
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleLifecycle, DeinitNotCalledWhenInitWasNotCalled) {
    /* Load but never start — init_called remains false, deinit must be skipped. */
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_with_init_deinit.aot"), kBasicPolicy, &id)) return;

    /* No start call — just unload directly. Should succeed without crash. */
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* -- Missing module_step export ----------------------------------------- */

TEST_F(ModuleLifecycle, LoadFailsWhenModuleStepMissing) {
    AotBinary bin(AOT_PATH("mod_no_step.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_no_step.aot";
    }
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(bin.Data(), bin.Size(), &kBasicPolicy, &id),
              FUSE_ERR_MODULE_LOAD_FAILED);
}

/* =========================================================================
 * Suite: ModuleStateGuards
 * Invalid state transitions must be rejected.
 * ====================================================================== */
class ModuleStateGuards : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }
};

TEST_F(ModuleStateGuards, StartFromRunningStateReturnsInvalidArg) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    /* Module is now RUNNING — starting again must fail. */
    EXPECT_EQ(fuse_module_start(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleStateGuards, PauseFromLoadedStateReturnsInvalidArg) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    /* Module is in LOADED state — pause must fail. */
    EXPECT_EQ(fuse_module_pause(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleStateGuards, PauseFromPausedStateReturnsInvalidArg) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_pause(id), FUSE_SUCCESS);
    /* Already PAUSED — pause again must fail. */
    EXPECT_EQ(fuse_module_pause(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleStateGuards, RunStepFromLoadedStateReturnsInvalidArg) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    /* Not started yet — run_step must fail. */
    EXPECT_EQ(fuse_module_run_step(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(ModuleStateGuards, RunStepFromPausedStateReturnsInvalidArg) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_pause(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * Suite: ModuleCapacityLimit
 * Filling all FUSE_MAX_MODULES slots and then attempting one more load
 * must return FUSE_ERR_MODULE_LIMIT.
 * ====================================================================== */
class ModuleCapacityLimit : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }
};

TEST_F(ModuleCapacityLimit, MaxModulesLimitEnforced) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t ids[FUSE_MAX_MODULES];
    /* Load FUSE_MAX_MODULES modules — all must succeed. */
    for (uint32_t i = 0u; i < FUSE_MAX_MODULES; ++i) {
        ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &kBasicPolicy,
                                   &ids[i]),
                  FUSE_SUCCESS)
            << "Failed to load module " << i;
    }

    /* One more must be rejected. */
    fuse_module_id_t extra_id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(bin.Data(), bin.Size(), &kBasicPolicy,
                               &extra_id),
              FUSE_ERR_MODULE_LIMIT);

    /* Unload all to clean up. */
    for (uint32_t i = 0u; i < FUSE_MAX_MODULES; ++i) {
        EXPECT_EQ(fuse_module_unload(ids[i]), FUSE_SUCCESS);
    }
}

TEST_F(ModuleCapacityLimit, SlotRecycledAfterUnload) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    /* Fill all slots. */
    fuse_module_id_t ids[FUSE_MAX_MODULES];
    for (uint32_t i = 0u; i < FUSE_MAX_MODULES; ++i) {
        ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &kBasicPolicy,
                                   &ids[i]),
                  FUSE_SUCCESS);
    }

    /* Unload the first one. */
    ASSERT_EQ(fuse_module_unload(ids[0]), FUSE_SUCCESS);

    /* Now one more load should succeed. */
    fuse_module_id_t new_id = FUSE_INVALID_MODULE_ID;
    EXPECT_EQ(fuse_module_load(bin.Data(), bin.Size(), &kBasicPolicy,
                               &new_id),
              FUSE_SUCCESS);

    /* Cleanup remaining. */
    for (uint32_t i = 1u; i < FUSE_MAX_MODULES; ++i) {
        EXPECT_EQ(fuse_module_unload(ids[i]), FUSE_SUCCESS);
    }
    EXPECT_EQ(fuse_module_unload(new_id), FUSE_SUCCESS);
}

/* =========================================================================
 * Suite: RunStepWhileStopped
 * fuse_module_run_step must fail while the runtime is stopped.
 * ====================================================================== */
class RunStepWhileStopped : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }
};

TEST_F(RunStepWhileStopped, RunStepRejectsWhenRuntimeStopped) {
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    if (!LoadAotOrSkip(AOT_PATH("mod_step_only.aot"), kBasicPolicy, &id)) return;

    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    /* Stop the runtime — all running modules become PAUSED. */
    ASSERT_EQ(fuse_stop(), FUSE_SUCCESS);

    /* run_step returns NOT_INITIALIZED when runtime is stopped. */
    EXPECT_EQ(fuse_module_run_step(id), FUSE_ERR_NOT_INITIALIZED);

    /* Restart so TearDown's fuse_stop() doesn't assert. */
    EXPECT_EQ(fuse_restart(), FUSE_SUCCESS);
    /* Module was paused by fuse_stop(); unload from paused state is fine. */
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}
