# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Redis gateway module: the RESP front end, the build cache it serves,
# redis_gateway_node and their tests (.kiro/specs/redis-gateway-module/).
#
# Entered from the top level only when KYTHIRA_BUILD_REDIS_GATEWAY is on
# (CONFIG_REDIS_GATEWAY, services/redis/Kconfig), after the core targets,
# tests/ and tests/docker_chaos/, so every core target and helper used here
# already exists. The core targets and variables this file and its
# subdirectories use are listed in README.md.

# The module's headers, as <kythira/redis/...>.
set(KYTHIRA_REDIS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(kythira_redis INTERFACE)
target_include_directories(kythira_redis INTERFACE "${KYTHIRA_REDIS_INCLUDE_DIR}")
target_link_libraries(kythira_redis INTERFACE kythira_core_headers OpenSSL::SSL OpenSSL::Crypto)

# The images' Dockerfiles, compose files and staging directories. Images
# build from KYTHIRA_IMAGE_CONTEXT (cmake/kythira_test_helpers.cmake), so
# their COPY lines name these paths from the source root.
set(KYTHIRA_REDIS_DOCKER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/docker")

# The daemon. Same guard as when it lived at the top level: not a
# coverage build, and folly plus a future backend (the subdirectory checks
# folly, Boost and OpenSSL itself).
if(NOT ENABLE_COVERAGE AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND folly_FOUND)
    add_subdirectory(cmd/redis_gateway_node)
endif()

# The tests, under the guard the top level enters tests/ with. tests/ defines
# kythira_test_pch, which add_raft_completion_test links, so its presence is
# the check.
if(TARGET kythira_test_pch)
    add_subdirectory(tests)
endif()
