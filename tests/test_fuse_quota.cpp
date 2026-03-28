/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_quota.cpp — CPU-quota enforcement tests.
 *
 * Covers:
 *  - cpu_quota_us == 0 → quota_arm is NEVER called, quota_cancel NEVER called.
 *  - cpu_quota_us > 0 AND hal.quota_arm != NULL → quota_arm called with correct
 *    (id, quota_us) before the step; quota_cancel called with correct id after.
 *  - cpu_quota_us > 0 AND hal.quota_arm == NULL → quota_arm not called (NULL
 *    guard in fuse_module_run_step), module executes normally.
 *  - cpu_quota_us > 0 AND hal.quota_cancel == NULL → quota_cancel guard:
 *    module executes and completes normally (no crash on NULL cancel).
 *  - fuse_quota_expired() called for a running module →
 *      module state becomes FUSE_MODULE_STATE_QUOTA_EXCEEDED,
 *      fuse_module_run_step returns FUSE_ERR_QUOTA_EXCEEDED,
 *      a FATAL log entry is written.
 *  - fuse_module_run_step on a QUOTA_EXCEEDED module → FUSE_ERR_INVALID_ARG.
 *  - fuse_quota_expired() with FUSE_INVALID_MODULE_ID → no crash.
 *  - fuse_quota_expired() with out-of-range id (>= FUSE_MAX_MODULES) → no crash.
 *  - fuse_quota_expired() before fuse_init → no crash.
 *  - fuse_quota_expired() for a module_id not in use → no crash.
 *
 * Quota expiry in practice is asynchronous (ISR fires during WASM execution).
 * In unit tests we simulate it by calling fuse_quota_expired() from the
 * quota_arm mock callback itself — this happens synchronously before
 * wasm_runtime_call_wasm() returns, which is safe because
 * wasm_runtime_terminate() is async-signal-safe per WAMR documentation.
 */

#include "fuse_test_helper.h"

/* fuse_internal.h already included via fuse_test_helper.h */

/* =========================================================================
 * Common fixture
 * ====================================================================== */
class QuotaTest : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }

    /* Scan g_log_mem for a FATAL (level==2) entry containing substr. */
    bool FatalLogContains(const char *substr) {
        size_t n = kLogMemSize / sizeof(fuse_log_entry_t);
        const auto *entries =
            reinterpret_cast<const fuse_log_entry_t *>(g_log_mem);
        for (size_t i = 0u; i < n; ++i) {
            if (entries[i].level == 2u &&
                std::strstr(entries[i].message, substr) != nullptr) {
                return true;
            }
        }
        return false;
    }
};

/* =========================================================================
 * Suite: QuotaArmCancel
 * Verifies quota_arm / quota_cancel invocation contract.
 * ====================================================================== */
class QuotaArmCancel : public QuotaTest {};

TEST_F(QuotaArmCancel, ZeroQuotaNoArmNorCancel) {
    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_)).Times(0);
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(0);

    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(QuotaArmCancel, NonZeroQuotaArmCalledBeforeStepCancelCalledAfter) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t loaded_id = FUSE_INVALID_MODULE_ID;

    {
        ::testing::InSequence seq;
        EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::Eq(500u)))
            .Times(1);
        EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_))
            .Times(1);
    }

    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/500u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &loaded_id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(loaded_id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(loaded_id), FUSE_SUCCESS);

    EXPECT_EQ(fuse_module_unload(loaded_id), FUSE_SUCCESS);
}

TEST_F(QuotaArmCancel, QuotaArmCalledWithCorrectModuleId) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t captured_arm_id = FUSE_INVALID_MODULE_ID;

    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_))
        .WillOnce([&captured_arm_id](fuse_module_id_t id, uint32_t) {
            captured_arm_id = id;
        });
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(1);

    fuse_module_id_t loaded_id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/100u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &loaded_id),
              FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(loaded_id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(loaded_id), FUSE_SUCCESS);

    EXPECT_EQ(captured_arm_id, loaded_id)
        << "quota_arm must receive the module's own ID";

    EXPECT_EQ(fuse_module_unload(loaded_id), FUSE_SUCCESS);
}

TEST_F(QuotaArmCancel, NullQuotaArmCallbackNoCrash) {
    /* Skip early before re-init if AOT binary is absent. */
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    /* Re-init with quota_arm and quota_cancel == NULL. Full teardown first. */
    (void)fuse_stop();
    if (g_ctx.initialized) { wasm_runtime_destroy(); }
    std::memset(&g_ctx,       0, sizeof(g_ctx));
    std::memset(g_module_mem, 0, sizeof(g_module_mem));
    std::memset(g_log_mem,    0, sizeof(g_log_mem));

    /* Use mock_hal_ (still in s_instance) but set quota callbacks to NULL in HAL. */
    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_)).Times(0);
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(0);

    fuse_hal_t hal{};
    hal.timer.get_timestamp = MockHal::MakeHal().timer.get_timestamp;
    /* quota_arm and quota_cancel are NULL in the hal struct */
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/200u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    /* Must complete without crash even though quota_arm is NULL. */
    EXPECT_EQ(fuse_module_run_step(id), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
    /* TearDown() handles final cleanup. */
}

/* =========================================================================
 * Suite: QuotaExpiry
 * Simulates ISR-triggered quota expiry by calling fuse_quota_expired()
 * from inside the quota_arm mock.
 * ====================================================================== */
