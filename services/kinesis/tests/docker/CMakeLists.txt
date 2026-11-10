# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Kinesis module's container scenario
# (.kiro/specs/kinesis-compatible-streams/ task 7): three
# kinesis_gateway_node containers from compose.yml with the AWS CLI v2 and a
# boto3 client, driven by kinesis_node_scenario_test. Like every container
# scenario it is not registered in CTest: it needs Docker or rootless Podman
# at run time and is run through its custom target,
# docker-kinesis-node-tests.

if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "Kinesis container scenario skipped: requires Boost and httplib")
    return()
endif()

include(kythira_test_helpers)
kythira_detect_container_runtime()

find_package(Threads REQUIRED)

add_docker_chaos_scenario_test(kinesis_node_scenario_test
    "${CMAKE_CURRENT_SOURCE_DIR}/kinesis_node_scenario_test.cpp")
set_target_properties(kinesis_node_scenario_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/docker_chaos")

set(_KINESIS_DOCKER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../docker")
set(_KINESIS_NODE_DOCKERFILE "${_KINESIS_DOCKER_DIR}/kinesis_gateway_node/Dockerfile")
set(_KINESIS_NODE_DIST "${_KINESIS_DOCKER_DIR}/kinesis_gateway_node/dist")
set(_KINESIS_CLIENTS_DOCKERFILE "${_KINESIS_DOCKER_DIR}/kinesis_clients/Dockerfile")
set(_KINESIS_KCL_DOCKERFILE "${_KINESIS_DOCKER_DIR}/kinesis_kcl_sample/Dockerfile")
set(_KINESIS_COMPOSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/compose.yml")

if(_RUNTIME_EXE)
    # The node image is staged from the host build, as the IAM node's is.
    # Not a coverage build: an instrumented binary in a container writes its
    # counters nowhere useful.
    if(NOT ENABLE_COVERAGE AND TARGET kinesis_gateway_node)
        # The tiering case's S3 mock rides in the node image when the build
        # has it (top-level CMakeLists.txt adds cmd/chaos_s3_mock before
        # services/). Without it the stale copy is removed, so the image
        # never carries a mock from an older build, and the case fails
        # naming the missing binary.
        if(TARGET chaos_s3_mock)
            set(_KINESIS_S3_MOCK_DEP chaos_s3_mock)
            set(_KINESIS_S3_MOCK_STAGE ${CMAKE_COMMAND} -E copy
                "$<TARGET_FILE:chaos_s3_mock>" "${_KINESIS_NODE_DIST}/chaos_s3_mock")
        else()
            set(_KINESIS_S3_MOCK_DEP)
            set(_KINESIS_S3_MOCK_STAGE ${CMAKE_COMMAND} -E rm -f
                "${_KINESIS_NODE_DIST}/chaos_s3_mock")
        endif()
        add_custom_target(docker-kinesis-node-image
            DEPENDS kinesis_gateway_node ${_KINESIS_S3_MOCK_DEP}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_KINESIS_NODE_DIST}"
            COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:kinesis_gateway_node>"
                    "${_KINESIS_NODE_DIST}/kinesis_gateway_node"
            COMMAND ${_KINESIS_S3_MOCK_STAGE}
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_KINESIS_NODE_DOCKERFILE}"
                    --tag kythira-kinesis-node:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_KINESIS_CLIENTS_DOCKERFILE}"
                    --tag kythira-kinesis-clients:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            # The KCL sample workers (Java, built inside the image).
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_KINESIS_KCL_DOCKERFILE}"
                    --tag kythira-kinesis-kcl:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging kinesis_gateway_node (and chaos_s3_mock) and building the kythira-kinesis-node, -clients and -kcl :dev images"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-kinesis-node-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-kinesis-node-image' requires kinesis_gateway_node (a future backend, Folly, Boost and OpenSSL, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-kinesis-node-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_KINESIS_COMPOSE_FILE=${_KINESIS_COMPOSE_FILE}
                $<TARGET_FILE:kinesis_node_scenario_test>
                --log_level=message
        DEPENDS docker-kinesis-node-image kinesis_node_scenario_test
        COMMENT "Running the Kinesis cluster container scenario (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )
    message(STATUS "Container runtime: docker-kinesis-node-image, docker-kinesis-node-tests targets available")
else()
    foreach(_t docker-kinesis-node-image docker-kinesis-node-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
