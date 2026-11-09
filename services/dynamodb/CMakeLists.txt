# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The DynamoDB-compatible service module: so far the pinned botocore models,
# their generated shape tables, the coverage test and the wire-evidence test
# (.kiro/specs/dynamodb-compatible-api/ task 1, services/README.md).
#
# Entered from the top level only when KYTHIRA_BUILD_DYNAMODB is on
# (CONFIG_DYNAMODB, services/dynamodb/Kconfig), after the core targets and
# tests/, so every core target and helper used here already exists. The core
# targets and variables this file and its subdirectories use are listed in
# README.md.

# The module's headers, as <kythira/dynamodb/...>. Header-only, like the
# other modules.
set(KYTHIRA_DYNAMODB_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(KYTHIRA_DYNAMODB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(kythira_dynamodb INTERFACE)
target_include_directories(kythira_dynamodb INTERFACE "${KYTHIRA_DYNAMODB_INCLUDE_DIR}")
# OpenSSL for SigV4 (front_end.hpp through core's sigv4_verifier).
target_link_libraries(kythira_dynamodb INTERFACE kythira_core_headers boost_json
    OpenSSL::SSL OpenSSL::Crypto)
target_compile_features(kythira_dynamodb INTERFACE cxx_std_23)

# The tests, under the guard the top level enters tests/ with. tests/
# defines kythira_test_pch, which add_raft_completion_test links, so its
# presence is the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
