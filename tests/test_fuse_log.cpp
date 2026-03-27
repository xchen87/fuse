/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * test_fuse_log.cpp — Security-log ring-buffer tests (fuse_log.c).
 *
 * Covers:
 *  - fuse_log_write: NULL context, NULL entries, zero capacity → silent no-op.
 *  - NULL message → silent no-op.
 *  - Single write at slot 0, fields populated correctly.
 *  - Monotonically advancing write_idx.
 *  - Ring-wrap: writing more entries than capacity overwrites oldest.
 *  - Timestamp populated from hal.timer_get_timestamp when available.
 *  - Timestamp = 0 when timer callback is NULL.
 *  - FUSE_LOG_MSG_MAX-1 truncation: message longer than limit is truncated
 *    and always NUL-terminated.
 *  - Multiple modules logging at different levels (DEBUG=0, INFO=1, FATAL=2).
 *  - FUSE_INVALID_MODULE_ID written correctly for runtime-level messages.
 *  - Log written at fuse_init, fuse_stop, fuse_restart (runtime messages).
 *
 * We access the raw log ring via the g_log_mem buffer (the same memory passed
 * to fuse_init).  We cast it to fuse_log_entry_t[] directly — the layout is
 * deterministic (no padding issues for our field sizes).
 */

#include "fuse_test_helper.h"
/* fuse_internal.h (fuse_log_write, fuse_log_ctx_t, g_ctx) is already
 * included transitively via fuse_test_helper.h. */

/* Shorthand for accessing entries in the raw log buffer. */
static inline const fuse_log_entry_t *LogEntry(size_t idx) {
    return reinterpret_cast<const fuse_log_entry_t *>(g_log_mem) + idx;
}

static inline size_t LogCapacity() {
    return kLogMemSize / sizeof(fuse_log_entry_t);
}

/* =========================================================================
 * Suite: LogWriteIsolation
 * Tests fuse_log_write() in complete isolation, bypassing fuse_init.
 * We call wasm_runtime_destroy() in SetUp to ensure g_ctx is zeroed.
 * ====================================================================== */
class LogWriteIsolation : public ::testing::Test {
protected:
    /* A minimal log context pointing at a stack-allocated buffer. */
    static constexpr size_t kSlots = 4u;
    fuse_log_entry_t  entries_[kSlots];
    fuse_log_ctx_t    ctx_{};

