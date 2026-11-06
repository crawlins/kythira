# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module: the policy language, the IAM state machine, iam_node and
# kythira-iam, and later the evaluator and authorizer
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
# OpenSSL for iam_seal.hpp (KYTHIRA_BUILD_IAM requires it).
target_link_libraries(kythira_iam INTERFACE kythira_core_headers OpenSSL::Crypto)

# The pinned AWS-managed policies (tools/aws_managed_policies.json),
# compiled into <kythira/iam/aws_managed_policies_data.hpp> in the build
# tree at configure time. CMAKE_CONFIGURE_DEPENDS reruns the step when the
# JSON or the generator changes; the generator leaves an unchanged header
# alone so a reconfigure does not rebuild everything that includes it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(KYTHIRA_IAM_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_iam_policies_json "${CMAKE_CURRENT_SOURCE_DIR}/tools/aws_managed_policies.json")
set(_iam_policies_gen "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_aws_managed_policies.py")
file(MAKE_DIRECTORY "${KYTHIRA_IAM_GENERATED_DIR}/kythira/iam")
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${_iam_policies_gen}" "${_iam_policies_json}"
            "${KYTHIRA_IAM_GENERATED_DIR}/kythira/iam/aws_managed_policies_data.hpp"
    RESULT_VARIABLE _iam_policies_rc)
if(NOT _iam_policies_rc EQUAL 0)
    message(FATAL_ERROR "IAM: gen_aws_managed_policies.py failed -- see output above")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_iam_policies_json}" "${_iam_policies_gen}")
target_include_directories(kythira_iam INTERFACE "${KYTHIRA_IAM_GENERATED_DIR}")

# iam_node and kythira-iam (task 9), before the tests, which run them.
add_subdirectory(cmd)

# The tests, under the guard the top level enters tests/ with. tests/
# defines kythira_test_pch, which add_raft_completion_test links, so its
# presence is the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
