# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The ledger module's container scenario (.kiro/specs/verifiable-ledger/
# task 11, Requirement 12.5): three ledger_node members from
# services/ledger/docker/ledger-chaos-compose.yml under kills and
# partitions, driven by ledger_chaos_test (docker-ledger-chaos-tests).
#
# Like every container scenario it is not registered in CTest: it needs
# Docker or rootless Podman at run time and is run through its custom
# target.

if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "ledger container scenario skipped: requires Boost and httplib")
    return()
endif()

include(kythira_test_helpers)
kythira_detect_container_runtime()

find_package(Threads REQUIRED)

# The scenario writes the test PKI with certificate_authority and drives
# the cluster through the ledger client, whose transport needs OpenSSL and
# the future backend certificate_authority carries.
add_docker_chaos_scenario_test(ledger_chaos_test
    "${CMAKE_CURRENT_SOURCE_DIR}/ledger_chaos_test.cpp")
target_link_libraries(ledger_chaos_test PRIVATE kythira_ledger certificate_authority OpenSSL::SSL)
target_compile_definitions(ledger_chaos_test PRIVATE
    KYTHIRA_LEDGER_CTL="$<TARGET_FILE:ledger_ctl>")
add_dependencies(ledger_chaos_test ledger_ctl)
set_target_properties(ledger_chaos_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/docker_chaos")

set(_LEDGER_CHAOS_DOCKERFILE   "${KYTHIRA_LEDGER_DOCKER_DIR}/ledger-chaos/Dockerfile")
set(_LEDGER_CHAOS_DIST         "${KYTHIRA_LEDGER_DOCKER_DIR}/ledger-chaos/dist")
set(_LEDGER_CHAOS_COMPOSE_FILE "${KYTHIRA_LEDGER_DOCKER_DIR}/ledger-chaos-compose.yml")

if(_RUNTIME_EXE)
    # Staged from the host build, as the Redis engine chaos image is. Not
    # a coverage build: an instrumented binary in a container writes its
    # counters nowhere useful.
    if(NOT ENABLE_COVERAGE)
        # Shared libraries the binaries take from the build's own trees
        # (ion-c is shared in the Ion job's vcpkg tree) go into dist/lib and
        # from there into the image; see stage_runtime_libs.cmake. The roots
        # are the build directory, vcpkg's installed tree and the prefix
        # path, never a system directory.
        set(_LEDGER_CHAOS_LIB_ROOTS "${CMAKE_BINARY_DIR}")
        foreach(_p IN LISTS VCPKG_INSTALLED_DIR CMAKE_PREFIX_PATH)
            if(_p AND IS_DIRECTORY "${_p}" AND NOT _p MATCHES "^/(usr|lib|lib64)?/?$")
                list(APPEND _LEDGER_CHAOS_LIB_ROOTS "${_p}")
            endif()
        endforeach()
        string(REPLACE ";" "|" _LEDGER_CHAOS_LIB_ROOTS_ARG "${_LEDGER_CHAOS_LIB_ROOTS}")
        add_custom_target(docker-ledger-chaos-image
            DEPENDS ledger_node ledger_ctl
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_LEDGER_CHAOS_DIST}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ledger_node>"
                    "${_LEDGER_CHAOS_DIST}/ledger_node"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:ledger_ctl>"
                    "${_LEDGER_CHAOS_DIST}/ledger_ctl"
            COMMAND ${CMAKE_COMMAND}
                    "-DDEST=${_LEDGER_CHAOS_DIST}/lib"
                    "-DROOTS=${_LEDGER_CHAOS_LIB_ROOTS_ARG}"
                    "-DEXECUTABLES=$<TARGET_FILE:ledger_node>|$<TARGET_FILE:ledger_ctl>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/stage_runtime_libs.cmake"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_LEDGER_CHAOS_DOCKERFILE}"
                    --tag kythira-ledger-chaos:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging ledger_node, ledger_ctl and their build-tree libraries and building kythira-ledger-chaos:dev"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
            # VERBATIM so the '|' list separators reach the script quoted;
            # without it Ninja's /bin/sh reads them as pipes.
            VERBATIM
        )
    else()
        add_custom_target(docker-ledger-chaos-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-ledger-chaos-image' is not built in a coverage build"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-ledger-chaos-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_LEDGER_CHAOS_COMPOSE_FILE=${_LEDGER_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:ledger_chaos_test>
                --log_level=message
        DEPENDS docker-ledger-chaos-image ledger_chaos_test
        COMMENT "Running the ledger chaos container scenario (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )
    message(STATUS "Container runtime: docker-ledger-chaos-image, docker-ledger-chaos-tests targets available")
else()
    foreach(_t docker-ledger-chaos-image docker-ledger-chaos-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
