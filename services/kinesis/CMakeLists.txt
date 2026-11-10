# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis Data Streams-compatible module: the operation table from the
# pinned botocore model, kinesis_gateway_node, and later the front end,
# stream state machine and lease API
# (.kiro/specs/kinesis-compatible-streams/, services/README.md).
#
# Entered from the top level only when KYTHIRA_BUILD_KINESIS is on
# (CONFIG_KINESIS, services/kinesis/Kconfig), after the core targets and
# tests/, so every core target and helper used here already exists. The core
# targets and variables this file and its subdirectories use are listed in
# README.md.

# The module's headers, as <kythira/kinesis/...>. Header-only, like the other
# modules. OpenSSL for SigV4, segment digests and iterator tokens, and its
# TLS half for the peer client (KYTHIRA_BUILD_KINESIS requires it).
set(KYTHIRA_KINESIS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(KYTHIRA_KINESIS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(kythira_kinesis INTERFACE)
target_include_directories(kythira_kinesis INTERFACE "${KYTHIRA_KINESIS_INCLUDE_DIR}")
target_link_libraries(kythira_kinesis INTERFACE kythira_core_headers boost_json OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(kythira_kinesis INTERFACE cxx_std_23)

# kinesis_gateway_node (task 1 placeholder; the daemon is task 6), before
# the tests, which run it.
add_subdirectory(cmd)

# The tests, under the guard the top level enters tests/ with. tests/
# defines kythira_test_pch, which add_raft_completion_test links, so its
# presence is the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