class QuotaExpiry : public QuotaTest {};

TEST_F(QuotaExpiry, QuotaExpiredTransitionsToQuotaExceededState) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t loaded_id = FUSE_INVALID_MODULE_ID;

    /* quota_arm fires quota expiry synchronously to simulate the ISR. */
    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_))
        .WillOnce([&loaded_id](fuse_module_id_t id, uint32_t) {
            fuse_quota_expired(id);
            loaded_id = id; /* capture for later assertions */
        });
    /* quota_cancel is still called after wasm_runtime_call_wasm returns. */
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(1);

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/1000u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);

    fuse_stat_t rc = fuse_module_run_step(id);
    EXPECT_EQ(rc, FUSE_ERR_QUOTA_EXCEEDED);

    fuse_module_state_t st{};
    EXPECT_EQ(fuse_module_stat(id, &st), FUSE_SUCCESS);
    EXPECT_EQ(st, FUSE_MODULE_STATE_QUOTA_EXCEEDED);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

TEST_F(QuotaExpiry, QuotaExpiredWritesFatalLog) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_))
        .WillOnce([](fuse_module_id_t id, uint32_t) {
            fuse_quota_expired(id);
        });
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(1);
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/1000u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    (void)fuse_module_run_step(id);

    EXPECT_TRUE(FatalLogContains("quota exceeded"))
        << "Expected FATAL 'quota exceeded' log entry";
}

TEST_F(QuotaExpiry, RunStepOnQuotaExceededModuleReturnsInvalidArg) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    EXPECT_CALL(mock_hal_, QuotaArm(::testing::_, ::testing::_))
        .WillOnce([](fuse_module_id_t id, uint32_t) {
            fuse_quota_expired(id);
        });
    EXPECT_CALL(mock_hal_, QuotaCancel(::testing::_)).Times(1);
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u, 1u, 8192u, 8192u, /*quota=*/1000u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_run_step(id), FUSE_ERR_QUOTA_EXCEEDED);

    /* Second run_step on QUOTA_EXCEEDED must fail with INVALID_ARG. */
    EXPECT_EQ(fuse_module_run_step(id), FUSE_ERR_INVALID_ARG);

    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * Suite: QuotaExpiredEdgeCases
 * Edge-case calls to fuse_quota_expired() that must not crash.
 * ====================================================================== */
class QuotaExpiredEdgeCases : public ::testing::Test {
protected:
    void SetUp() override {
        /* Guard against double-destroy: only call wasm_runtime_destroy()
         * if WAMR is currently initialized. */
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

TEST_F(QuotaExpiredEdgeCases, CalledBeforeFuseInitNoCrash) {
    /* g_ctx.initialized == false. Must return immediately without crash. */
    fuse_quota_expired(0u);
    fuse_quota_expired(FUSE_INVALID_MODULE_ID);
}

TEST_F(QuotaExpiredEdgeCases, CalledWithOutOfRangeIdNoCrash) {
    static uint8_t mod_mem[512 * 1024];
    static uint8_t log_mem[16 * 1024];
    std::memset(mod_mem, 0, sizeof(mod_mem));
    std::memset(log_mem, 0, sizeof(log_mem));

    MockHal m;
    MockHal::Install(&m);
    ON_CALL(m, TimerGetTimestamp()).WillByDefault(::testing::Return(0u));
    EXPECT_CALL(m, TimerGetTimestamp()).WillRepeatedly(::testing::Return(0u));

    fuse_hal_t hal{};
    hal.timer.get_timestamp = MockHal::MakeHalTimerOnly().timer.get_timestamp;
    ASSERT_EQ(fuse_init(mod_mem, sizeof(mod_mem),
                        log_mem, sizeof(log_mem), &hal), FUSE_SUCCESS);

    /* id >= FUSE_MAX_MODULES must be rejected early. */
    fuse_quota_expired(FUSE_MAX_MODULES);
    fuse_quota_expired(FUSE_MAX_MODULES + 100u);
    fuse_quota_expired(FUSE_INVALID_MODULE_ID);

    (void)fuse_stop();
    MockHal::Uninstall();
}

TEST_F(QuotaExpiredEdgeCases, CalledForUnusedSlotNoCrash) {
    static uint8_t mod_mem[512 * 1024];
    static uint8_t log_mem[16 * 1024];
    std::memset(mod_mem, 0, sizeof(mod_mem));
    std::memset(log_mem, 0, sizeof(log_mem));

    MockHal m;
    MockHal::Install(&m);
    ON_CALL(m, TimerGetTimestamp()).WillByDefault(::testing::Return(0u));
    EXPECT_CALL(m, TimerGetTimestamp()).WillRepeatedly(::testing::Return(0u));

    fuse_hal_t hal{};
    hal.timer.get_timestamp = MockHal::MakeHalTimerOnly().timer.get_timestamp;
    ASSERT_EQ(fuse_init(mod_mem, sizeof(mod_mem),
                        log_mem, sizeof(log_mem), &hal), FUSE_SUCCESS);

    /* No module loaded — all slots are empty; quota_expired must be a no-op. */
    for (uint32_t i = 0u; i < FUSE_MAX_MODULES; ++i) {
        fuse_quota_expired(i);
    }

    (void)fuse_stop();
    MockHal::Uninstall();
}
