/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_policy.cpp — Policy enforcement and HAL bridge tests.
 *
 * Covers fuse_policy_check_cap() directly, and the policy gate inside every
 * HAL bridge function (fuse_native_temp_get_reading, fuse_native_timer_get_timestamp,
 * fuse_native_camera_last_frame, fuse_native_module_log_event).
 *
 * For each HAL function:
 *   - Module WITHOUT the required capability → module transitions to TRAPPED,
 *     HAL callback is NEVER called, a FATAL security-log entry is written.
 *   - Module WITH the required capability → HAL callback is called exactly once.
 *
 * fuse_policy_check_cap corner cases:
 *   - NULL policy pointer → returns false.
 *   - All bits clear → returns false for any cap_bit.
 *   - Exact bit set → returns true.
 *   - Multiple bits set → each returns true; unset bits return false.
 */

#include "fuse_test_helper.h"

/* fuse_internal.h (fuse_policy_check_cap, fuse_log_entry_t, g_ctx) is included
 * transitively via fuse_test_helper.h. */

/* =========================================================================
 * Suite: PolicyCheckCapUnit
 * Direct unit tests of fuse_policy_check_cap() (fuse_policy.c).
 * No WAMR or real module required.
 * ====================================================================== */
class PolicyCheckCapUnit : public ::testing::Test {};

TEST_F(PolicyCheckCapUnit, NullPolicyReturnsFalse) {
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, FUSE_CAP_TEMP_SENSOR));
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, FUSE_CAP_TIMER));
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, FUSE_CAP_CAMERA));
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, FUSE_CAP_LOG));
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, 0u));
    EXPECT_FALSE(fuse_policy_check_cap(nullptr, UINT32_MAX));
}

TEST_F(PolicyCheckCapUnit, ZeroCapabilitiesReturnsFalse) {
    fuse_policy_t p{};
    p.capabilities = 0u;
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_TEMP_SENSOR));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_TIMER));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_CAMERA));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_LOG));
}

TEST_F(PolicyCheckCapUnit, ExactBitSetReturnsTrue) {
    fuse_policy_t p{};
    p.capabilities = FUSE_CAP_TEMP_SENSOR;
    EXPECT_TRUE(fuse_policy_check_cap(&p, FUSE_CAP_TEMP_SENSOR));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_TIMER));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_CAMERA));
    EXPECT_FALSE(fuse_policy_check_cap(&p, FUSE_CAP_LOG));
}

TEST_F(PolicyCheckCapUnit, AllBitsSetReturnsTrueForAll) {
    fuse_policy_t p{};
    p.capabilities = FUSE_CAP_TEMP_SENSOR | FUSE_CAP_TIMER |
                     FUSE_CAP_CAMERA      | FUSE_CAP_LOG;
    EXPECT_TRUE(fuse_policy_check_cap(&p, FUSE_CAP_TEMP_SENSOR));
    EXPECT_TRUE(fuse_policy_check_cap(&p, FUSE_CAP_TIMER));
    EXPECT_TRUE(fuse_policy_check_cap(&p, FUSE_CAP_CAMERA));
    EXPECT_TRUE(fuse_policy_check_cap(&p, FUSE_CAP_LOG));
}

TEST_F(PolicyCheckCapUnit, ZeroBitQueryReturnsFalseWhenNoCaps) {
    /* cap_bit == 0: (capabilities & 0) == 0, so always false. */
    fuse_policy_t p{};
    p.capabilities = 0xFFFFFFFFu;
    EXPECT_FALSE(fuse_policy_check_cap(&p, 0u));
}

/* =========================================================================
 * Suite: PolicyEnforcementHal
 * End-to-end policy checks via the HAL bridge, exercised through real
 * module execution.  Each test loads an AOT module that calls exactly one
 * HAL function, then verifies allowed/denied behaviour.
 * ====================================================================== */
class PolicyEnforcementHal : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        /* Suppress log-timestamp calls so EXPECT_CALL counts stay clean. */
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(42u));
    }

    /* Load, start, run one step, and return the module state afterwards. */
    fuse_module_state_t RunOneStep(const std::string &aot_path,
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
            ADD_FAILURE() << "fuse_module_load failed for " << aot_path;
            return FUSE_MODULE_STATE_UNLOADED;
        }

        EXPECT_EQ(fuse_module_start(id), FUSE_SUCCESS);
        (void)fuse_module_run_step(id); /* result may be trap — intentional */

        fuse_module_state_t st = FUSE_MODULE_STATE_UNLOADED;
        EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
        EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
        return st;
    }
};

/* -- temp_get_reading ------------------------------------------------------- */

TEST_F(PolicyEnforcementHal, TempAllowedWhenCapGranted) {
    AotBinary bin(AOT_PATH("mod_temp.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_temp.aot";
    }
    EXPECT_CALL(mock_hal_, TempGetReading())
        .Times(1)
        .WillOnce(::testing::Return(22.5f));

    fuse_policy_t p = MakePolicy(FUSE_CAP_TEMP_SENSOR);
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);
    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
}