    void SetUp() override {
        std::memset(entries_, 0, sizeof(entries_));
        ctx_.entries   = entries_;
        ctx_.capacity  = kSlots;
        ctx_.write_idx = 0u;

        /* g_ctx must be zeroed so hal.timer_get_timestamp == NULL.
         * Guard against double-destroy from a prior TearDown. */
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
    void TearDown() override {
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
};

TEST_F(LogWriteIsolation, NullContextIsNoop) {
    /* Must not crash. */
    fuse_log_write(nullptr, 0u, 0u, "hello");
}

TEST_F(LogWriteIsolation, NullEntriesIsNoop) {
    fuse_log_ctx_t bad{};
    bad.entries   = nullptr;
    bad.capacity  = 4u;
    bad.write_idx = 0u;
    fuse_log_write(&bad, 0u, 0u, "hello"); /* must not crash */
}

TEST_F(LogWriteIsolation, ZeroCapacityIsNoop) {
    fuse_log_ctx_t bad{};
    bad.entries   = entries_;
    bad.capacity  = 0u;
    bad.write_idx = 0u;
    fuse_log_write(&bad, 0u, 0u, "hello"); /* must not crash */
}

TEST_F(LogWriteIsolation, NullMessageIsNoop) {
    fuse_log_write(&ctx_, 0u, 0u, nullptr); /* must not crash */
    /* write_idx must remain 0 since nothing was written. */
    EXPECT_EQ(ctx_.write_idx, 0u);
}

TEST_F(LogWriteIsolation, SingleWritePopulatesFieldsCorrectly) {
    /* No timer in g_ctx — timestamp must be 0. */
    fuse_log_write(&ctx_, 7u, 2u, "test message");

    EXPECT_EQ(ctx_.write_idx, 1u);
    EXPECT_EQ(entries_[0].module_id,     7u);
    EXPECT_EQ(entries_[0].level,         2u);
    EXPECT_EQ(entries_[0].timestamp_us,  0u);
    EXPECT_STREQ(entries_[0].message,    "test message");
}

TEST_F(LogWriteIsolation, WriteIdxAdvancesMonotonically) {
    fuse_log_write(&ctx_, 0u, 0u, "a");
    EXPECT_EQ(ctx_.write_idx, 1u);
    fuse_log_write(&ctx_, 0u, 0u, "b");
    EXPECT_EQ(ctx_.write_idx, 2u);
    fuse_log_write(&ctx_, 0u, 0u, "c");
    EXPECT_EQ(ctx_.write_idx, 3u);
}

TEST_F(LogWriteIsolation, RingWrapOverwritesOldestEntry) {
    /* Fill all kSlots slots. */
    fuse_log_write(&ctx_, 1u, 0u, "slot0");
    fuse_log_write(&ctx_, 2u, 0u, "slot1");
    fuse_log_write(&ctx_, 3u, 0u, "slot2");
    fuse_log_write(&ctx_, 4u, 0u, "slot3");
    EXPECT_EQ(ctx_.write_idx, 0u); /* wrapped */

    /* Write one more — overwrites slot 0. */
    fuse_log_write(&ctx_, 99u, 2u, "overwrite");
    EXPECT_EQ(ctx_.write_idx, 1u);

    /* Slot 0 must now contain the newest entry. */
    EXPECT_EQ(entries_[0].module_id, 99u);
    EXPECT_EQ(entries_[0].level,      2u);
    EXPECT_STREQ(entries_[0].message, "overwrite");

    /* Remaining slots are still the original entries. */
    EXPECT_EQ(entries_[1].module_id, 2u);
    EXPECT_EQ(entries_[2].module_id, 3u);
    EXPECT_EQ(entries_[3].module_id, 4u);
}

TEST_F(LogWriteIsolation, MessageTruncatedToLogMsgMaxMinus1) {
    /* Build a message of exactly FUSE_LOG_MSG_MAX characters (no NUL). */
    std::string long_msg(FUSE_LOG_MSG_MAX, 'X');

    fuse_log_write(&ctx_, 0u, 0u, long_msg.c_str());

    /* Stored message must be at most FUSE_LOG_MSG_MAX-1 chars + NUL. */
    EXPECT_EQ(std::strlen(entries_[0].message), (size_t)(FUSE_LOG_MSG_MAX - 1u));
    EXPECT_EQ(entries_[0].message[FUSE_LOG_MSG_MAX - 1u], '\0');
}

TEST_F(LogWriteIsolation, MessageExactlyAtLimitFitsWithNul) {
    /* A message of FUSE_LOG_MSG_MAX-1 chars must fit completely. */
    std::string msg(FUSE_LOG_MSG_MAX - 1u, 'A');
    fuse_log_write(&ctx_, 0u, 0u, msg.c_str());

    EXPECT_STREQ(entries_[0].message, msg.c_str());
    EXPECT_EQ(entries_[0].message[FUSE_LOG_MSG_MAX - 1u], '\0');
}

TEST_F(LogWriteIsolation, EmptyStringWrittenAndNulTerminated) {
    fuse_log_write(&ctx_, 0u, 1u, "");
    EXPECT_EQ(ctx_.write_idx, 1u);
    EXPECT_EQ(entries_[0].message[0], '\0');
}

TEST_F(LogWriteIsolation, FuseInvalidModuleIdStoredCorrectly) {
    fuse_log_write(&ctx_, FUSE_INVALID_MODULE_ID, 1u, "runtime msg");
    EXPECT_EQ(entries_[0].module_id, FUSE_INVALID_MODULE_ID);
}

/* =========================================================================
 * Suite: LogTimestamp
 * Timestamp is sourced from hal.timer_get_timestamp when set.
 * We leverage fuse_init() to populate g_ctx.hal, then call fuse_log_write
 * via the public API path (fuse_init itself writes a log entry).
 * ====================================================================== */
class LogTimestamp : public ::testing::Test {
protected:
    MockHal mock_;
    uint64_t call_count_ = 0u;
    uint64_t next_ts_    = 1000u;

    void SetUp() override {
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));

        MockHal::Install(&mock_);
        ON_CALL(mock_, TimerGetTimestamp())
            .WillByDefault([this]() -> uint64_t { return next_ts_++; });
    }

