# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger module's composition roots (.kiro/specs/verifiable-ledger/).
# ledger_node is a placeholder until task 7 makes it the daemon; ledger_ctl
# is the offline verification tool (task 10). Like the other module daemons they run from the build
# tree and are in no install target.

add_executable(ledger_node ledger_node/main.cpp)
target_link_libraries(ledger_node PRIVATE kythira_ledger)
target_compile_features(ledger_node PRIVATE cxx_std_23)
set_target_properties(ledger_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# ledger_ctl: verify and prove (task 10), offline or through a node's API.
# `verify --api` uses the ledger client (Boost.Beast and OpenSSL), whose
# API types pull in the core's future backend; as for iam_node,
# certificate_authority carries that (through network_simulator), so the
# mode is built only where that target exists.
add_executable(ledger_ctl ledger_ctl/main.cpp)
target_link_libraries(ledger_ctl PRIVATE kythira_ledger)
if(TARGET certificate_authority)
    find_package(Threads REQUIRED)
    target_link_libraries(ledger_ctl PRIVATE certificate_authority OpenSSL::SSL Threads::Threads)
    target_compile_definitions(ledger_ctl PRIVATE KYTHIRA_LEDGER_CTL_API=1)
endif()
target_compile_features(ledger_ctl PRIVATE cxx_std_23)
set_target_properties(ledger_ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
