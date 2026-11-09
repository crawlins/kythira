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

# ledger_ctl: offline verify and prove (task 10).
add_executable(ledger_ctl ledger_ctl/main.cpp)
target_link_libraries(ledger_ctl PRIVATE kythira_ledger)
target_compile_features(ledger_ctl PRIVATE cxx_std_23)
set_target_properties(ledger_ctl PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
