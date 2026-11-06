// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_gateway_disk_store_integration_test.cpp
/// @brief Every case of redis_gateway_integration_test.cpp, over the
///        disk-backed `cache_kv_state_machine` instead of
///        `redis_kv_state_machine` (.kiro/specs/disk-backed-build-cache/
///        Requirement 2.7).
///
/// A source file rather than a `target_compile_definitions`, so the target
/// can keep reusing the shared precompiled header: clang refuses a PCH whose
/// command-line macros differ from the TU's.

#define KYTHIRA_GATEWAY_TEST_DISK_STORE 1
#include "redis_gateway_integration_test.cpp"  // NOLINT(bugprone-suspicious-include)
