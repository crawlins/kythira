# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger module's composition roots (.kiro/specs/verifiable-ledger/).
# ledger_node is a placeholder until task 7 makes it the daemon; ledger_ctl
# follows in task 10. Like the other module daemons they run from the build
# tree and are in no install target.

add_executable(ledger_node ledger_node/main.cpp)
target_link_libraries(ledger_node PRIVATE kythira_ledger)
target_compile_features(ledger_node PRIVATE cxx_std_23)
set_target_properties(ledger_node PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
