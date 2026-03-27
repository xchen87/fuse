/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_init_test.cpp — Smoke-level unit tests for FUSE runtime init/stop/restart.
 *
 * These tests exercise the public Host API surface without loading any real
 * WASM modules.  Hardware callbacks are provided via lambda stubs so no
 * physical hardware is required.
 */

#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>

extern "C" {
#include "fuse.h"
#include "wasm_export.h"
#include "../core/fuse_internal.h"
}

/* -------------------------------------------------------------------------
 * Shared test fixture memory regions
 * ---------------------------------------------------------------------- */
static constexpr size_t kModuleMemSize = (512u * 1024u);   /* 512 KiB */
static constexpr size_t kLogMemSize    = (16u * 1024u);    /* 16 KiB  */

static uint8_t  g_module_mem[kModuleMemSize];
static uint8_t  g_log_mem[kLogMemSize];

/* Simple HAL stubs */
static float    stub_temp(void)            { return 25.0f; }
static uint64_t stub_timer(void)           { return 0u; }
static uint64_t stub_camera(void *, uint32_t) { return 0u; }

static const fuse_hal_t kTestHal = {
    stub_temp,
    stub_timer,
    stub_camera,
    nullptr,   /* quota_arm   */
    nullptr    /* quota_cancel */
};

/* -------------------------------------------------------------------------
 * Helper: ensure runtime is de-initialised between tests.
 * We call fuse_stop() defensively; a fresh g_ctx is set up per test.
 * ---------------------------------------------------------------------- */
class FuseTest : public ::testing::Test {
protected:
    void SetUp() override {
        /* Each test gets a clean context. */
        (void)std::memset(g_module_mem, 0, sizeof(g_module_mem));
        (void)std::memset(g_log_mem,    0, sizeof(g_log_mem));
        /* Zero g_ctx so g_ctx.initialized == false for every test. */
        (void)std::memset(&g_ctx, 0, sizeof(g_ctx));
    }

    void TearDown() override {
        /* Stop FUSE if it was started, then tear down WAMR and zero g_ctx.
         * Guard against double-destroy when fuse_init was never called. */
        (void)fuse_stop();
        if (g_ctx.initialized) {
            wasm_runtime_destroy();
        }
        (void)std::memset(&g_ctx, 0, sizeof(g_ctx));
    }
};

/* -------------------------------------------------------------------------
 * fuse_init tests
 * ---------------------------------------------------------------------- */

TEST_F(FuseTest, InitNullModuleMemory) {
    fuse_stat_t s = fuse_init(nullptr, kModuleMemSize,
                              g_log_mem, kLogMemSize, &kTestHal);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);
}

TEST_F(FuseTest, InitNullLogMemory) {
    fuse_stat_t s = fuse_init(g_module_mem, kModuleMemSize,
                              nullptr, kLogMemSize, &kTestHal);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);
}

TEST_F(FuseTest, InitNullHal) {
    fuse_stat_t s = fuse_init(g_module_mem, kModuleMemSize,
                              g_log_mem, kLogMemSize, nullptr);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);
}

TEST_F(FuseTest, InitZeroModuleMemSize) {
    fuse_stat_t s = fuse_init(g_module_mem, 0u,
                              g_log_mem, kLogMemSize, &kTestHal);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);
}

TEST_F(FuseTest, InitZeroLogMemSize) {
    fuse_stat_t s = fuse_init(g_module_mem, kModuleMemSize,
                              g_log_mem, 0u, &kTestHal);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);
}

TEST_F(FuseTest, InitSuccess) {
    fuse_stat_t s = fuse_init(g_module_mem, kModuleMemSize,
                              g_log_mem, kLogMemSize, &kTestHal);
    EXPECT_EQ(s, FUSE_SUCCESS);
    /* Cleanup */
    (void)fuse_stop();
}

TEST_F(FuseTest, InitDoubleInit) {
    fuse_stat_t s1 = fuse_init(g_module_mem, kModuleMemSize,
                               g_log_mem, kLogMemSize, &kTestHal);
    ASSERT_EQ(s1, FUSE_SUCCESS);

    fuse_stat_t s2 = fuse_init(g_module_mem, kModuleMemSize,
                               g_log_mem, kLogMemSize, &kTestHal);
    EXPECT_EQ(s2, FUSE_ERR_ALREADY_INITIALIZED);

    (void)fuse_stop();
}

