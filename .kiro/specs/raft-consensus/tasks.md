# Implementation Plan

- [x] 1. Set up Raft project structure and core type definitions
  - Create directory structure: `include/raft/` for headers
  - Define core Raft types (term_id, log_index, node_id concepts)
  - Define server_state enum (follower, candidate, leader)
  - Define exception hierarchy (raft_exception, network_exception, persistence_exception, serialization_exception, election_exception)
  - _Requirements: 1.1_

- [x] 2. Implement RPC message type concepts and default implementations
  - Define RPC message concepts (request_vote_request_type, request_vote_response_type, append_entries_request_type, append_entries_response_type, install_snapshot_request_type, install_snapshot_response_type)
  - Implement default RPC message structs (request_vote_request, request_vote_response, append_entries_request, append_entries_response, install_snapshot_request, install_snapshot_response)
  - _Requirements: 2.5, 7.2_

- [x] 3. Implement core data model concepts and default implementations
  - Define log_entry_type concept and default log_entry implementation
  - Define cluster_configuration_type concept and default cluster_configuration implementation
  - Define snapshot_type concept and default snapshot implementation
  - _Requirements: 7.1, 9.2, 10.2_

- [x] 3.1 Write property test for RPC message serialization round-trip
  - **Property 6: RPC Serialization Round-Trip**
  - **Validates: Requirements 2.5**

- [x] 4. Define RPC serializer concept
  - Define serialized_data concept (range of std::byte)
  - Define rpc_serializer concept with serialize/deserialize methods for all RPC types
  - _Requirements: 2.1, 2.2_

- [x] 5. Implement JSON RPC serializer
  - Implement json_rpc_serializer<std::vector<std::byte>> class
  - Implement serialization for all RPC message types using JSON
  - Implement deserialization with validation
  - _Requirements: 2.3, 2.5_

- [x] 5.1 Write property test for JSON serialization round-trip
  - **Property 6: RPC Serialization Round-Trip**
  - **Validates: Requirements 2.5**

- [x] 5.2 Write property test for malformed message rejection
  - **Property 7: Malformed Message Rejection**
  - **Validates: Requirements 2.6**

- [x] 6. Define network client and server concepts
  - Define network_client concept with send_request_vote, send_append_entries, send_install_snapshot methods
  - Define network_server concept with register handlers and lifecycle methods
  - _Requirements: 3.1, 3.2_

- [x] 7. Implement network simulator transport for Raft
  - Implement simulator_network_client<Serializer, Data> using existing network simulator
  - Implement simulator_network_server<Serializer, Data> using existing network simulator
  - Ensure state machine only communicates through network interfaces
  - _Requirements: 3.11, 3.12_

- [x] 7.1 Write property test for network retry convergence
  - **Property 8: Network Retry Convergence**
  - **Validates: Requirements 3.13**

- [x] 8. Define persistence engine concept
  - Define persistence_engine concept with save/load methods for term, votedFor, log entries, and snapshots
  - _Requirements: 5.1, 5.2_

- [x] 9. Implement in-memory persistence engine
  - Implement memory_persistence_engine for testing and development
  - Implement all CRUD operations for Raft state
  - _Requirements: 5.4_

- [x] 9.1 Write property test for persistence round-trip
  - **Property 10: Persistence Round-Trip**
  - **Validates: Requirements 5.6**

- [x] 10. Define diagnostic logger concept
  - Define log_level enum (trace, debug, info, warning, error, critical)
  - Define diagnostic_logger concept with structured logging methods
  - _Requirements: 4.1, 4.2_

- [x] 11. Implement basic console logger
  - Implement console_logger for development and testing
  - Support structured logging with key-value pairs
  - _Requirements: 4.6_

- [x] 12. Define metrics concept
  - Define metrics concept with set_metric_name, add_dimension, add_one, add_count, add_duration, add_value, emit methods
  - _Requirements: 13.1, 13.2, 13.3, 13.4, 13.5, 13.6_

- [x] 13. Implement no-op metrics recorder
  - Implement noop_metrics for testing without metrics overhead
  - _Requirements: 13.7_

- [x] 14. Define membership manager concept
  - Define membership_manager concept with validate_new_node, authenticate_node, create_joint_configuration, is_node_in_configuration, handle_node_removal methods
  - _Requirements: 9.1, 12.1, 12.2_

- [x] 15. Implement default membership manager
  - Implement default_membership_manager with basic validation
  - Implement joint consensus configuration creation
  - _Requirements: 9.2, 12.3, 12.4_

- [x] 16. Define raft_configuration concept and default implementation
  - Define raft_configuration_type concept with timing and size parameters
  - Implement default raft_configuration with reasonable defaults
  - _Requirements: 6.1, 6.3, 7.1_

- [x] 17. Implement Raft node class template structure
  - Define node class template with all component template parameters
  - Define persistent state members (current_term, voted_for, log)
  - Define volatile state members (commit_index, last_applied, state)
  - Define leader-specific volatile state (next_index, match_index)
  - Define component members and configuration
  - _Requirements: 1.2, 1.3_

