/*
 * Copyright (c) 2026 FUSE Project. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * fuse_test_helper.cpp — Definitions for shared test infrastructure.
 *
 * This translation unit provides the single-definition-rule (ODR) compliant
 * definitions for:
 *   - g_module_mem / g_log_mem  : shared memory arenas used by all fixtures.
 *   - MockHal::s_instance       : static pointer to the active MockHal object.
 *
 * Every test .cpp file includes fuse_test_helper.h which contains only
 * declarations (extern) for these symbols.
 */

#include "fuse_test_helper.h"

/* Shared memory arenas — declared extern in fuse_test_helper.h. */
uint8_t g_module_mem[kModuleMemSize];
uint8_t g_log_mem[kLogMemSize];

/* MockHal static instance pointer — one definition for the whole binary. */
MockHal *MockHal::s_instance = nullptr;
