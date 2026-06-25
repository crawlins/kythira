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

# ── Quorum healing Docker scenario tests (Req 19) ────────────────────────────

add_docker_chaos_scenario_test(quorum_healing_tests
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/quorum_healing_test.cpp")

# ── poco_peer_discovery Docker scenario test ──────────────────────────────────

add_docker_chaos_scenario_test(docker_poco_discovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/poco_discovery_test.cpp")

# ── DNS / DNS-SD Docker scenario tests ───────────────────────────────────────

add_docker_chaos_scenario_test(docker_dns_discovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/dns_discovery_test.cpp")

add_docker_chaos_scenario_test(docker_dns_sd_discovery_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/dns_sd_discovery_test.cpp")

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

# ── Image and scenario-runner custom targets ──────────────────────────────────

if(_RUNTIME_EXE)
    set(_CHAOS_COMPOSE_FILE              "${CMAKE_SOURCE_DIR}/docker/docker-compose.yml")
    set(_CHAOS_DOCKERFILE                "${CMAKE_SOURCE_DIR}/docker/chaos_node/Dockerfile")
    set(_QUORUM_COMPOSE_FILE             "${CMAKE_SOURCE_DIR}/docker/docker-compose.quorum.yml")
    set(_POCO_DISCOVERY_COMPOSE_FILE     "${CMAKE_SOURCE_DIR}/docker/poco-discovery-compose.yml")
    set(_POCO_DISCOVERY_DOCKERFILE       "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/Dockerfile")
    set(_BIND9_DOCKERFILE                "${CMAKE_SOURCE_DIR}/docker/bind9/Dockerfile")
    set(_DNS_DISCOVERY_COMPOSE_FILE      "${CMAKE_SOURCE_DIR}/docker/dns-discovery-compose.yml")
    set(_DNS_DISCOVERY_DOCKERFILE        "${CMAKE_SOURCE_DIR}/docker/dns_discovery_node/Dockerfile")
    set(_DNS_SD_DISCOVERY_COMPOSE_FILE   "${CMAKE_SOURCE_DIR}/docker/dns-sd-discovery-compose.yml")
    set(_DNS_SD_DISCOVERY_DOCKERFILE     "${CMAKE_SOURCE_DIR}/docker/dns_sd_discovery_node/Dockerfile")

    # Env-var prefix injected into every scenario-test invocation.
    set(_RT_ENV
        KYTHIRA_CONTAINER_RUNTIME=${CONTAINER_RUNTIME}
        KYTHIRA_COMPOSE_COMMAND=${COMPOSE_COMMAND}
    )

    add_custom_target(docker-chaos-image
        COMMAND ${_RUNTIME_EXE} build
                --file "${_CHAOS_DOCKERFILE}"
                --tag kythira-chaos-node:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-chaos-node:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-poco-discovery-image
        COMMAND ${_RUNTIME_EXE} build
                --file "${_POCO_DISCOVERY_DOCKERFILE}"
                --tag kythira-poco-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-poco-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-chaos-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_smoke_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_election_recovery_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_crash_recovery_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_network_degradation_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_az_partition_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_persistence_faults_test>
                --log_level=message
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_COMPOSE_FILE=${_CHAOS_COMPOSE_FILE}
                $<TARGET_FILE:docker_chaos_safety_assertions_test>
                --log_level=message
        DEPENDS docker-chaos-image ${_scenario_tests}
        COMMENT "Running chaos scenario tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(quorum-healing-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_DOCKER_INTEGRATION_TESTS=1
                KYTHIRA_QUORUM_COMPOSE_FILE=${_QUORUM_COMPOSE_FILE}
                $<TARGET_FILE:quorum_healing_tests>
                --log_level=message
        DEPENDS docker-chaos-image quorum_healing_tests
        COMMENT "Running quorum healing scenario tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-poco-discovery-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_POCO_DISCOVERY_COMPOSE_FILE=${_POCO_DISCOVERY_COMPOSE_FILE}
                $<TARGET_FILE:docker_poco_discovery_test>
                --log_level=message
        DEPENDS docker-poco-discovery-image docker_poco_discovery_test
        COMMENT "Running poco_peer_discovery scenario tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-bind9-image
        COMMAND ${_RUNTIME_EXE} build
                --file "${_BIND9_DOCKERFILE}"
                --tag kythira-bind9:dev
                "${CMAKE_SOURCE_DIR}/docker/bind9"
        COMMENT "Building kythira-bind9:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-dns-discovery-image
        COMMAND ${_RUNTIME_EXE} build
                --file "${_DNS_DISCOVERY_DOCKERFILE}"
                --tag kythira-dns-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-dns-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-dns-sd-discovery-image
        COMMAND ${_RUNTIME_EXE} build
                --file "${_DNS_SD_DISCOVERY_DOCKERFILE}"
                --tag kythira-dns-sd-discovery:dev
                "${CMAKE_SOURCE_DIR}"
        COMMENT "Building kythira-dns-sd-discovery:dev image"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-dns-discovery-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_DNS_DISCOVERY_COMPOSE_FILE=${_DNS_DISCOVERY_COMPOSE_FILE}
                $<TARGET_FILE:docker_dns_discovery_test>
                --log_level=message
        DEPENDS docker-bind9-image docker-dns-discovery-image docker_dns_discovery_test
        COMMENT "Running DNS (RFC 2136) discovery scenario tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(docker-dns-sd-discovery-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_DNS_SD_DISCOVERY_COMPOSE_FILE=${_DNS_SD_DISCOVERY_COMPOSE_FILE}
                $<TARGET_FILE:docker_dns_sd_discovery_test>
                --log_level=message
        DEPENDS docker-bind9-image docker-dns-sd-discovery-image docker_dns_sd_discovery_test
        COMMENT "Running DNS-SD discovery scenario tests (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    message(STATUS
        "Container runtime: ${_RUNTIME_EXE} — "
        "docker-chaos-image, docker-chaos-tests, "
        "docker-poco-discovery-image, docker-poco-discovery-tests, "
        "docker-bind9-image, "
        "docker-dns-discovery-image, docker-dns-discovery-tests, "
        "docker-dns-sd-discovery-image, docker-dns-sd-discovery-tests targets available")
else()
    foreach(_t
        docker-chaos-image docker-chaos-tests
        quorum-healing-tests
        docker-poco-discovery-image docker-poco-discovery-tests
        docker-bind9-image
        docker-dns-discovery-image docker-dns-discovery-tests
        docker-dns-sd-discovery-image docker-dns-sd-discovery-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
    message(STATUS "No container runtime found — all docker-* targets disabled")
endif()
