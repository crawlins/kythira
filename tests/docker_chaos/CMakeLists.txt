if(NOT Boost_FOUND OR NOT httplib_FOUND)
    message(STATUS "docker_chaos tests skipped: requires Boost and httplib")
    return()
endif()

# Resolve boost_json the same way the rest of the project does.
if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
    set(_boost_json_lib
        "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
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

# ── OTLP telemetry backend Docker scenario test (Requirement 7) ─────────────
# Real chaos_node + real otel/opentelemetry-collector-contrib; no custom
# image needed for the collector (pulled directly, mirroring
# docker/ca-provisioning-compose.yml's use of stock alpine:3.20).

add_docker_chaos_scenario_test(docker_otlp_collector_test
    "${CMAKE_SOURCE_DIR}/tests/docker_chaos/otlp_collector_test.cpp")

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
    set(_OTLP_COLLECTOR_COMPOSE_FILE     "${CMAKE_SOURCE_DIR}/docker/otlp-collector-compose.yml")

    # Env-var prefix injected into every scenario-test invocation.
    set(_RT_ENV
        KYTHIRA_CONTAINER_RUNTIME=${CONTAINER_RUNTIME}
        KYTHIRA_COMPOSE_COMMAND=${COMPOSE_COMMAND}
    )

    # docker-chaos-image: stage the host-built chaos_node binary, then
    # package it into a runtime-only image (.kiro/specs/chaos-node-host-build/).
    # Guarded exactly like cmd/chaos_node's own inclusion at the top level
    # (NOT ENABLE_COVERAGE AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)
    # — chaos_node is defined later in configure-time processing order
    # (cmd/chaos_node is added after tests/ at the top level) so
    # `if(TARGET chaos_node)` isn't usable here; this must match the top-level
    # CMakeLists.txt guard exactly, including NOT ENABLE_COVERAGE, or a
    # coverage build (which skips add_subdirectory(cmd/chaos_node) entirely)
    # hits this branch with folly/Boost/httplib all found but no chaos_node
    # target actually defined, failing CMake generate on
    # `$<TARGET_FILE:chaos_node>` with "No target chaos_node".
    if(NOT ENABLE_COVERAGE AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)
        add_custom_target(docker-chaos-image
            DEPENDS chaos_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_SOURCE_DIR}/docker/chaos_node/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:chaos_node>"
                    "${CMAKE_SOURCE_DIR}/docker/chaos_node/dist/chaos_node"
            COMMAND ${_RUNTIME_EXE} build
                    --file "${_CHAOS_DOCKERFILE}"
                    --tag kythira-chaos-node:dev
                    "${CMAKE_SOURCE_DIR}"
            COMMENT "Staging chaos_node binary and building kythira-chaos-node:dev image"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    else()
        add_custom_target(docker-chaos-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-chaos-image' requires chaos_node to be buildable (folly + Boost + httplib)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

    # docker-poco-discovery-image: stage the host-built poco_discovery_node
    # binary, then package it into a runtime-only image
    # (.kiro/specs/discovery-nodes-host-build/). Guarded exactly like
    # cmd/poco_discovery_node's own inclusion at the top level, for the same
    # `if(TARGET ...)`-isn't-usable-here reason docker-chaos-image's own
    # comment above documents (cmd/poco_discovery_node is also added after
    # tests/ at the top level).
    if(POCO_DNSSD_FOUND AND folly_FOUND AND Boost_FOUND AND httplib_FOUND)
        add_custom_target(docker-poco-discovery-image
            DEPENDS poco_discovery_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:poco_discovery_node>"
                    "${CMAKE_SOURCE_DIR}/docker/poco_discovery_node/dist/poco_discovery_node"
            COMMAND ${_RUNTIME_EXE} build
                    --file "${_POCO_DISCOVERY_DOCKERFILE}"
                    --tag kythira-poco-discovery:dev
                    "${CMAKE_SOURCE_DIR}"
            COMMENT "Staging poco_discovery_node binary and building kythira-poco-discovery:dev image"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    else()
        add_custom_target(docker-poco-discovery-image
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'docker-poco-discovery-image' requires poco_discovery_node to be buildable (Poco DNSSD + folly + Boost + httplib)"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endif()

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

    # docker-dns-discovery-image / docker-dns-sd-discovery-image: stage each
    # host-built binary, then package into a runtime-only image
    # (.kiro/specs/discovery-nodes-host-build/) — same shape and rationale as
    # docker-poco-discovery-image above, guarded on cmd/dns_discovery_node's
    # and cmd/dns_sd_discovery_node's own condition.
    if(LIBLDNS_FOUND AND folly_FOUND AND httplib_FOUND)
        add_custom_target(docker-dns-discovery-image
            DEPENDS dns_discovery_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_SOURCE_DIR}/docker/dns_discovery_node/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:dns_discovery_node>"
                    "${CMAKE_SOURCE_DIR}/docker/dns_discovery_node/dist/dns_discovery_node"
            COMMAND ${_RUNTIME_EXE} build
                    --file "${_DNS_DISCOVERY_DOCKERFILE}"
                    --tag kythira-dns-discovery:dev
                    "${CMAKE_SOURCE_DIR}"
            COMMENT "Staging dns_discovery_node binary and building kythira-dns-discovery:dev image"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
        add_custom_target(docker-dns-sd-discovery-image
            DEPENDS dns_sd_discovery_node
            COMMAND ${CMAKE_COMMAND} -E make_directory
                    "${CMAKE_SOURCE_DIR}/docker/dns_sd_discovery_node/dist"
            COMMAND ${CMAKE_COMMAND} -E copy
                    "$<TARGET_FILE:dns_sd_discovery_node>"
                    "${CMAKE_SOURCE_DIR}/docker/dns_sd_discovery_node/dist/dns_sd_discovery_node"
            COMMAND ${_RUNTIME_EXE} build
                    --file "${_DNS_SD_DISCOVERY_DOCKERFILE}"
                    --tag kythira-dns-sd-discovery:dev
                    "${CMAKE_SOURCE_DIR}"
            COMMENT "Staging dns_sd_discovery_node binary and building kythira-dns-sd-discovery:dev image"
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        )
    else()
        foreach(_t docker-dns-discovery-image docker-dns-sd-discovery-image)
            add_custom_target(${_t}
                COMMAND ${CMAKE_COMMAND} -E echo
                        "'${_t}' requires the relevant node to be buildable (libldns + folly + httplib)"
                COMMAND ${CMAKE_COMMAND} -E false
            )
        endforeach()
    endif()

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

    # otel-collector's image is pulled directly (no custom Dockerfile, per
    # Requirement 7.2) — this target only needs the existing chaos-node image.
    add_custom_target(docker-otlp-collector-tests
        COMMAND ${CMAKE_COMMAND} -E env
                ${_RT_ENV}
                KYTHIRA_OTLP_COLLECTOR_COMPOSE_FILE=${_OTLP_COLLECTOR_COMPOSE_FILE}
                $<TARGET_FILE:docker_otlp_collector_test>
                --log_level=message
        DEPENDS docker-chaos-image docker_otlp_collector_test
        COMMENT "Running OTLP telemetry backend Collector scenario test (runtime: ${CONTAINER_RUNTIME})"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    )

    message(STATUS
        "Container runtime: ${_RUNTIME_EXE} — "
        "docker-chaos-image, docker-chaos-tests, "
        "docker-poco-discovery-image, docker-poco-discovery-tests, "
        "docker-bind9-image, "
        "docker-dns-discovery-image, docker-dns-discovery-tests, "
        "docker-dns-sd-discovery-image, docker-dns-sd-discovery-tests, "
        "docker-otlp-collector-tests targets available")
else()
    foreach(_t
        docker-chaos-image docker-chaos-tests
        quorum-healing-tests
        docker-poco-discovery-image docker-poco-discovery-tests
        docker-bind9-image
        docker-dns-discovery-image docker-dns-discovery-tests
        docker-dns-sd-discovery-image docker-dns-sd-discovery-tests
        docker-otlp-collector-tests)
        add_custom_target(${_t}
            COMMAND ${CMAKE_COMMAND} -E echo
                    "'${_t}' requires docker or podman on PATH"
            COMMAND ${CMAKE_COMMAND} -E false
        )
    endforeach()
    message(STATUS "No container runtime found — all docker-* targets disabled")
endif()
