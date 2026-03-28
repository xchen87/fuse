/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_test_helper.h — Shared test infrastructure for the FUSE test suite.
 *
 * Provides:
 *  - Memory buffers and HAL stub/mock wiring shared across all fixtures.
 *  - AotBinary helper that loads a pre-compiled .aot file into a byte vector
 *    and provides a GTEST_SKIP() guard when the file is absent (wamrc not built).
 *  - FuseTestBase: GoogleTest fixture that calls fuse_init() in SetUp() and
 *    fuse_stop() + wasm_runtime_destroy() in TearDown() for clean isolation.
 *  - MockHal: gMock class wrapping all five HAL callbacks as mock methods.
 *    Static C-linkage thunks route FUSE's C callbacks into the global mock.
 */

#ifndef FUSE_TEST_HELPER_H
#define FUSE_TEST_HELPER_H

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <cstdint>
#include <cstring>
#include <vector>
#include <fstream>
#include <string>

extern "C" {
#include "fuse.h"
/* Access WAMR's destroy so each test gets a clean runtime. */
#include "wasm_export.h"
/* Access g_ctx so TearDown can zero it, clearing g_ctx.initialized. */
#include "../core/fuse_internal.h"
}

/* -------------------------------------------------------------------------
 * Fixed-size host memory arenas.
 * All fixtures share these arrays; each test reinitialises them in SetUp().
 * ---------------------------------------------------------------------- */
static constexpr size_t kModuleMemSize = (512u * 1024u);   /* 512 KiB */
static constexpr size_t kLogMemSize    = (32u  * 1024u);   /* 32  KiB */

/* Defined once in fuse_test_helper.cpp (included via CMake OBJECT library),
 * declared here for use across translation units. */
extern uint8_t g_module_mem[kModuleMemSize];
extern uint8_t g_log_mem[kLogMemSize];

/* -------------------------------------------------------------------------
 * MockHal — gMock wrapper for all five HAL callbacks.
 *
 * Usage pattern:
 *   1. Instantiate MockHal as a test member.
 *   2. Call MockHal::Install(&mock) to wire static thunks to this instance.
 *   3. Build fuse_hal_t from MockHal::MakeHal().
 *   4. Pass that hal to fuse_init().
 *   5. Set EXPECT_CALL on mock methods as needed.
 * ---------------------------------------------------------------------- */
class MockHal {
public:
    MOCK_METHOD(float,    TempGetReading,   (), ());
    MOCK_METHOD(uint64_t, TimerGetTimestamp, (), ());
    MOCK_METHOD(uint64_t, CameraLastFrame,  (void *buf, uint32_t max_len), ());
    MOCK_METHOD(void,     QuotaArm,         (fuse_module_id_t id, uint32_t quota_us), ());
    MOCK_METHOD(void,     QuotaCancel,      (fuse_module_id_t id), ());

    /* Install this instance as the target for all static thunks. */
    static void Install(MockHal *instance) { s_instance = instance; }
    static void Uninstall()                { s_instance = nullptr; }

    /* Build a fuse_hal_t that routes all callbacks through this mock. */
    static fuse_hal_t MakeHal() {
        fuse_hal_t h{};
        h.temp_get_reading    = &MockHal::ThunkTemp;
        h.timer_get_timestamp = &MockHal::ThunkTimer;
        h.camera_last_frame   = &MockHal::ThunkCamera;
        h.quota_arm           = &MockHal::ThunkQuotaArm;
        h.quota_cancel        = &MockHal::ThunkQuotaCancel;
        return h;
    }

    /* Build a fuse_hal_t with only the timer hooked (used by log tests). */
    static fuse_hal_t MakeHalTimerOnly() {
        fuse_hal_t h{};
        h.timer_get_timestamp = &MockHal::ThunkTimer;
        return h;
    }

    /* Build a fuse_hal_t with no callbacks at all. */
    static fuse_hal_t MakeHalNull() {
        return fuse_hal_t{};
    }

private:
    static MockHal *s_instance;

    static float    ThunkTemp(void) {
        return s_instance ? s_instance->TempGetReading() : 0.0f;
    }
    static uint64_t ThunkTimer(void) {
        return s_instance ? s_instance->TimerGetTimestamp() : 0u;
    }
    static uint64_t ThunkCamera(void *buf, uint32_t max_len) {
        return s_instance ? s_instance->CameraLastFrame(buf, max_len) : 0u;
    }
    static void ThunkQuotaArm(fuse_module_id_t id, uint32_t quota_us) {
        if (s_instance) s_instance->QuotaArm(id, quota_us);
    }
    static void ThunkQuotaCancel(fuse_module_id_t id) {
        if (s_instance) s_instance->QuotaCancel(id);
    }
};

/* s_instance is defined once in fuse_test_helper.cpp */

/* -------------------------------------------------------------------------
 * AotBinary — RAII loader for a pre-compiled .aot file.
 *
 * If the file does not exist (wamrc not yet built), calling Skip() on the
 * current test via GTEST_SKIP is the caller's responsibility after checking
 * IsAvailable().
 * ---------------------------------------------------------------------- */