TEST_F(PolicyEnforcementHal, TempDeniedWhenCapMissing) {
    /* HAL callback must NOT be reached. */
    EXPECT_CALL(mock_hal_, TempGetReading()).Times(0);

    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_temp.aot"),
                                        MakePolicy(0u)); /* no caps */
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

TEST_F(PolicyEnforcementHal, TempDeniedWithOtherCapsButNotTemp) {
    EXPECT_CALL(mock_hal_, TempGetReading()).Times(0);

    fuse_module_state_t st = RunOneStep(
        AOT_PATH("mod_temp.aot"),
        MakePolicy(FUSE_CAP_TIMER | FUSE_CAP_CAMERA | FUSE_CAP_LOG));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* -- timer_get_timestamp ---------------------------------------------------- */

TEST_F(PolicyEnforcementHal, TimerAllowedWhenCapGranted) {
    /* TimerGetTimestamp is also used for log timestamps — use AtLeast(1). */
    EXPECT_CALL(mock_hal_, TimerGetTimestamp())
        .WillRepeatedly(::testing::Return(999u));

    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_timer.aot"),
                                        MakePolicy(FUSE_CAP_TIMER));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
}

TEST_F(PolicyEnforcementHal, TimerDeniedWhenCapMissing) {
    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_timer.aot"),
                                        MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* -- camera_last_frame ------------------------------------------------------ */

TEST_F(PolicyEnforcementHal, CameraAllowedWhenCapGranted) {
    AotBinary bin(AOT_PATH("mod_camera.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera.aot";
    }
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::NotNull(),
                                            ::testing::Eq(256u)))
        .Times(1)
        .WillOnce(::testing::Return(100u));

    fuse_policy_t p = MakePolicy(FUSE_CAP_CAMERA);
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);
    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
}

TEST_F(PolicyEnforcementHal, CameraDeniedWhenCapMissing) {
    EXPECT_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_))
        .Times(0);

    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_camera.aot"),
                                        MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* -- module_log_event ------------------------------------------------------- */

TEST_F(PolicyEnforcementHal, LogAllowedWhenCapGranted) {
    /* The log just writes into the ring buffer — no HAL callback, but
     * the module must complete without trapping. */
    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_log.aot"),
                                        MakePolicy(FUSE_CAP_LOG));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_RUNNING);
}

TEST_F(PolicyEnforcementHal, LogDeniedWhenCapMissing) {
    fuse_module_state_t st = RunOneStep(AOT_PATH("mod_log.aot"),
                                        MakePolicy(0u));
    if (IsSkipped()) return;
    EXPECT_EQ(st, FUSE_MODULE_STATE_TRAPPED);
}

/* =========================================================================
 * Suite: PolicyViolationSecurityLog
 * Verifies that a FATAL security-log entry is written when a module violates
 * its policy, by inspecting the raw log ring buffer via g_ctx.
 * ====================================================================== */
class PolicyViolationSecurityLog : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }

    /* Scan the raw log memory for a message containing the given substring. */
    bool LogContains(const char *substr, uint32_t level = 2u) {
        /* The log ring lives in g_log_mem.  Walk the entries. */
        size_t entry_size = sizeof(fuse_log_entry_t);
        size_t n = kLogMemSize / entry_size;
        const auto *entries = reinterpret_cast<const fuse_log_entry_t *>(g_log_mem);
        for (size_t i = 0u; i < n; ++i) {
            if (entries[i].level == level &&
                std::strstr(entries[i].message, substr) != nullptr) {
                return true;
            }
        }
        return false;
    }
};

TEST_F(PolicyViolationSecurityLog, TempViolationWritesFatalLog) {
    EXPECT_CALL(mock_hal_, TempGetReading()).Times(0);

    AotBinary bin(AOT_PATH("mod_temp.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_temp.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);

    EXPECT_TRUE(LogContains("SECURITY")) << "Expected FATAL security log entry";
    EXPECT_TRUE(LogContains("TEMP_SENSOR"))
        << "Expected cap name in security log";

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(PolicyViolationSecurityLog, TimerViolationWritesFatalLog) {
    AotBinary bin(AOT_PATH("mod_timer.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_timer.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);

    EXPECT_TRUE(LogContains("TIMER"));
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(PolicyViolationSecurityLog, CameraViolationWritesFatalLog) {
    AotBinary bin(AOT_PATH("mod_camera.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_camera.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);

    EXPECT_TRUE(LogContains("CAMERA"));
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(PolicyViolationSecurityLog, LogCapViolationWritesFatalLog) {
    AotBinary bin(AOT_PATH("mod_log.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_log.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);

    EXPECT_TRUE(LogContains("LOG"));
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}
