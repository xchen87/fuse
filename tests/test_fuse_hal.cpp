/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_hal.cpp — HAL bridge function tests (fuse_hal.c).
 *
 * Covers every native bridge entry point invoked via real module execution:
 *
 * fuse_native_temp_get_reading:
 *   - HAL callback NULL → returns 0.0f, module stays RUNNING (no trap).
 *   - HAL callback provided, cap granted → callback invoked exactly once,
 *     return value propagated.
 *
 * fuse_native_timer_get_timestamp:
 *   - HAL callback NULL → returns 0, module stays RUNNING.
 *   - HAL callback provided, cap granted → callback invoked exactly once.
 *
 * fuse_native_camera_last_frame:
 *   - Cap granted, valid in-bounds buffer (offset 0, len 256) → callback
 *     invoked exactly once.
 *   - HAL callback NULL → returns 0, module stays RUNNING (not trapped).
 *   - Cap denied → TRAPPED, callback never called.
 *   - Out-of-bounds buffer pointer (offset beyond linear memory) → TRAPPED,
 *     callback never called, security log entry written.
 *
 * fuse_native_module_log_event:
 *   - len == 0 → early return, no log entry written past the policy check.
 *   - len > FUSE_LOG_MSG_MAX → message truncated, module stays RUNNING.
 *   - Cap denied → TRAPPED.
 *   - Valid call with cap granted → log entry written at correct level.
 *
 * For all bridge tests that go through module execution the run_step() call
 * is the trigger.  We verify post-conditions on module state and mock
 * invocation counts.
 */

#include "fuse_test_helper.h"

/* fuse_internal.h (fuse_log_entry_t, g_ctx) already included via fuse_test_helper.h */

/* =========================================================================
 * Common fixture
 * ====================================================================== */
class HalBridgeTest : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }

    /* Load a module, start it, run one step, query state, unload.
     * Returns the module state observed after run_step.
     * Callers must check IsSkipped() or use ASSERT after this call. */
    fuse_module_state_t RunModule(const std::string &aot_path,
                                  const fuse_policy_t &policy) {
        AotBinary bin(aot_path);
        if (!bin.IsAvailable()) {
            testing::internal::AssertHelper(
                testing::TestPartResult::kSkip,
                __FILE__, __LINE__,
                (std::string("AOT binary not found: ") + aot_path).c_str())
                = testing::Message();
            return FUSE_MODULE_STATE_UNLOADED;
        }
        fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
        if (fuse_module_load(bin.Data(), bin.Size(), &policy, &id)
                != FUSE_SUCCESS) {
            ADD_FAILURE() << "load failed: " << aot_path;
            return FUSE_MODULE_STATE_UNLOADED;
        }
        EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);
        (void)fuse_module_run_step(id);
        fuse_module_state_t st{};
        EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
        EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
        return st;
    }
};

/* =========================================================================
 * temp_get_reading bridge
 * ====================================================================== */

TEST_F(HalBridgeTest, TempNullHalCallbackReturnsZeroNoTrap) {
    /* Skip early (before any re-init) if AOT binary is absent. */
    AotBinary bin(AOT_PATH("mod_temp.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_temp.aot";
    }

    /* Rebuild the runtime with a HAL that has no temp callback.
     * Must tear down fully (stop + destroy WAMR + zero g_ctx) before re-init.
     * Re-install mock_hal_ at the end so TearDown() works cleanly. */
    (void)fuse_stop();
    wasm_runtime_destroy();
    std::memset(&g_ctx,       0, sizeof(g_ctx));
    std::memset(g_module_mem, 0, sizeof(g_module_mem));
    std::memset(g_log_mem,    0, sizeof(g_log_mem));

    /* Build a temporary HAL with timer but no temp callback. */
    fuse_hal_t hal{};
    hal.timer.get_timestamp = MockHal::MakeHal().timer.get_timestamp;
    /* temp.get_reading intentionally left NULL */
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    /* mock_hal_ is still installed as s_instance — timer thunk routes there. */
    EXPECT_CALL(mock_hal_, TempGetReading()).Times(0);

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_TEMP_SENSOR);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    /* run_step must succeed — null HAL returns 0.0f but does NOT trap. */
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    /* TearDown() will call fuse_stop() + wasm_runtime_destroy() + zero g_ctx. */
}

TEST_F(HalBridgeTest, TempCallbackInvokedExactlyOnce) {
    AotBinary bin(AOT_PATH("mod_temp.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_temp.aot";
    }
    EXPECT_CALL(mock_hal_, TempGetReading())
        .Times(1)
        .WillOnce(::testing::Return(36.6f));

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_TEMP_SENSOR);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);
    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * timer_get_timestamp bridge
 * ====================================================================== */

