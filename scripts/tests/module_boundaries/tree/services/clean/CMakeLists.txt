# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

add_library(kythira_clean INTERFACE)
target_include_directories(kythira_clean INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(kythira_clean INTERFACE kythira_core_headers)
add_executable(clean_node cmd/clean_node/main.cpp)
target_include_directories(clean_node PRIVATE "${CMAKE_SOURCE_DIR}/services/clean/cmd/clean_node")
add_raft_completion_test(clean_test tests/clean_test.cpp)
target_link_libraries(clean_test PRIVATE kythira_clean kythira_test_support)
