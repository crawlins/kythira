# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# Test helpers shared by tests/, tests/docker_chaos/ and the service modules
# under services/ (.kiro/specs/redis-gateway-module/ Requirement 4.4).
#
# Moved here unchanged from tests/CMakeLists.txt and
# tests/docker_chaos/CMakeLists.txt, so a module registers its tests and
# container scenarios exactly as core does, without reaching into core's
# test directories. Include it with
#
#     include(kythira_test_helpers)
#
# (the top-level CMakeLists.txt puts cmake/ on CMAKE_MODULE_PATH). It is
# include()d per directory, not once: functions are global, but the
# variables below are set in the including directory's scope.

# boost_json, resolved the same way the rest of the project does. Read by
# add_docker_chaos_scenario_test from the including directory's scope.
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
    set(_boost_json_lib
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
elseif(TARGET Boost::json)
    set(_boost_json_lib Boost::json)
else()
    set(_boost_json_lib "")
endif()

# The build context every container image uses and the directory every
# container scenario target runs in: the source root, so a Dockerfile COPYs
# and a scenario harness defaults by a repository-relative path (several
# images take files from more than one directory). A service module uses this
# instead of naming the source root itself.
set(KYTHIRA_IMAGE_CONTEXT "${CMAKE_SOURCE_DIR}")

# Helper function for creating Raft completion test executables with
# additional dependencies. Needs tests/'s kythira_test_pch, so it may be
# called only once tests/ has been added.
function(add_raft_completion_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE kythira_test_pch)
    target_precompile_headers(${test_name} REUSE_FROM kythira_test_pch)
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Helper: build one scenario-test executable.
# Not registered in CTest — requires Docker at runtime.
# Run via the docker-chaos-tests custom target.
#
# cmd/chaos_workload is on the include path because chaos_fixture.hpp runs
# the workload from the test process (cairn-chaos-testing task 6).
function(add_docker_chaos_scenario_test test_name source_file)
    add_executable(${test_name} "${source_file}")
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/docker_chaos"
        "${CMAKE_SOURCE_DIR}/cmd/chaos_workload"
    )
    target_link_libraries(${test_name} PRIVATE
        Boost::unit_test_framework
        httplib::httplib
        Threads::Threads
        ${_boost_json_lib}
    )
    target_compile_features(${test_name} PRIVATE cxx_std_23)
endfunction()

# ── Container runtime detection ───────────────────────────────────────────────
#
# CONTAINER_RUNTIME  — CMake cache variable; empty = auto-detect (docker then podman).
#                      Set to "podman" to force Podman.
# COMPOSE_COMMAND    — CMake cache variable; empty = "<runtime> compose" subcommand.
#                      Set to "podman-compose" when using the standalone Python tool.
#
# Both values are forwarded to scenario-test invocations via environment variables
# (KYTHIRA_CONTAINER_RUNTIME / KYTHIRA_COMPOSE_COMMAND) so the test harness can
# call the right binary at runtime without recompiling.
#
# A macro, so that what it sets lands in the caller's scope:
#   _RUNTIME_EXE        the runtime binary (empty: none found, define no image targets)
#   CONTAINER_RUNTIME   its name, when auto-detected
#   _IMAGE_FORMAT_ARGS  extra `build` arguments for the detected runtime
#   _RT_ENV             env-var prefix for every scenario-test invocation
macro(kythira_detect_container_runtime)
    set(CONTAINER_RUNTIME "" CACHE STRING
        "Container runtime to use (docker or podman). Empty = auto-detect.")
    set(COMPOSE_COMMAND "" CACHE STRING
        "Compose command override (e.g. podman-compose). Empty = <CONTAINER_RUNTIME> compose.")

    if(CONTAINER_RUNTIME)
        find_program(_RUNTIME_EXE NAMES "${CONTAINER_RUNTIME}")
    else()
        find_program(_RUNTIME_EXE NAMES docker podman)
        if(_RUNTIME_EXE)
            get_filename_component(CONTAINER_RUNTIME "${_RUNTIME_EXE}" NAME)
        endif()
    endif()

    if(_RUNTIME_EXE)
        # Podman defaults to the OCI image format, which has no HEALTHCHECK
        # field: it warns and hands back an image whose health check is gone, so
        # a readiness gate on it (compose `service_healthy`, or a harness polling
        # .State.Health.Status) waits forever. Every image here carries a
        # HEALTHCHECK, so every Podman build asks for the Docker format. Docker's
        # builder emits that format anyway and does not take the flag. See
        # scripts/build-tier-e-image.sh for the measurement that found this.
        set(_IMAGE_FORMAT_ARGS "")
        if(CONTAINER_RUNTIME STREQUAL "podman")
            set(_IMAGE_FORMAT_ARGS --format docker)
        endif()

        # Env-var prefix injected into every scenario-test invocation.
        set(_RT_ENV
            KYTHIRA_CONTAINER_RUNTIME=${CONTAINER_RUNTIME}
            KYTHIRA_COMPOSE_COMMAND=${COMPOSE_COMMAND}
        )
    endif()
endmacro()

# ── Test-timeout scaling ──────────────────────────────────────────────────────
#
# Applies KYTHIRA_TEST_TIMEOUT_SCALE (a cache variable tests/CMakeLists.txt
# declares, with the reasoning) to the calling directory: the compile
# definition the Boost per-case budgets read, and every registered test's
# CTest TIMEOUT. Call it at the end of a directory that registers tests, after
# the last add_test. Moved unchanged from the end of tests/CMakeLists.txt.
function(kythira_scale_test_timeouts)
    if(KYTHIRA_TEST_TIMEOUT_SCALE GREATER 1)
        add_compile_definitions(KYTHIRA_TEST_TIMEOUT_SCALE=${KYTHIRA_TEST_TIMEOUT_SCALE})
        get_property(_kythira_tests DIRECTORY PROPERTY TESTS)
        foreach(_test IN LISTS _kythira_tests)
            get_test_property(${_test} TIMEOUT _test_timeout)
            if(_test_timeout)
                math(EXPR _scaled "${_test_timeout} * ${KYTHIRA_TEST_TIMEOUT_SCALE}")
                set_tests_properties(${_test} PROPERTIES TIMEOUT ${_scaled})
            endif()
        endforeach()
        message(STATUS
            "Test timeouts scaled by ${KYTHIRA_TEST_TIMEOUT_SCALE}x "
            "(Boost per-case budgets and CTest TIMEOUT properties)")
    endif()
endfunction()