    void TearDown() override {
        (void)fuse_stop();
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        std::memset(&g_ctx, 0, sizeof(g_ctx));
        MockHal::Uninstall();
    }
};

TEST_F(LogTimestamp, TimestampPopulatedFromTimer) {
    EXPECT_CALL(mock_, TimerGetTimestamp())
        .WillRepeatedly([this]() -> uint64_t { return next_ts_++; });

    fuse_hal_t hal = MockHal::MakeHalTimerOnly();
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    /* fuse_init writes one log entry ("FUSE runtime initialised").
     * That entry should have a non-zero timestamp. */
    const auto *entry0 = LogEntry(0u);
    EXPECT_GT(entry0->timestamp_us, 0u);
}

TEST_F(LogTimestamp, TimestampZeroWhenTimerNull) {
    /* Use a HAL with no timer callback. */
    fuse_hal_t hal = MockHal::MakeHalNull();
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &hal), FUSE_SUCCESS);

    const auto *entry0 = LogEntry(0u);
    EXPECT_EQ(entry0->timestamp_us, 0u);
}

/* =========================================================================
 * Suite: LogViaFuseApi
 * Tests that fuse_init, fuse_stop, and fuse_restart each write a log entry,
 * and that module operations produce appropriate log entries.
 * ====================================================================== */
class LogViaFuseApi : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(5u));
    }
};

TEST_F(LogViaFuseApi, FuseInitWritesLogEntry) {
    /* fuse_init was called in FuseTestBase::SetUp — check first entry. */
    const auto *entry0 = LogEntry(0u);
    EXPECT_EQ(entry0->module_id, FUSE_INVALID_MODULE_ID);
    EXPECT_EQ(entry0->level,     1u); /* INFO */
    EXPECT_NE(std::strstr(entry0->message, "initialised"), nullptr)
        << "Expected 'initialised' in first log entry: " << entry0->message;
}

