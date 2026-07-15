if(NOT CHAOS_TESTS_ENABLED)
    add_custom_target(chaos-tests
        COMMAND ${CMAKE_COMMAND} -E echo
            "ERROR: chaos tests require libfiu-dev (sudo apt install libfiu-dev)"
        COMMAND ${CMAKE_COMMAND} -E false
        COMMENT "chaos-tests requires libfiu — not available"
    )
    return()
endif()

# Helper to build one chaos test executable
function(add_chaos_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE
        network_simulator
        Boost::unit_test_framework
        ${FIU_LIBRARIES}
    )
    if(TARGET Folly::folly)
        target_link_libraries(${test_name} PRIVATE Folly::folly)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
        target_link_libraries(${test_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_json.a")
    elseif(TARGET Boost::json)
        target_link_libraries(${test_name} PRIVATE Boost::json)
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_context.a")
        target_link_libraries(${test_name} PRIVATE
            "${CMAKE_SOURCE_DIR}/vcpkg_installed/${KYTHIRA_VCPKG_TRIPLET}/lib/libboost_context.a")
    elseif(TARGET Boost::context)
        target_link_libraries(${test_name} PRIVATE Boost::context)
    endif()
    target_include_directories(${test_name} PRIVATE
        ${FIU_INCLUDE_DIRS}
        "${CMAKE_SOURCE_DIR}/tests/chaos"
    )
    target_compile_definitions(${test_name} PRIVATE FIU_ENABLE)
    target_compile_options(${test_name} PRIVATE ${FIU_CFLAGS_OTHER})
    target_compile_features(${test_name} PRIVATE cxx_std_23)

    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "chaos")
endfunction()

# ── Chaos test executables ────────────────────────────────────────────────────
add_chaos_test(chaos_smoke_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_smoke_test.cpp")

add_chaos_test(chaos_profiles_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_profiles_test.cpp")

add_chaos_test(chaos_election_safety_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_election_safety_test.cpp")

add_chaos_test(chaos_log_matching_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_log_matching_test.cpp")

add_chaos_test(chaos_leader_completeness_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_leader_completeness_test.cpp")

add_chaos_test(chaos_state_machine_safety_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_state_machine_safety_test.cpp")

add_chaos_test(chaos_leader_election_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_leader_election_recovery_test.cpp")

add_chaos_test(chaos_commit_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_commit_recovery_test.cpp")

add_chaos_test(chaos_persistence_degradation_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_persistence_degradation_recovery_test.cpp")

add_chaos_test(chaos_state_machine_recovery_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/chaos_state_machine_recovery_test.cpp")

add_chaos_test(fiu_remote_unit_test
    "${CMAKE_SOURCE_DIR}/tests/chaos/fiu_remote_unit_test.cpp")
target_compile_definitions(fiu_remote_unit_test PRIVATE KYTHIRA_FAULT_INJECTION)
target_include_directories(fiu_remote_unit_test PRIVATE
    "${CMAKE_SOURCE_DIR}/cmd/chaos_node")

# DNS peer-discovery chaos tests — only built when libldns is present.
if(LIBLDNS_FOUND)
    add_chaos_test(dns_peer_discovery_chaos_test
        "${CMAKE_SOURCE_DIR}/tests/chaos/dns_peer_discovery_chaos_test.cpp")
    target_link_libraries(dns_peer_discovery_chaos_test PRIVATE ${LIBLDNS_LIBRARIES})
    target_include_directories(dns_peer_discovery_chaos_test PRIVATE ${LIBLDNS_INCLUDE_DIRS})
    set_tests_properties(dns_peer_discovery_chaos_test PROPERTIES
        TIMEOUT 120
        LABELS "chaos;dns;peer_discovery"
    )
endif()

# ── chaos-tests aggregate target ─────────────────────────────────────────────
set(_chaos_targets
    chaos_smoke_test
    chaos_profiles_test
    chaos_election_safety_test
    chaos_log_matching_test
    chaos_leader_completeness_test
    chaos_state_machine_safety_test
    chaos_leader_election_recovery_test
    chaos_commit_recovery_test
    chaos_persistence_degradation_recovery_test
    chaos_state_machine_recovery_test
    fiu_remote_unit_test
)
if(LIBLDNS_FOUND)
    list(APPEND _chaos_targets dns_peer_discovery_chaos_test)
endif()

add_custom_target(chaos-tests
    DEPENDS ${_chaos_targets}
    COMMENT "Building all chaos test executables"
)
