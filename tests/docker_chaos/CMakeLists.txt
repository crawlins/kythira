if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "docker_chaos tests skipped: requires Boost and httplib")
    return()
endif()

# Resolve boost_json the same way the rest of the project does.
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_json.a")
    set(_boost_json_lib
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-linux/lib/libboost_json.a")
elseif(TARGET Boost::json)
    set(_boost_json_lib Boost::json)
else()
    set(_boost_json_lib "")
endif()

# Helper: build one unit-test executable and register in CTest.
function(add_docker_chaos_unit_test test_name source_file)
    add_executable(${test_name} "${source_file}")
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/docker_chaos"
    )
    target_link_libraries(${test_name} PRIVATE
        Boost::unit_test_framework
        httplib::httplib
        ${_boost_json_lib}
    )
    target_compile_features(${test_name} PRIVATE cxx_std_23)
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "docker_chaos_unit")
endfunction()

# Helper: build one scenario-test executable.
# Not registered in CTest — requires Docker at runtime.
# Run via the docker-chaos-tests custom target.
function(add_docker_chaos_scenario_test test_name source_file)
    add_executable(${test_name} "${source_file}")
    target_include_directories(${test_name} PRIVATE
        "${CMAKE_SOURCE_DIR}/tests/docker_chaos"
    )
    target_link_libraries(${test_name} PRIVATE
        Boost::unit_test_framework
        httplib::httplib
        ${_boost_json_lib}
    )
    target_compile_features(${test_name} PRIVATE cxx_std_23)
endfunction()

# ── Harness unit tests (no Docker required) ───────────────────────────────────
# Tests command-string construction, HTTP response parsing, and ChaosNode/
# ChaosCluster lifecycle logic using injected mocks. Registered in CTest so
# they run as part of the standard `ctest` invocation.

add_docker_chaos_unit_test(docker_chaos_harness_unit_tests
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/harness_unit_test.cpp")

add_docker_chaos_unit_test(docker_chaos_fault_control_unit_tests
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/fault_control_unit_test.cpp")

# ── Docker scenario tests (require Docker at runtime) ─────────────────────────
# Each scenario test is its own executable — Boost.Test embeds BOOST_TEST_MODULE
# in every TU so they cannot share a binary.

set(_scenario_tests
    docker_chaos_smoke_test
    docker_chaos_election_recovery_test
    docker_chaos_crash_recovery_test
    docker_chaos_network_degradation_test
    docker_chaos_az_partition_test
    docker_chaos_persistence_faults_test
    docker_chaos_safety_assertions_test
)

add_docker_chaos_scenario_test(docker_chaos_smoke_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/chaos_smoke_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_election_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/election_recovery_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_crash_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/crash_recovery_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_network_degradation_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/network_degradation_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_az_partition_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/az_partition_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_persistence_faults_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/persistence_faults_test.cpp")
add_docker_chaos_scenario_test(docker_chaos_safety_assertions_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/safety_assertions_test.cpp")

# ── poco_peer_discovery Docker scenario test ──────────────────────────────────

add_docker_chaos_scenario_test(docker_poco_discovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/poco_discovery_test.cpp")

# ── Docker image and scenario-runner custom targets ───────────────────────────

find_program(DOCKER_EXECUTABLE docker)

if(DOCKER_EXECUTABLE)
    set(_CHAOS_COMPOSE_FILE           "${CMAKE_SOURCE_DIR}/docker/docker-compose.yml")
    set(_CHAOS_DOCKERFILE             "${CMAKE_SOURCE_DIR}/docker/chaos_node/Dockerfile")
    set(_POCO_DISCOVERY_COMPOSE_FILE  "${CMAKE_SOURCE_DIR}/docker/poco-discovery-compose.yml")
    set(_POCO_DISCOVERY_DOCKERFILE    "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/Dockerfile")

    add_custom_target(docker-chaos-image
        COMMAND ${DOCKER_EXECUTABLE} build
                --file "${_CHAOS_DOCKERFILE}"
                --tag kythira-chaos-node:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-chaos-node:dev Docker image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-poco-discovery-image
        COMMAND ${DOCKER_EXECUTABLE} build
                --file "${_POCO_DISCOVERY_DOCKERFILE}"
                --tag kythira-poco-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-poco-discovery:dev Docker image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-chaos-tests
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_smoke_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_election_recovery_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_crash_recovery_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_network_degradation_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_az_partition_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_persistence_faults_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_safety_assertions_test>
                --log_level=message
        DEPENDS docker-chaos-image ${_scenario_tests}
        COMMENT "Running Docker chaos scenario tests"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-poco-discovery-tests
        COMMAND ${CMAKE_COMMAND} -E env
                KYTHIRA_POCO_DISCOVERY_COMPOSE_FILE=${_POCO_DISCOVERY_COMPOSE_FILE}
                $<TARGET_FILE:docker_poco_discovery_test>
                --log_level=message
        DEPENDS docker-poco-discovery-image docker_poco_discovery_test
        COMMENT "Running poco_peer_discovery Docker scenario tests"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    message(STATUS "docker-chaos-image, docker-chaos-tests, docker-poco-discovery-image, docker-poco-discovery-tests targets available")
else()
    foreach(_t docker-chaos-image docker-chaos-tests docker-poco-discovery-image docker-poco-discovery-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'${_t}' requires 'docker' on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
    message(STATUS "docker not found — docker-chaos and docker-poco-discovery targets disabled")
endif()