/* -------------------------------------------------------------------------
 * fuse_stop / fuse_restart tests
 * ---------------------------------------------------------------------- */

TEST_F(FuseTest, StopBeforeInit) {
    /* g_ctx is zeroed by SetUp; stop without init should fail. */
    fuse_stat_t s = fuse_stop();
    EXPECT_EQ(s, FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(FuseTest, RestartBeforeInit) {
    fuse_stat_t s = fuse_restart();
    EXPECT_EQ(s, FUSE_ERR_NOT_INITIALIZED);
}

TEST_F(FuseTest, StopAndRestart) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);
    EXPECT_EQ(fuse_stop(),    FUSE_SUCCESS);
    EXPECT_EQ(fuse_restart(), FUSE_SUCCESS);
    EXPECT_EQ(fuse_stop(),    FUSE_SUCCESS);
}

/* -------------------------------------------------------------------------
 * fuse_module_load argument validation (no real WASM binary)
 * ---------------------------------------------------------------------- */

TEST_F(FuseTest, LoadNullBuf) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_policy_t   policy = {FUSE_CAP_LOG, 1u, 4096u, 4096u, 0u};
    fuse_module_id_t id    = FUSE_INVALID_MODULE_ID;

    fuse_stat_t s = fuse_module_load(nullptr, 64u, &policy, &id);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);

    (void)fuse_stop();
}

TEST_F(FuseTest, LoadNullPolicy) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    static const uint8_t kDummyBuf[4] = {0};
    fuse_module_id_t id = FUSE_INVALID_MODULE_ID;

    fuse_stat_t s = fuse_module_load(kDummyBuf, sizeof(kDummyBuf),
                                     nullptr, &id);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);

    (void)fuse_stop();
}

TEST_F(FuseTest, LoadNullOutId) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    static const uint8_t kDummyBuf[4] = {0};
    fuse_policy_t policy = {FUSE_CAP_LOG, 1u, 4096u, 4096u, 0u};

    fuse_stat_t s = fuse_module_load(kDummyBuf, sizeof(kDummyBuf),
                                     &policy, nullptr);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);

    (void)fuse_stop();
}

TEST_F(FuseTest, LoadInvalidBinaryFails) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    /* A four-byte invalid "binary" should fail WAMR load. */
    static const uint8_t kBadBuf[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    fuse_policy_t    policy = {FUSE_CAP_LOG, 1u, 4096u, 4096u, 0u};
    fuse_module_id_t id     = FUSE_INVALID_MODULE_ID;

    fuse_stat_t s = fuse_module_load(kBadBuf, sizeof(kBadBuf), &policy, &id);
    EXPECT_EQ(s, FUSE_ERR_MODULE_LOAD_FAILED);

    (void)fuse_stop();
}

/* -------------------------------------------------------------------------
 * fuse_module_stat / pause / start — invalid ID path
 * ---------------------------------------------------------------------- */

TEST_F(FuseTest, StatInvalidId) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_module_state_t state = FUSE_MODULE_STATE_UNLOADED;
    fuse_stat_t s = fuse_module_stat(FUSE_INVALID_MODULE_ID, &state);
    EXPECT_EQ(s, FUSE_ERR_MODULE_NOT_FOUND);

    (void)fuse_stop();
}

TEST_F(FuseTest, StatNullOutState) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_stat_t s = fuse_module_stat(0u, nullptr);
    EXPECT_EQ(s, FUSE_ERR_INVALID_ARG);

    (void)fuse_stop();
}

TEST_F(FuseTest, PauseInvalidId) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_stat_t s = fuse_module_pause(FUSE_INVALID_MODULE_ID);
    EXPECT_EQ(s, FUSE_ERR_MODULE_NOT_FOUND);

    (void)fuse_stop();
}

TEST_F(FuseTest, StartInvalidId) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_stat_t s = fuse_module_start(FUSE_INVALID_MODULE_ID);
    EXPECT_EQ(s, FUSE_ERR_MODULE_NOT_FOUND);

    (void)fuse_stop();
}

TEST_F(FuseTest, UnloadInvalidId) {
    ASSERT_EQ(fuse_init(g_module_mem, kModuleMemSize,
                        g_log_mem, kLogMemSize, &kTestHal), FUSE_SUCCESS);

    fuse_stat_t s = fuse_module_unload(FUSE_INVALID_MODULE_ID);
    EXPECT_EQ(s, FUSE_ERR_MODULE_NOT_FOUND);

    (void)fuse_stop();
}