- [x] 18. Implement Raft node initialization and lifecycle
  - Implement node constructor with component initialization
  - Implement start() method to begin Raft protocol
  - Implement stop() method to gracefully shutdown
  - Implement state recovery from persistence on startup
  - _Requirements: 1.7, 5.6_

- [x] 18.1 Write property test for crash recovery
  - **Property 17: Crash Recovery**
  - **Validates: Requirements 1.7**

- [x] 19. Implement RequestVote RPC handler
  - Implement request_vote_handler to process RequestVote requests
  - Implement vote granting logic with log up-to-dateness check
  - Ensure persistence before responding
  - _Requirements: 6.1, 8.1, 8.2_

- [x] 19.1 Write property test for persistence before response
  - **Property 9: Persistence Before Response**
  - **Validates: Requirements 5.5**

- [x] 20. Implement election timeout and candidate behavior
  - Implement election timer with randomized timeout
  - Implement transition from follower to candidate
  - Implement RequestVote RPC sending to all peers
  - Implement vote counting and transition to leader
  - _Requirements: 6.1, 6.2, 6.3_

- [x] 20.1 Write property test for election safety
  - **Property 1: Election Safety**
  - **Validates: Requirements 6.5**

- [x] 21. Implement term discovery and follower transition
  - Implement higher term detection in all RPC handlers
  - Implement immediate transition to follower on higher term
  - _Requirements: 6.4_

- [x] 21.1 Write property test for higher term causes follower transition
  - **Property 22: Higher Term Causes Follower Transition**
  - **Validates: Requirements 6.4**

- [x] 22. Implement AppendEntries RPC handler
  - Implement append_entries_handler to process AppendEntries requests
  - Implement log consistency check (prev_log_index, prev_log_term)
  - Implement log conflict resolution (overwrite conflicting entries)
  - Implement commit index update
  - Ensure persistence before responding
  - _Requirements: 7.2, 7.3, 7.5_

- [x] 22.1 Write property test for log matching
  - **Property 3: Log Matching**
  - **Validates: Requirements 7.5**

- [x] 22.2 Write property test for log convergence
  - **Property 11: Log Convergence**
  - **Validates: Requirements 7.3**

- [x] 23. Implement leader heartbeat mechanism
  - Implement heartbeat timer
  - Implement periodic empty AppendEntries RPC sending
  - _Requirements: 6.2_

- [x] 24. Implement client command submission
  - Implement submit_command() method for leaders
  - Implement command appending to leader's log
  - Implement log replication to followers
  - _Requirements: 7.1, 11.1_

- [x] 24.1 Write property test for leader append-only
  - **Property 2: Leader Append-Only**
  - **Validates: Requirements 8.1**

- [x] 25. Implement commit index advancement
  - Implement match_index tracking for all followers
  - Implement commit index calculation (majority replication)
  - Implement commit index advancement with current term check
  - _Requirements: 7.4, 8.3_

- [x] 25.1 Write property test for commit implies replication
  - **Property 12: Commit Implies Replication**
  - **Validates: Requirements 7.4**

- [x] 26. Implement state machine application
  - Implement apply_to_state_machine() method
  - Implement sequential application of committed entries
  - Implement last_applied index tracking
  - _Requirements: 1.1, 7.4_

- [x] 26.1 Write property test for state machine safety
  - **Property 5: State Machine Safety**
  - **Validates: Requirements 8.4**

- [x] 27. Implement leader completeness guarantee
  - Implement no-op entry commitment on leader election
  - Ensure leaders only commit entries from current term directly
  - _Requirements: 8.3, 8.5, 11.3_

- [x] 27.1 Write property test for leader completeness
  - **Property 4: Leader Completeness**
  - **Validates: Requirements 8.1, 8.5**

- [x] 28. Implement linearizable read operations
  - Implement read_state() method with heartbeat-based lease
  - Implement read index optimization
  - _Requirements: 11.2, 11.5_

- [x] 28.1 Write property test for linearizable operations
  - **Property 15: Linearizable Operations**
  - **Validates: Requirements 1.4**

- [x] 29. Implement duplicate detection for client operations
  - Implement client session tracking with serial numbers
  - Implement response caching for duplicate detection
  - _Requirements: 11.1, 11.4_

- [x] 29.1 Write property test for duplicate detection
  - **Property 19: Duplicate Detection**
  - **Validates: Requirements 11.4**

- [x] 30. Implement snapshot creation
  - Implement create_snapshot() method
  - Implement snapshot threshold checking
  - Implement state machine state capture
  - Implement snapshot metadata (last_included_index, last_included_term, configuration)
  - _Requirements: 10.1, 10.2_

- [x] 30.1 Write property test for snapshot preserves state
  - **Property 14: Snapshot Preserves State**
  - **Validates: Requirements 10.5**

- [x] 31. Implement log compaction
  - Implement log entry deletion after snapshot
  - Implement safe log truncation
  - _Requirements: 5.7, 10.5_

