# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module: the policy language, and later the IAM state machine,
# evaluator, authorizer, iam_node and kythira-iam
# (.kiro/specs/iam-compatible-auth/, services/README.md).
#
# Entered from the top level only when KYTHIRA_BUILD_IAM is on (CONFIG_IAM,
# services/iam/Kconfig), after the core targets and tests/, so every core
# target and helper used here already exists. The core targets and
# variables this file and its subdirectories use are listed in README.md.

# The module's headers, as <kythira/iam/...>.
set(KYTHIRA_IAM_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(kythira_iam INTERFACE)
target_include_directories(kythira_iam INTERFACE "${KYTHIRA_IAM_INCLUDE_DIR}")
target_link_libraries(kythira_iam INTERFACE kythira_core_headers)

# The tests, under the guard the top level enters tests/ with. tests/
# defines kythira_test_pch, which add_raft_completion_test links, so its
# presence is the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