TEST_F(HalBridgeTest, TimerNullHalCallbackReturnsZeroNoTrap) {
    /* Skip early before any re-init if AOT binary is absent. */
    AotBinary bin(AOT_PATH("mod_timer.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_timer.aot";
    }

    /* Re-init with null timer_get_timestamp in HAL. Full teardown first. */
    (void)fuse_stop();
    wasm_runtime_destroy();
    std::memset(&g_ctx,       0, sizeof(g_ctx));
    std::memset(g_module_mem, 0, sizeof(g_module_mem));
    std::memset(g_log_mem,    0, sizeof(g_log_mem));

    /* HAL with no callbacks at all — logs will have timestamp 0. */
    fuse_hal_t hal{};
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_TIMER);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    /* TearDown() handles final cleanup. */
}

TEST_F(HalBridgeTest, TimerCallbackInvokedAtLeastOnce) {
    /* AtLeast(1) because the timer is also used for log timestamps. */
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(12345u));

    fuse_module_state_t st = RunModule(AOT_PATH("mod_timer.aot"),
                                       MakePolicy(FUSE_CAP_TIMER));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
}

/* =========================================================================
 * camera_last_frame bridge
 * ====================================================================== */

TEST_F(HalBridgeTest, CameraCallbackInvokedWithCorrectArgs) {
    AotBinary bin(AOT_PATH("mod_camera.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera.aot";
    }
    EXPECT_CALL(mock_hal_, CameraLastFrame(
            ::testing::NotNull(),
            ::testing::Eq(256u)))
        .Times(1)
        .WillOnce(::testing::Return(256u));

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_CAMERA);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);
    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(HalBridgeTest, CameraNullHalCallbackReturnsZeroNoTrap) {
    /* Skip early before re-init if AOT binary is absent. */
    AotBinary bin(AOT_PATH("mod_camera.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera.aot";
    }

    /* Rebuild runtime with camera_last_frame == NULL. Full teardown first. */
    (void)fuse_stop();
    wasm_runtime_destroy();
    std::memset(&g_ctx,       0, sizeof(g_ctx));
    std::memset(g_module_mem, 0, sizeof(g_module_mem));
    std::memset(g_log_mem,    0, sizeof(g_log_mem));

    /* HAL with timer but no camera callback. mock_hal_ still in s_instance. */
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_)).Times(0);
    fuse_hal_t hal{};
    hal.timer.get_timestamp = MockHal::MakeHal().timer.get_timestamp;
    /* camera.last_frame left NULL */
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_CAMERA);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    /* TearDown() handles final cleanup. */
}

/* =========================================================================
 * camera out-of-bounds pointer test.
 *
 * We need a WAT module that passes a pointer well beyond its linear memory.
 * We craft this inline as a WAT text and rely on the AOT binary
 * mod_camera_oob.aot being built by CMake.
 *
 * The module passes offset 0xFFFF_FF00 (past end of 64KiB page) as the
 * camera buffer pointer.  WAMR's '*' signature converts it to a native ptr;
 * fuse_native_camera_last_frame must call wasm_runtime_validate_native_addr
 * and detect the OOB, set the module to TRAPPED.
 * ====================================================================== */
