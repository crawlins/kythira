# Copyright (c) 2026 Clark Rawlins
# SPDX-License-Identifier: Apache-2.0

# The Redis module's container scenarios and images. Moved here from
# tests/docker_chaos/CMakeLists.txt (.kiro/specs/redis-gateway-module/
# task 4) with every target name unchanged. Like the scenarios there, none is
# registered in CTest: each needs Docker or rootless Podman at run time and is
# run through its custom target.

if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "redis container scenarios skipped: requires Boost and httplib")
    return()
endif()

include(kythira_test_helpers)
kythira_detect_container_runtime()

# As in tests/docker_chaos/: checkers/liveness.hpp scales its bound by it.
if(KYTHIRA_TEST_TIMEOUT_SCALE GREATER 1)
    add_compile_definitions(KYTHIRA_TEST_TIMEOUT_SCALE=${KYTHIRA_TEST_TIMEOUT_SCALE})
endif()

find_package(Threads REQUIRED)

# The real-sccache acceptance tests (.kiro/specs/redis-compatible-kv/ tasks 12
# and 13). The test speaks RESP to the gateways directly for its readiness
# probe and wire-level checks, which is the one reason it needs the module's
# headers (resp_protocol.hpp).
add_docker_chaos_scenario_test(docker_sccache_e2e_test
    "${CMAKE_CURRENT_SOURCE_DIR}/sccache_e2e_test.cpp")
target_include_directories(docker_sccache_e2e_test PRIVATE "${KYTHIRA_REDIS_INCLUDE_DIR}")
# The disk-backed build cache in containers
# (.kiro/specs/disk-backed-build-cache/ task 8): same RESP framing need.
add_docker_chaos_scenario_test(docker_cache_cluster_test
    "${CMAKE_CURRENT_SOURCE_DIR}/cache_cluster_test.cpp")
target_include_directories(docker_cache_cluster_test PRIVATE "${KYTHIRA_REDIS_INCLUDE_DIR}")
# Cross-shard transactions through the gateway under faults
# (.kiro/specs/multi-raft-transactions/ task 15): same RESP framing need.
add_docker_chaos_scenario_test(docker_txn_bank_chaos_test
    "${CMAKE_CURRENT_SOURCE_DIR}/txn_bank_chaos_test.cpp")
