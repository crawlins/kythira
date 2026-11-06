# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The small CMake project the end-to-end test builds through sccache
# (.kiro/specs/disk-backed-build-cache/ Requirement 12.1). Four library
# translation units and a main, so each clean build is five compiles and a
# second clean build should be five cache hits.
#
# run.sh passes -DKYTHIRA_SALT=<n> in CMAKE_CXX_FLAGS. The flag is part of
# every compile's command line, so a new salt is a new set of cache keys and
# test cases cannot see each other's objects.

cmake_minimum_required(VERSION 3.20)
project(kythira_cache_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(probe STATIC alpha.cpp beta.cpp gamma.cpp delta.cpp)
target_include_directories(probe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(kythira_cache_probe main.cpp)
target_link_libraries(kythira_cache_probe PRIVATE probe)
