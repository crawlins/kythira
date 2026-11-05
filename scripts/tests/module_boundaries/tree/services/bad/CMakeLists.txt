# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

add_library(bad_lib INTERFACE)
# rule cmake-source-dir
target_include_directories(bad_lib INTERFACE ${CMAKE_SOURCE_DIR}/tests)
# rule cmake-escape
target_include_directories(bad_lib INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
target_include_directories(bad_lib INTERFACE ${CMAKE_SOURCE_DIR}/services/bad/include)