target_include_directories(docker_txn_bank_chaos_test PRIVATE "${KYTHIRA_REDIS_INCLUDE_DIR}")
# Where they were built before the move.
set_target_properties(docker_sccache_e2e_test docker_cache_cluster_test docker_txn_bank_chaos_test
    PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/docker_chaos")

if(_RUNTIME_EXE)
    set(_REDIS_GATEWAY_DOCKERFILE        "${KYTHIRA_REDIS_DOCKER_DIR}/redis_gateway_node/Dockerfile")
    set(_SCCACHE_RUNNER_DOCKERFILE       "${KYTHIRA_REDIS_DOCKER_DIR}/sccache_runner/Dockerfile")
    set(_SCCACHE_E2E_COMPOSE_FILE        "${KYTHIRA_REDIS_DOCKER_DIR}/sccache-e2e-compose.yml")
    set(_CACHE_CLUSTER_DOCKERFILE        "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/Dockerfile")
    set(_CACHE_CLUSTER_COMPOSE_FILE      "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/compose.yml")
    set(_CACHE_ROLLING_UPGRADE_FILE      "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/rolling-upgrade.yml")
    set(_CACHE_CLIENT_DOCKERFILE         "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/client/Dockerfile")
    set(_TXN_CHAOS_DOCKERFILE            "${KYTHIRA_REDIS_DOCKER_DIR}/txn-chaos/Dockerfile")
    set(_TXN_CHAOS_COMPOSE_FILE          "${KYTHIRA_REDIS_DOCKER_DIR}/txn-chaos-compose.yml")

    # The redis-compatible-kv acceptance tests (tasks 12 and 13). Two images:
    # the gateway daemon, staged from the host build exactly like
    # docker-chaos-image (and guarded exactly like the daemon's inclusion in
    # services/redis/CMakeLists.txt, for the same reason), and
    # the sccache runner, which is upstream sccache plus a Rust toolchain and
    # needs nothing from this build. The module is entered only with the
    # feature on, so a CONFIG_REDIS_GATEWAY=n configure defines none of these
    # targets.
    if(NOT ENABLE_COVERAGE AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND folly_FOUND)
        add_custom_target(docker-redis-gateway-image
            DEPENDS redis_gateway_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${KYTHIRA_REDIS_DOCKER_DIR}/redis_gateway_node/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:redis_gateway_node>"
                    "${KYTHIRA_REDIS_DOCKER_DIR}/redis_gateway_node/dist/redis_gateway_node"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_REDIS_GATEWAY_DOCKERFILE}"
                    --tag kythira-redis-gateway:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging redis_gateway_node binary and building kythira-redis-gateway:dev image"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-redis-gateway-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-redis-gateway-image' requires redis_gateway_node to be buildable (folly + a future backend, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-sccache-runner-image
        COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                --file "${_SCCACHE_RUNNER_DOCKERFILE}"
                --tag kythira-sccache-runner:dev
                "${KYTHIRA_IMAGE_CONTEXT}"
        COMMENT "Building kythira-sccache-runner:dev image (upstream sccache + rust toolchain)"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    add_custom_target(docker-sccache-e2e-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_SCCACHE_E2E_COMPOSE_FILE=${_SCCACHE_E2E_COMPOSE_FILE}
                $<TARGET_FILE:docker_sccache_e2e_test>
                --log_level=message
        DEPENDS docker-redis-gateway-image docker-sccache-runner-image docker_sccache_e2e_test
        COMMENT "Running real-sccache acceptance tests against the redis gateway (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    # The disk-backed build cache (.kiro/specs/disk-backed-build-cache/
    # task 8): the same daemon in its default file mode, as uid 10001
    # with a /data volume. Staged and guarded like the gateway image.
    if(NOT ENABLE_COVERAGE AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND folly_FOUND)
        add_custom_target(docker-cache-cluster-image
            DEPENDS redis_gateway_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:redis_gateway_node>"
                    "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/dist/redis_gateway_node"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_CACHE_CLUSTER_DOCKERFILE}"
                    --tag kythira-cache-cluster:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging redis_gateway_node binary and building kythira-cache-cluster:dev image"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-cache-cluster-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-cache-cluster-image' requires redis_gateway_node to be buildable (folly + a future backend, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    # Task 9's CI job: upstream sccache 0.17.0 and vcpkg at the pinned
    # baseline, nothing from this project. Needs the internet to build
    # (GitHub), none to run.
    add_custom_target(docker-cache-client-image
        COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                --file "${_CACHE_CLIENT_DOCKERFILE}"
                --tag kythira-cache-client:dev
                "${KYTHIRA_IMAGE_CONTEXT}"
        COMMENT "Building kythira-cache-client:dev image (upstream sccache + vcpkg)"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    add_custom_target(docker-cache-cluster-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_CACHE_CLUSTER_COMPOSE_FILE=${_CACHE_CLUSTER_COMPOSE_FILE}
                $<TARGET_FILE:docker_cache_cluster_test>
                --log_level=message
        DEPENDS docker-cache-cluster-image docker-cache-client-image docker_cache_cluster_test
        COMMENT "Running the disk-backed cache cluster container tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    # Rolling upgrade (.kiro/specs/redis-gateway-module/ task 7): a
    # cluster that mixes the gateway built from an older commit with the
    # one from this tree. The old binary is built out of tree (CI checks
    # out the last pre-move commit into a worktree) and handed in here;
    # it goes into the same Dockerfile so only the binary differs.
    set(KYTHIRA_REDIS_OLD_GATEWAY_BINARY "" CACHE FILEPATH
        "redis_gateway_node built from an older commit, for docker-cache-rolling-upgrade-tests")
    if(KYTHIRA_REDIS_OLD_GATEWAY_BINARY)
        add_custom_target(docker-cache-cluster-old-image
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/dist-old"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "${KYTHIRA_REDIS_OLD_GATEWAY_BINARY}"
                    "${KYTHIRA_REDIS_DOCKER_DIR}/cache-cluster/dist-old/redis_gateway_node"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_CACHE_CLUSTER_DOCKERFILE}"
                    --build-arg GATEWAY_BINARY=services/redis/docker/cache-cluster/dist-old/redis_gateway_node
                    --tag kythira-cache-cluster:old
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging the older redis_gateway_node and building kythira-cache-cluster:old image"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-cache-cluster-old-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-cache-cluster-old-image' needs -DKYTHIRA_REDIS_OLD_GATEWAY_BINARY=<redis_gateway_node from an older commit>"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-cache-rolling-upgrade-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_CACHE_CLUSTER_COMPOSE_FILE=${_CACHE_CLUSTER_COMPOSE_FILE}
                KYTHIRA_CACHE_CLUSTER_COMPOSE_OVERRIDE=${_CACHE_ROLLING_UPGRADE_FILE}
                $<TARGET_FILE:docker_cache_cluster_test>
                --run_test=rolling_upgrade
                --log_level=message
        DEPENDS docker-cache-cluster-old-image docker-cache-cluster-image
                docker-cache-client-image docker_cache_cluster_test
        COMMENT "Running the cache cluster rolling-upgrade test, old and new gateway builds mixed (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    # Transactions under faults (.kiro/specs/multi-raft-transactions/
    # task 15): the daemon's snapshot-isolation variant, built whatever
    # the Kconfig selects for redis_gateway_node itself.
    if(NOT ENABLE_COVERAGE AND KYTHIRA_FUTURE_BACKEND_AVAILABLE AND folly_FOUND)
        add_custom_target(docker-txn-chaos-image
            DEPENDS redis_gateway_node_snapshot
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${KYTHIRA_REDIS_DOCKER_DIR}/txn-chaos/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:redis_gateway_node_snapshot>"
                    "${KYTHIRA_REDIS_DOCKER_DIR}/txn-chaos/dist/redis_gateway_node"
            COMMAND ${_RUNTIME_EXE} build ${_IMAGE_FORMAT_ARGS}
                    --file "${_TXN_CHAOS_DOCKERFILE}"
                    --tag kythira-txn-chaos:dev
                    "${KYTHIRA_IMAGE_CONTEXT}"
            COMMENT "Staging redis_gateway_node_snapshot and building kythira-txn-chaos:dev image"
            WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
        )
    else()
        add_custom_target(docker-txn-chaos-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-txn-chaos-image' requires redis_gateway_node_snapshot to be buildable (folly + a future backend, and not a coverage build)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    add_custom_target(docker-txn-chaos-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_TXN_CHAOS_COMPOSE_FILE=${_TXN_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_txn_bank_chaos_test>
                --log_level=message
        DEPENDS docker-txn-chaos-image docker_txn_bank_chaos_test
        COMMENT "Running the transaction bank chaos container test (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${KYTHIRA_IMAGE_CONTEXT}"
    )

    message(STATUS
        "Container runtime: docker-redis-gateway-image, docker-sccache-runner-image, "
        "docker-sccache-e2e-tests, docker-cache-cluster-image, docker-cache-client-image, "
        "docker-cache-cluster-tests, docker-cache-cluster-old-image, "
        "docker-cache-rolling-upgrade-tests, docker-txn-chaos-image, docker-txn-chaos-tests "
        "targets available")
else()
    foreach(_t
        docker-redis-gateway-image docker-sccache-runner-image docker-sccache-e2e-tests
        docker-cache-cluster-image docker-cache-client-image docker-cache-cluster-tests
        docker-cache-cluster-old-image docker-cache-rolling-upgrade-tests
        docker-txn-chaos-image docker-txn-chaos-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
endif()