class AotBinary {
public:
    explicit AotBinary(const std::string &path) : path_(path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) return;
        std::streamsize sz = f.tellg();
        if (sz <= 0) return;
        f.seekg(0, std::ios::beg);
        data_.resize(static_cast<size_t>(sz));
        if (!f.read(reinterpret_cast<char *>(data_.data()),
                    static_cast<std::streamsize>(data_.size()))) {
            data_.clear();
        }
    }

    bool IsAvailable() const { return !data_.empty(); }

    uint8_t       *Data()  { return data_.data(); }
    uint32_t       Size()  const { return static_cast<uint32_t>(data_.size()); }
    const std::string &Path() const { return path_; }

private:
    std::string          path_;
    std::vector<uint8_t> data_;
};

/* -------------------------------------------------------------------------
 * FuseTestBase — base fixture shared by all test suites.
 *
 * SetUp():
 *   - Zeros both memory arenas.
 *   - Installs a default stub MockHal (no expectations).
 *   - Calls fuse_init().
 *
 * TearDown():
 *   - Calls fuse_stop() (idempotent if already stopped).
 *   - Calls wasm_runtime_destroy() to fully reset WAMR state so the next
 *     fuse_init() can call wasm_runtime_full_init() again without failure.
 *   - Uninstalls the MockHal.
 * ---------------------------------------------------------------------- */
class FuseTestBase : public ::testing::Test {
protected:
    MockHal mock_hal_;

    void SetUp() override {
        std::memset(g_module_mem, 0, sizeof(g_module_mem));
        std::memset(g_log_mem,    0, sizeof(g_log_mem));

        MockHal::Install(&mock_hal_);

        /* Default: timer returns monotonic counter for log timestamps. */
        ON_CALL(mock_hal_, TimerGetTimestamp())
            .WillByDefault(::testing::Return(0u));
        ON_CALL(mock_hal_, TempGetReading())
            .WillByDefault(::testing::Return(25.0f));
        ON_CALL(mock_hal_, CameraLastFrame(::testing::_, ::testing::_))
            .WillByDefault(::testing::Return(0u));

        fuse_hal_t hal = MockHal::MakeHal();
        fuse_stat_t rc = fuse_init(g_module_mem, kModuleMemSize,
                                   g_log_mem, kLogMemSize, &hal);
        ASSERT_EQ(rc, FUSE_SUCCESS) << "fuse_init failed in SetUp()";
    }

    void TearDown() override {
        (void)fuse_stop();
        wasm_runtime_destroy();
        /* Zero g_ctx so the next fuse_init() sees initialized==false.
         * wasm_runtime_destroy() tears down WAMR but does not touch g_ctx. */
        std::memset(&g_ctx, 0, sizeof(g_ctx));
        MockHal::Uninstall();
    }

    /* Load an AOT binary and skip the test gracefully if it is absent.
     *
     * Returns true on successful load; returns false and marks the test
     * skipped when the .aot file does not exist, or marks it failed when
     * fuse_module_load() returns an error.
     *
     * GTEST_SKIP() expands to a statement that prevents any subsequent
     * code from running in the calling test body, so callers must guard
     * with:  if (!LoadAotOrSkip(...)) return;
     */
    bool LoadAotOrSkip(const std::string &aot_path,
                       const fuse_policy_t &policy,
                       fuse_module_id_t *out_id) {
        AotBinary bin(aot_path);
        if (!bin.IsAvailable()) {
            /* Store the skip message; actual skip is triggered by the
             * caller's  if (!...) return;  after GTEST_SKIP sets the flag.
             * We use the two-statement form to avoid the "void value" error
             * that arises when GTEST_SKIP() is used inside a non-void lambda
             * or bool-returning helper in older GTest versions. */
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
        EXPECT_EQ(rc, FUSE_SUCCESS) << "fuse_module_load failed for "
                                    << aot_path;
        return (rc == FUSE_SUCCESS);
    }
};

/* -------------------------------------------------------------------------
 * Convenience: default policy granting all capabilities.
 * ---------------------------------------------------------------------- */
static inline fuse_policy_t MakePolicy(uint32_t caps,
                                       uint32_t pages    = 1u,
                                       uint32_t stack    = 8192u,
                                       uint32_t heap     = 8192u,
                                       uint32_t quota    = 0u,
                                       uint32_t interval = 0u) {
    fuse_policy_t p{};
    p.capabilities      = caps;
    p.memory_pages_max  = pages;
    p.stack_size        = stack;
    p.heap_size         = heap;
    p.cpu_quota_us      = quota;
    p.step_interval_us  = interval;
    return p;
}

/* -------------------------------------------------------------------------
 * CMake places generated .aot files in the test binary directory.
 * This macro builds the absolute path to a named AOT binary.
 * FUSE_AOT_DIR is defined via target_compile_definitions in CMakeLists.txt.
 * ---------------------------------------------------------------------- */
#ifndef FUSE_AOT_DIR
#  define FUSE_AOT_DIR "."
#endif

#define AOT_PATH(name) (std::string(FUSE_AOT_DIR) + "/" + (name))

#endif /* FUSE_TEST_HELPER_H */