- [x] 32. Implement InstallSnapshot RPC handler
  - Implement install_snapshot_handler to process InstallSnapshot requests
  - Implement snapshot chunk receiving and assembly
  - Implement state machine restoration from snapshot
  - Implement log truncation after snapshot installation
  - _Requirements: 10.3, 10.4_

- [x] 33. Implement InstallSnapshot RPC sending
  - Implement snapshot transfer for lagging followers
  - Implement chunked snapshot transmission
  - _Requirements: 10.3, 10.4_

- [x] 34. Implement cluster membership changes
  - Implement add_server() method with joint consensus
  - Implement remove_server() method with joint consensus
  - Implement non-voting member catch-up phase
  - Implement leader step-down after removal
  - _Requirements: 9.2, 9.3, 9.4, 9.5_

- [x] 34.1 Write property test for joint consensus majority
  - **Property 13: Joint Consensus Majority**
  - **Validates: Requirements 9.3**

- [x] 35. Implement removed server disruption prevention
  - Implement configuration-based RequestVote filtering
  - _Requirements: 9.6_

- [x] 36. Implement comprehensive state transition logging
  - Add logging for all state transitions (follower→candidate, candidate→leader, etc.)
  - Add logging for term changes
  - Add logging for membership changes
  - _Requirements: 4.6_

- [x] 36.1 Write property test for state transition logging
  - **Property 21: State Transition Logging**
  - **Validates: Requirements 4.6**

- [x] 37. Implement metrics emission
  - Add metrics for elections (election_started, election_won, election_lost)
  - Add metrics for log replication (entries_replicated, replication_latency)
  - Add metrics for commits (entries_committed, commit_latency)
  - Add metrics for RPC latencies (request_vote_latency, append_entries_latency)
  - _Requirements: 13.7_

- [x] 38. Implement raft_node concept
  - Define raft_node concept with submit_command, read_state, lifecycle, state queries, and cluster operations
  - Verify node implementation satisfies concept
  - _Requirements: 1.1_

- [x] 39. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 40. Write integration test for leader election with failures
  - Test leader election with various failure patterns
  - Test split vote prevention with randomized timeouts
  - _Requirements: 6.1, 6.2, 6.3_

- [x] 41. Write integration test for log replication with partitions
  - Test log replication across network partitions
  - Test log convergence after partition healing
  - _Requirements: 7.1, 7.2, 7.3_

- [x] 42. Write integration test for cluster membership changes
  - Test adding servers to cluster
  - Test removing servers from cluster
  - Test joint consensus transitions
  - _Requirements: 9.2, 9.3, 9.4_

- [x] 43. Write integration test for snapshot creation and installation
  - Test snapshot creation at threshold
  - Test snapshot installation for lagging followers
  - Test log compaction after snapshot
  - _Requirements: 10.1, 10.2, 10.3, 10.4, 10.5_

- [x] 44. Write property test for safety under partitions
  - **Property 18: Safety Under Partitions**
  - **Validates: Requirements 1.5**

- [x] 45. Write property test for majority availability
  - **Property 16: Majority Availability**
  - **Validates: Requirements 1.6**

- [x] 46. Write property test for liveness after partition healing
  - **Property 20: Liveness After Partition Healing**
  - **Validates: Requirements 14.5**

- [x] 47. Create example program for basic Raft cluster
  - Create examples/raft/basic_cluster.cpp
  - Demonstrate creating a 3-node cluster
  - Demonstrate submitting commands
  - Demonstrate reading state
  - Follow example program guidelines (run all scenarios, clear pass/fail, exit codes)
  - _Requirements: 1.1, 1.4_

- [x] 48. Create example program for failure scenarios
  - Create examples/raft/failure_scenarios.cpp
  - Demonstrate leader failure and re-election
  - Demonstrate network partition and recovery
  - Demonstrate follower crash and recovery
  - Follow example program guidelines
  - _Requirements: 1.5, 1.6, 1.7_

- [x] 49. Create example program for membership changes
  - Create examples/raft/membership_changes.cpp
  - Demonstrate adding a server to the cluster
  - Demonstrate removing a server from the cluster
  - Follow example program guidelines
  - _Requirements: 9.2, 9.4, 9.5_

- [x] 50. Create example program for snapshot and log compaction
  - Create examples/raft/snapshot_example.cpp
  - Demonstrate snapshot creation
  - Demonstrate log compaction
  - Demonstrate snapshot installation
  - Follow example program guidelines
  - _Requirements: 10.1, 10.2, 10.3, 10.5_

- [x] 51. Update CMakeLists.txt for Raft examples
  - Add Raft examples subdirectory to examples/CMakeLists.txt
  - Create examples/raft/CMakeLists.txt
  - Add all example executables with proper linking
  - Set output directories for organized builds
  - _Requirements: 1.1_

- [x] 52. Final checkpoint - Ensure all tests and examples pass
  - Ensure all tests pass, ask the user if questions arise.