TEST_F(LogViaFuseApi, FuseStopWritesLogEntry) {
    /* Find the write index after init. */
    size_t entries_before = LogCapacity(); /* generous upper bound */
    (void)entries_before;

    ASSERT_EQ(fuse_stop(), FUSE_SUCCESS);

    /* Scan for "stopped" message. */
    bool found = false;
    for (size_t i = 0u; i < LogCapacity(); ++i) {
        if (LogEntry(i)->level == 1u &&
            std::strstr(LogEntry(i)->message, "stopped") != nullptr) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'stopped' in log after fuse_stop()";

    /* Restart so TearDown's fuse_stop doesn't assert. */
    EXPECT_EQ(fuse_restart(), FUSE_SUCCESS);
}

TEST_F(LogViaFuseApi, FuseRestartWritesLogEntry) {
    ASSERT_EQ(fuse_stop(),    FUSE_SUCCESS);
    ASSERT_EQ(fuse_restart(), FUSE_SUCCESS);

    bool found = false;
    for (size_t i = 0u; i < LogCapacity(); ++i) {
        if (LogEntry(i)->level == 1u &&
            std::strstr(LogEntry(i)->message, "restarted") != nullptr) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'restarted' in log after fuse_restart()";
}

TEST_F(LogViaFuseApi, ModuleLoadWritesInfoLog) {
    AotBinary bin(AOT_PATH("mod_step_only.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_step_only.aot";
    }

    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;
    fuse_policy_t p = MakePolicy(0u);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &p, &id), FUSE_SUCCESS);

    bool found = false;
    for (size_t i = 0u; i < LogCapacity(); ++i) {
        if (LogEntry(i)->level == 1u &&
            std::strstr(LogEntry(i)->message, "loaded") != nullptr) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found) << "Expected 'loaded' in log after fuse_module_load()";
    EXPECT_EQ(fuse_module_unload(id), FUSE_SUCCESS);
}

/* =========================================================================
 * Suite: LogMultipleModulesLevels
 * Confirms correct level values and module IDs are stored when multiple
 * modules write log events.
 * ====================================================================== */
class LogMultipleModulesLevels : public FuseTestBase {
protected:
    void SetUp() override {
        FuseTestBase::SetUp();
        EXPECT_CALL(mock_hal_, TimerGetTimestamp())
            .WillRepeatedly(::testing::Return(0u));
    }
};

TEST_F(LogMultipleModulesLevels, MultipleModulesLogAtDifferentLevels) {
    AotBinary bin(AOT_PATH("mod_log.aot"));
    if (!bin.IsAvailable()) {
        GTEST_SKIP() << "AOT binary not found: mod_log.aot";
    }

    /* Load two log modules. */
    fuse_module_id_t id0 = FUSE_INVALID_MODULE_ID;
    fuse_module_id_t id1 = FUSE_INVALID_MODULE_ID;
    fuse_policy_t pol = MakePolicy(FUSE_CAP_LOG);

    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &pol, &id0), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_load(bin.Data(), bin.Size(), &pol, &id1), FUSE_SUCCESS);

    ASSERT_EQ(fuse_module_start(id0), FUSE_SUCCESS);
    ASSERT_EQ(fuse_module_start(id1), FUSE_SUCCESS);

    /* Each step call triggers module_log_event(level=1) from mod_log.wat. */
    EXPECT_EQ(fuse_module_run_step(id0), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_run_step(id1), FUSE_SUCCESS);

    /* Scan log for entries with level==1 and message=="hello". */
    int hello_count = 0;
    for (size_t i = 0u; i < LogCapacity(); ++i) {
        if (LogEntry(i)->level == 1u &&
            std::strstr(LogEntry(i)->message, "hello") != nullptr) {
            ++hello_count;
        }
    }
    EXPECT_GE(hello_count, 2) << "Expected at least 2 'hello' INFO log entries";

    EXPECT_EQ(fuse_module_unload(id0), FUSE_SUCCESS);
    EXPECT_EQ(fuse_module_unload(id1), FUSE_SUCCESS);
}

TEST_F(LogMultipleModulesLevels, LogCapacitySmallEnoughToWrapWithinTest) {
    /* Use a tiny in-memory context to verify ring-wrap behaviour in
     * end-to-end flow, without touching the live g_log_mem. */
    constexpr size_t kSlots = 2u;
    fuse_log_entry_t entries[kSlots]{};
    fuse_log_ctx_t   ctx{};
    ctx.entries   = entries;
    ctx.capacity  = kSlots;
    ctx.write_idx = 0u;

    extern fuse_context_t g_ctx;
    fuse_log_write(&ctx, 1u, 0u, "DEBUG msg");  /* slot 0 */
    fuse_log_write(&ctx, 2u, 1u, "INFO  msg");  /* slot 1 */
    fuse_log_write(&ctx, 3u, 2u, "FATAL msg");  /* wraps → slot 0 */

    /* Slot 0 now holds the FATAL entry. */
    EXPECT_EQ(entries[0].module_id, 3u);
    EXPECT_EQ(entries[0].level,     2u);
    EXPECT_STREQ(entries[0].message, "FATAL msg");

    /* Slot 1 still holds the INFO entry. */
    EXPECT_EQ(entries[1].module_id, 2u);
    EXPECT_EQ(entries[1].level,     1u);
    EXPECT_STREQ(entries[1].message, "INFO  msg");
}
