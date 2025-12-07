# Test configuration
find_package(Boost REQUIRED COMPONENTS unit_test_framework)

# Helper function for creating test executables
function(add_network_test test_name source_file)
    add_executable(${test_name} ${source_file})
    target_link_libraries(${test_name} PRIVATE
        network_simulator
        Boost::unit_test_framework
    )
    if(TARGET Folly::folly)
        target_link_libraries(${test_name} PRIVATE Folly::folly)
    endif()
    target_compile_features(${test_name} PRIVATE cxx_std_23)
    
    # Add test to CTest
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

# Test files
add_network_test(concept_test concept_test.cpp)
add_network_test(future_test future_test.cpp)
add_network_test(simulator_test simulator_test.cpp)
add_network_test(simulator_property_test simulator_property_test.cpp)
add_network_test(integration_test integration_test.cpp)

# Raft tests
add_network_test(raft_types_test raft_types_test.cpp)
add_network_test(persistence_concept_test persistence_concept_test.cpp)
add_network_test(persistence_round_trip_property_test persistence_round_trip_property_test.cpp)

# RPC serialization property test needs Boost.JSON
add_executable(rpc_serialization_property_test rpc_serialization_property_test.cpp)
target_link_libraries(rpc_serialization_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
target_compile_features(rpc_serialization_property_test PRIVATE cxx_std_23)
add_test(NAME rpc_serialization_property_test COMMAND rpc_serialization_property_test)

# RPC serializer concept test needs Boost.JSON
add_executable(rpc_serializer_concept_test rpc_serializer_concept_test.cpp)
target_link_libraries(rpc_serializer_concept_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
target_compile_features(rpc_serializer_concept_test PRIVATE cxx_std_23)
add_test(NAME rpc_serializer_concept_test COMMAND rpc_serializer_concept_test)

# RPC malformed message property test needs Boost.JSON
add_executable(rpc_malformed_message_property_test rpc_malformed_message_property_test.cpp)
target_link_libraries(rpc_malformed_message_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
target_compile_features(rpc_malformed_message_property_test PRIVATE cxx_std_23)
add_test(NAME rpc_malformed_message_property_test COMMAND rpc_malformed_message_property_test)

# Network retry property test needs Boost.JSON and Folly
add_executable(network_retry_property_test network_retry_property_test.cpp)
target_link_libraries(network_retry_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(network_retry_property_test PRIVATE Folly::folly)
endif()
target_compile_features(network_retry_property_test PRIVATE cxx_std_23)
add_test(NAME network_retry_property_test COMMAND network_retry_property_test)

add_executable(minimal_network_test minimal_network_test.cpp)
target_link_libraries(minimal_network_test PRIVATE
    network_simulator
    Folly::folly
)
target_compile_features(minimal_network_test PRIVATE cxx_std_23)

# Logger concept test
add_executable(logger_concept_test logger_concept_test.cpp)
target_link_libraries(logger_concept_test PRIVATE
    network_simulator
)
target_compile_features(logger_concept_test PRIVATE cxx_std_23)

# Console logger test
add_executable(console_logger_test console_logger_test.cpp)
target_link_libraries(console_logger_test PRIVATE
    network_simulator
)
target_compile_features(console_logger_test PRIVATE cxx_std_23)
add_test(NAME console_logger_test COMMAND console_logger_test)

# Metrics concept test
add_executable(metrics_concept_test metrics_concept_test.cpp)
target_link_libraries(metrics_concept_test PRIVATE
    network_simulator
)
target_compile_features(metrics_concept_test PRIVATE cxx_std_23)

# No-op metrics test
add_executable(noop_metrics_test noop_metrics_test.cpp)
target_link_libraries(noop_metrics_test PRIVATE
    network_simulator
)
target_compile_features(noop_metrics_test PRIVATE cxx_std_23)
add_test(NAME noop_metrics_test COMMAND noop_metrics_test)

# No-op metrics template test
add_executable(noop_metrics_template_test noop_metrics_template_test.cpp)
target_link_libraries(noop_metrics_template_test PRIVATE
    network_simulator
)
target_compile_features(noop_metrics_template_test PRIVATE cxx_std_23)
add_test(NAME noop_metrics_template_test COMMAND noop_metrics_template_test)

# Membership manager concept test
add_executable(membership_concept_test membership_concept_test.cpp)
target_link_libraries(membership_concept_test PRIVATE
    network_simulator
)
target_compile_features(membership_concept_test PRIVATE cxx_std_23)
add_test(NAME membership_concept_test COMMAND membership_concept_test)

# Default membership manager test
add_executable(default_membership_manager_test default_membership_manager_test.cpp)
target_link_libraries(default_membership_manager_test PRIVATE
    network_simulator
)
target_compile_features(default_membership_manager_test PRIVATE cxx_std_23)
add_test(NAME default_membership_manager_test COMMAND default_membership_manager_test)

# Raft configuration concept test
add_executable(raft_configuration_concept_test raft_configuration_concept_test.cpp)
target_link_libraries(raft_configuration_concept_test PRIVATE
    network_simulator
    Boost::unit_test_framework
)
target_compile_features(raft_configuration_concept_test PRIVATE cxx_std_23)
add_test(NAME raft_configuration_concept_test COMMAND raft_configuration_concept_test)

# Raft node structure test
add_executable(raft_node_structure_test raft_node_structure_test.cpp)
target_link_libraries(raft_node_structure_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_node_structure_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_node_structure_test PRIVATE cxx_std_23)
add_test(NAME raft_node_structure_test COMMAND raft_node_structure_test)

# Raft lifecycle test
add_executable(raft_lifecycle_test raft_lifecycle_test.cpp)
target_link_libraries(raft_lifecycle_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_lifecycle_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_lifecycle_test PRIVATE cxx_std_23)
add_test(NAME raft_lifecycle_test COMMAND raft_lifecycle_test)

# Raft crash recovery property test
add_executable(raft_crash_recovery_property_test raft_crash_recovery_property_test.cpp)
target_link_libraries(raft_crash_recovery_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_crash_recovery_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_crash_recovery_property_test PRIVATE cxx_std_23)
add_test(NAME raft_crash_recovery_property_test COMMAND raft_crash_recovery_property_test)

# RequestVote persistence property test
add_executable(request_vote_persistence_property_test request_vote_persistence_property_test.cpp)
target_link_libraries(request_vote_persistence_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(request_vote_persistence_property_test PRIVATE Folly::folly)
endif()
target_compile_features(request_vote_persistence_property_test PRIVATE cxx_std_23)
add_test(NAME request_vote_persistence_property_test COMMAND request_vote_persistence_property_test)

# Election safety property test
add_executable(raft_election_safety_property_test raft_election_safety_property_test.cpp)
target_link_libraries(raft_election_safety_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_election_safety_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_election_safety_property_test PRIVATE cxx_std_23)
add_test(NAME raft_election_safety_property_test COMMAND raft_election_safety_property_test)

# Higher term follower transition property test
add_executable(raft_higher_term_follower_property_test raft_higher_term_follower_property_test.cpp)
target_link_libraries(raft_higher_term_follower_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_higher_term_follower_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_higher_term_follower_property_test PRIVATE cxx_std_23)
add_test(NAME raft_higher_term_follower_property_test COMMAND raft_higher_term_follower_property_test)

# Log matching property test
add_executable(raft_log_matching_property_test raft_log_matching_property_test.cpp)
target_link_libraries(raft_log_matching_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_log_matching_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_log_matching_property_test PRIVATE cxx_std_23)
add_test(NAME raft_log_matching_property_test COMMAND raft_log_matching_property_test)

# Log convergence property test
add_executable(raft_log_convergence_property_test raft_log_convergence_property_test.cpp)
target_link_libraries(raft_log_convergence_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_log_convergence_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_log_convergence_property_test PRIVATE cxx_std_23)
add_test(NAME raft_log_convergence_property_test COMMAND raft_log_convergence_property_test)

# Heartbeat test
add_executable(raft_heartbeat_test raft_heartbeat_test.cpp)
target_link_libraries(raft_heartbeat_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_heartbeat_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_heartbeat_test PRIVATE cxx_std_23)
add_test(NAME raft_heartbeat_test COMMAND raft_heartbeat_test)

# Leader append-only property test
add_executable(raft_leader_append_only_property_test raft_leader_append_only_property_test.cpp)
target_link_libraries(raft_leader_append_only_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_leader_append_only_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_leader_append_only_property_test PRIVATE cxx_std_23)
add_test(NAME raft_leader_append_only_property_test COMMAND raft_leader_append_only_property_test)

# Commit implies replication property test
add_executable(raft_commit_implies_replication_property_test raft_commit_implies_replication_property_test.cpp)
target_link_libraries(raft_commit_implies_replication_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_commit_implies_replication_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_commit_implies_replication_property_test PRIVATE cxx_std_23)
add_test(NAME raft_commit_implies_replication_property_test COMMAND raft_commit_implies_replication_property_test)

# State machine safety property test
add_executable(raft_state_machine_safety_property_test raft_state_machine_safety_property_test.cpp)
target_link_libraries(raft_state_machine_safety_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_state_machine_safety_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_state_machine_safety_property_test PRIVATE cxx_std_23)
add_test(NAME raft_state_machine_safety_property_test COMMAND raft_state_machine_safety_property_test)

# Leader completeness property test
add_executable(raft_leader_completeness_property_test raft_leader_completeness_property_test.cpp)
target_link_libraries(raft_leader_completeness_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_leader_completeness_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_leader_completeness_property_test PRIVATE cxx_std_23)
add_test(NAME raft_leader_completeness_property_test COMMAND raft_leader_completeness_property_test)

# Linearizable operations property test
add_executable(raft_linearizable_operations_property_test raft_linearizable_operations_property_test.cpp)
target_link_libraries(raft_linearizable_operations_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_linearizable_operations_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_linearizable_operations_property_test PRIVATE cxx_std_23)
add_test(NAME raft_linearizable_operations_property_test COMMAND raft_linearizable_operations_property_test)

# Duplicate detection property test
add_executable(raft_duplicate_detection_property_test raft_duplicate_detection_property_test.cpp)
target_link_libraries(raft_duplicate_detection_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_duplicate_detection_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_duplicate_detection_property_test PRIVATE cxx_std_23)
add_test(NAME raft_duplicate_detection_property_test COMMAND raft_duplicate_detection_property_test)

# Snapshot preserves state property test
add_executable(raft_snapshot_preserves_state_property_test raft_snapshot_preserves_state_property_test.cpp)
target_link_libraries(raft_snapshot_preserves_state_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_snapshot_preserves_state_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_snapshot_preserves_state_property_test PRIVATE cxx_std_23)
add_test(NAME raft_snapshot_preserves_state_property_test COMMAND raft_snapshot_preserves_state_property_test)

# Joint consensus majority property test
add_executable(raft_joint_consensus_majority_property_test raft_joint_consensus_majority_property_test.cpp)
target_link_libraries(raft_joint_consensus_majority_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_joint_consensus_majority_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_joint_consensus_majority_property_test PRIVATE cxx_std_23)
add_test(NAME raft_joint_consensus_majority_property_test COMMAND raft_joint_consensus_majority_property_test)

# State transition logging property test
add_executable(raft_state_transition_logging_property_test raft_state_transition_logging_property_test.cpp)
target_link_libraries(raft_state_transition_logging_property_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_state_transition_logging_property_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_state_transition_logging_property_test PRIVATE cxx_std_23)
add_test(NAME raft_state_transition_logging_property_test COMMAND raft_state_transition_logging_property_test)

# Integration tests
# Leader election integration test
add_executable(raft_leader_election_integration_test raft_leader_election_integration_test.cpp)
target_link_libraries(raft_leader_election_integration_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_leader_election_integration_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_leader_election_integration_test PRIVATE cxx_std_23)
add_test(NAME raft_leader_election_integration_test COMMAND raft_leader_election_integration_test)

# Log replication integration test
add_executable(raft_log_replication_integration_test raft_log_replication_integration_test.cpp)
target_link_libraries(raft_log_replication_integration_test PRIVATE
    network_simulator
    Boost::unit_test_framework
    boost_json
)
if(TARGET Folly::folly)
    target_link_libraries(raft_log_replication_integration_test PRIVATE Folly::folly)
endif()
target_compile_features(raft_log_replication_integration_test PRIVATE cxx_std_23)
add_test(NAME raft_log_replication_integration_test COMMAND raft_log_replication_integration_test)
