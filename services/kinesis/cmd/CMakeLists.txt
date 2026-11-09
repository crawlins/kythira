# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis module's composition root
# (.kiro/specs/kinesis-compatible-streams/). kinesis_gateway_node is a
# placeholder until task 6 makes it the daemon. Like the other module daemons
# it runs from the build tree and is in no install target.

add_executable(kinesis_gateway_node kinesis_gateway_node/main.cpp)
target_link_libraries(kinesis_gateway_node PRIVATE kythira_kinesis)
target_compile_features(kinesis_gateway_node PRIVATE cxx_std_23)
set_target_properties(kinesis_gateway_node PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
