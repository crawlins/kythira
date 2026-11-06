# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module's container scenario (.kiro/specs/iam-compatible-auth/
# task 9): three iam_node voters and a read replica from compose.yml,
# driven by iam_node_scenario_test. Like every container scenario it is not
# registered in CTest: it needs Docker or rootless Podman at run time and is
# run through its custom target, docker-iam-node-tests.

if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "IAM container scenario skipped: requires Boost and httplib")
    return()
endif()

include(kythira_test_helpers)
kythira_detect_container_runtime()

find_package(Threads REQUIRED)

add_docker_chaos_scenario_test(iam_node_scenario_test
    "${CMAKE_CURRENT_SOURCE_DIR}/iam_node_scenario_test.cpp")
set_target_properties(iam_node_scenario_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/docker_chaos")

set(_IAM_DOCKER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../docker")
set(_IAM_NODE_DOCKERFILE "${_IAM_DOCKER_DIR}/iam_node/Dockerfile")
set(_IAM_NODE_DIST "${_IAM_DOCKER_DIR}/iam_node/dist")
set(_IAM_COMPOSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/compose.yml")

if(_RUNTIME_EXE)
    # The image is staged from the host build, as the Redis gateway's is,
    # and so needs the three binaries buildable here. Not a coverage build:
    # an instrumented binary in a container writes its counters nowhere
    # useful.
    if(NOT ENABLE_COVERAGE AND TARGET iam_node AND TARGET kythira-iam AND TARGET iam_test_certs)
        add_custom_target(docker-iam-node-image
            DEPENDS iam_node kythira-iam iam_test_certs
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_IAM_NODE_DIST}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:iam_node>" "${_IAM_NODE_DIST}/iam_node"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:kythira-iam>"
                    "${_IAM_NODE_DIST}/kythira-iam"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:iam_test_certs>"
                    "${_IAM_NODE_DIST}/iam_test_certs"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_IAM_NODE_DOCKERFILE}"
                    --tag kythira-iam-node:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging iam_node, kythira-iam and iam_test_certs and building kythira-iam-node:dev"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-iam-node-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-iam-node-image' requires iam_node, kythira-iam and iam_test_certs (a future backend and the certificate_authority target, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-iam-node-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_IAM_COMPOSE_FILE=${_IAM_COMPOSE_FILE}
                $<TARGET_FILE:iam_node_scenario_test>
                --log_level=message
        DEPENDS docker-iam-node-image iam_node_scenario_test
        COMMENT "Running the IAM cluster container scenario (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )
    message(STATUS "Container runtime: docker-iam-node-image, docker-iam-node-tests targets available")
else()
    foreach(_t docker-iam-node-image docker-iam-node-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
