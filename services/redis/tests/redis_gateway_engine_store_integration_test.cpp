// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file redis_gateway_engine_store_integration_test.cpp
/// @brief Every case of redis_gateway_integration_test.cpp, over the
///        persistent `persistent_kv_state_machine` instead of
///        `redis_kv_state_machine` (.kiro/specs/persistent-state-machine/
///        task 7, Requirement 9.1).
///
/// A source file rather than a `target_compile_definitions`, so the target
/// can keep reusing the shared precompiled header, as the disk-store variant
/// does.

#define KYTHIRA_GATEWAY_TEST_ENGINE_STORE 1
#include "redis_gateway_integration_test.cpp"  // NOLINT(bugprone-suspicious-include)