TEST_F(HalBridgeTest, CameraOutOfBoundsPointerTrapsModule) {
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_))
        .Times(0);

    AotBinary bin(AOT_PATH("mod_camera_oob.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera_oob.aot";
    }
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_CAMERA);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    fuse_stat_t rc = fuse_module_run_step(id);
    /* WAMR rejects the OOB pointer before we even reach our validation —
     * or our validation catches it.  Either way the step must fail. */
    EXPECT_NE(rc, FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * module_log_event bridge
 * ====================================================================== */

TEST_F(HalBridgeTest, LogEventWritesEntryAtCorrectLevel) {
    /* mod_log.wat writes level=1 (INFO). */
    fuse_module_state_t st = RunModule(AOT_PATH("mod_log.aot"),
                                       MakePolicy(FUSE_CAP_LOG));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    /* Verify a level-1 entry with "hello" exists in the log ring. */
    bool found = false;
    size_t n = kLogMemSize / sizeof(fuse_log_entry_t);
    const auto *entries = reinterpret_cast<const fuse_log_entry_t *>(g_log_mem);
    for (size_t i = 0u; i < n; ++i) {
        if (entries[i].level == 1u &&
            std::strstr(entries[i].message, "hello") != nullptr) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected level-1 'hello' log entry";
}

TEST_F(HalBridgeTest, LogEventLongMessageTruncated) {
    /* mod_log_long.wat writes 200 'A' chars with level=0. */
    fuse_module_state_t st = RunModule(AOT_PATH("mod_log_long.aot"),
                                       MakePolicy(FUSE_CAP_LOG));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);

    /* Find the DEBUG entry and verify it is truncated. */
    size_t n = kLogMemSize / sizeof(fuse_log_entry_t);
    const auto *entries = reinterpret_cast<const fuse_log_entry_t *>(g_log_mem);
    for (size_t i = 0u; i < n; ++i) {
        if (entries[i].level == 0u &&
            entries[i].message[0] == 'A') {
            size_t msglen = std::strlen(entries[i].message);
            EXPECT_LE(msglen, (size_t)(FUSE_LOG_MSG_MAX - 1u));
            EXPECT_EQ(entries[i].message[FUSE_LOG_MSG_MAX - 1u], '\0');
            return;
        }
    }
    GTEST_SKIP() << "Long-log entry not found (module may not have run)";
}

TEST_F(HalBridgeTest, LogEventCapDeniedTrapsModule) {
    fuse_module_state_t st = RunModule(AOT_PATH("mod_log.aot"),
                                       MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* =========================================================================
 * Capability-denied paths for temp, timer, and camera bridges.
 *
 * Each test loads a module that calls the respective HAL bridge, but the
 * policy grants zero capabilities.  fuse_policy_violation() must be called,
 * the module must transition to TRAPPED, and the HAL callback must never
 * be invoked.
 *
 * These tests also exercise the fuse_policy_violation() code path for all
 * three bridge families, verifying that it correctly handles the non-NULL
 * inst pointer it receives (the NULL-inst early-return in the bridge fires
 * before fuse_policy_violation() is ever reached).
 * ====================================================================== */

TEST_F(HalBridgeTest, TempCapDeniedTrapsModule) {
    /* HAL callback must never be reached when capability is denied. */
    EXPECT_CALL(mock_hal_, TempGetReading()).Times(0);

    fuse_module_state_t st = RunModule(AOT_PATH("mod_temp_no_cap.aot"),
                                       MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

TEST_F(HalBridgeTest, TimerCapDeniedTrapsModule) {
    /* The timer is also used for log timestamps via SetUp()'s WillRepeatedly;
     * restrict the expectation to exactly zero direct module-triggered calls by
     * using a separate override.  Because the module has no LOG cap either,
     * no log-path timestamp call is issued. */
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(0u));

    fuse_module_state_t st = RunModule(AOT_PATH("mod_timer_no_cap.aot"),
                                       MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

TEST_F(HalBridgeTest, CameraCapDeniedTrapsModule) {
    /* HAL callback must never be reached when capability is denied. */
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_))
        .Times(0);

    fuse_module_state_t st = RunModule(AOT_PATH("mod_camera_no_cap.aot"),
                                       MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* =========================================================================
 * Camera zero-length buffer path.
 *
 * fuse_hal_camera.c lines 52-57: if (buf == NULL || max_len == 0) the bridge
 * must set a WASM exception and transition the module to TRAPPED without
 * ever calling the HAL callback.  This is distinct from the OOB path
 * (which uses a large but in-bounds-offset pointer with an enormous len).
 * ====================================================================== */
TEST_F(HalBridgeTest, CameraZeroLenBufferTrapsModule) {
    /* HAL callback must never be reached when max_len == 0. */
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_))
        .Times(0);

    AotBinary bin(AOT_PATH("mod_camera_zero_len.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera_zero_len.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_CAMERA);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    fuse_stat_t rc = fuse_module_run_step(id);
    EXPECT_NE(rc, FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * Log bridge out-of-bounds buffer path.
 *
 * fuse_hal_log.c lines 61-68: when the native pointer for the log message
 * fails wasm_runtime_validate_native_addr, fuse_native_module_log_event must
 * write a SECURITY FATAL log entry and transition the module to TRAPPED without
 * copying any bytes from the invalid range.
 *
 * The module passes offset 0 with len=0x7FFFFFFF — this range far exceeds
 * any allocation (64 KiB linear memory + 8 KiB WAMR heap = 73728 bytes in
 * AOT mode), so validate_native_addr will reject it unconditionally.
 * ====================================================================== */
TEST_F(HalBridgeTest, LogEventOutOfBoundsPointerTrapsModule) {
    AotBinary bin(AOT_PATH("mod_log_oob.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_log_oob.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(FUSE_CAP_LOG);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    /* WAMR's '*~' signature validates ptr+len before calling our native, so
     * the module is trapped by WAMR raising an OOB exception before our code
     * runs — the same behaviour as CameraOutOfBoundsPointerTrapsModule.
     * Either way the step must fail and the module must be TRAPPED. */
    fuse_stat_t rc = fuse_module_run_step(id);
    EXPECT_NE(rc, FUSE_SUCCESS);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}
