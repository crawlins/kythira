# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The verifiable ledger module: the Ion hash, the Merkle accumulator, and
# later the ledger state machine, ledger_node and ledger_ctl
# (.kiro/specs/verifiable-ledger/, services/README.md).
#
# Entered from the top level only when KYTHIRA_BUILD_LEDGER is on
# (CONFIG_LEDGER, services/ledger/Kconfig, which needs ion-c and OpenSSL),
# after the core targets and tests/, so every core target and helper used
# here already exists. The core targets and variables this file and its
# subdirectories use are listed in README.md.

# The module's headers, as <kythira/ledger/...>. Header-only, like the other
# modules; raft_ion_serializer carries ion-c's link and the DECNUMDIGITS
# definition every includer of ion_decimal.h needs.
set(KYTHIRA_LEDGER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(kythira_ledger INTERFACE)
target_include_directories(kythira_ledger INTERFACE "${KYTHIRA_LEDGER_INCLUDE_DIR}")
target_link_libraries(kythira_ledger INTERFACE
    kythira_core_headers raft_ion_serializer OpenSSL::Crypto)
target_compile_features(kythira_ledger INTERFACE cxx_std_23)

# ledger_node (task 1 placeholder; the daemon is task 7), before the tests.
add_subdirectory(cmd)

# The tests, under the guard the top level enters tests/ with. tests/
# defines kythira_test_pch, which add_raft_completion_test links, so its
# presence is the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
