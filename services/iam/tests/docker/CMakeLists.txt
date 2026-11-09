# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The IAM module's container scenarios (.kiro/specs/iam-compatible-auth/):
#
#   - task 9: three iam_node voters and a read replica from compose.yml,
#     driven by iam_node_scenario_test (docker-iam-node-tests);
#   - task 23: the conformance stack of conformance.yml (IAM voters, the
#     Kythira CA, two Redis gateways with IAM replicas, a test OIDC issuer),
#     driven by iam_conformance_test with boto3 and the AWS CLI v2
#     (docker-iam-conformance-tests).
#
# Like every container scenario they are not registered in CTest: they need
# Docker or rootless Podman at run time and are run through their custom
# targets.

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
# Task 19: the scenario signs web identity tokens with raft/jwt.hpp.
target_link_libraries(iam_node_scenario_test PRIVATE kythira_core_headers OpenSSL::Crypto)

add_docker_chaos_scenario_test(iam_conformance_test
    "${CMAKE_CURRENT_SOURCE_DIR}/iam_conformance_test.cpp")
set_target_properties(iam_conformance_test PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/docker_chaos")

set(_IAM_DOCKER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../docker")
set(_IAM_NODE_DOCKERFILE "${_IAM_DOCKER_DIR}/iam_node/Dockerfile")
set(_IAM_NODE_DIST "${_IAM_DOCKER_DIR}/iam_node/dist")
set(_IAM_COMPOSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/compose.yml")
set(_IAM_CONF_DOCKERFILE "${_IAM_DOCKER_DIR}/conformance/Dockerfile")
set(_IAM_CONF_DIST "${_IAM_DOCKER_DIR}/conformance/dist")
set(_IAM_CONF_COMPOSE_FILE "${CMAKE_CURRENT_SOURCE_DIR}/conformance.yml")
set(_IAM_CONF_DRIVER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/conformance")

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

    # Task 23's image adds the CA and the IAM-enabled Redis gateway, so it
    # needs Folly (redis_gateway_node) and CONFIG_REDIS_GATEWAY_IAM. The
    # driver image is pip-installed boto3 from conformance/Dockerfile.
    set(_IAM_CONF_BINS iam_node kythira-iam iam_test_certs ca_cluster_node redis_gateway_node)
    set(_IAM_CONF_OK TRUE)
    foreach(_b IN LISTS _IAM_CONF_BINS)
        if(NOT TARGET ${_b})
            set(_IAM_CONF_OK FALSE)
        endif()
    endforeach()
    if(NOT KCONFIG_REDIS_GATEWAY_IAM OR ENABLE_COVERAGE)
        set(_IAM_CONF_OK FALSE)
    endif()
    if(_IAM_CONF_OK)
        set(_IAM_CONF_COPIES)
        foreach(_b IN LISTS _IAM_CONF_BINS)
            list(APPEND _IAM_CONF_COPIES
                COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${_b}>" "${_IAM_CONF_DIST}/${_b}")
        endforeach()
        add_custom_target(docker-iam-conformance-image
            DEPENDS ${_IAM_CONF_BINS}
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_IAM_CONF_DIST}"
            ${_IAM_CONF_COPIES}
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_IAM_CONF_DOCKERFILE}"
                    --tag kythira-iam-conformance:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --tag kythira-iam-conformance-driver:dev
                    "${_IAM_CONF_DRIVER_DIR}"
            COMMENT "Building kythira-iam-conformance:dev and kythira-iam-conformance-driver:dev"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-iam-conformance-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-iam-conformance-image' requires ${_IAM_CONF_BINS} with CONFIG_REDIS_GATEWAY_IAM (Folly, a future backend, the certificate_authority target, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-iam-conformance-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_IAM_CONFORMANCE_COMPOSE_FILE=${_IAM_CONF_COMPOSE_FILE}
                $<TARGET_FILE:iam_conformance_test>
                --log_level=message
        DEPENDS docker-iam-conformance-image iam_conformance_test
        COMMENT "Running the IAM conformance job (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )
    message(STATUS "Container runtime: docker-iam-node-image, docker-iam-node-tests, docker-iam-conformance-image, docker-iam-conformance-tests targets available")
else()
    foreach(_t docker-iam-node-image docker-iam-node-tests
               docker-iam-conformance-image docker-iam-conformance-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
