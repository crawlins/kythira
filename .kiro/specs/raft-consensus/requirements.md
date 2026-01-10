# Requirements Document

## Introduction

This document specifies the requirements for implementing a complete, production-ready Raft consensus algorithm in C++. Raft is a consensus algorithm for managing a replicated log that is designed to be more understandable than Paxos while providing equivalent functionality. The implementation will use C++20/23 concepts to define pluggable interfaces for RPC serialization, network transport, logging, and state machine persistence, enabling flexible deployment across different environments and use cases.

This specification covers both the core Raft consensus protocol and the completion components necessary for production deployment, including commit waiting mechanisms, async future handling, configuration change synchronization, and comprehensive error handling for network operations. The completion focuses on making the Raft implementation fully asynchronous and robust, ensuring that client operations properly wait for commit and state machine application, that configuration changes are safely synchronized, and that network failures are handled gracefully with appropriate retry and recovery mechanisms.

**Implementation Note:** This implementation will use generic future concepts as template parameters, allowing flexibility in future implementations while defaulting to the kythira future wrapper classes from `include/raft/future.hpp`. The kythira wrappers (`kythira::Future<T>`, `kythira::Promise<T>`, `kythira::Try<T>`, etc.) provide concept-compliant wrappers around Folly futures and serve as the default concrete implementation. Core implementations will be placed in the `kythira` namespace to reflect their foundational role in the system architecture.

## Glossary

- **Raft_Cluster**: A collection of servers running the Raft consensus algorithm
- **Raft_Node**: A single server instance participating in the Raft consensus protocol
- **Raft_Leader**: The distinguished server that handles all client requests and log replication
- **Raft_Follower**: A passive server that responds to requests from leaders and candidates
- **Raft_Candidate**: A server attempting to become the leader during an election
- **Raft_Term**: A logical time period with consecutive integer numbering, each beginning with an election
- **Raft_Log**: An ordered sequence of log entries containing state machine commands
- **Log_Entry**: A single command in the replicated log with associated term and index
- **Committed_Entry**: A log entry that has been safely replicated to a majority of servers
- **State_Machine**: The application-specific component that executes committed log entries
- **RPC_Serializer**: A pluggable component that handles serialization/deserialization of RPC messages
- **Network_Client**: A pluggable component that sends RPC requests to remote nodes and depends on the RPC_Serializer for message encoding
- **Network_Server**: A pluggable component that receives RPC requests from remote nodes and depends on the RPC_Serializer for message decoding
- **Diagnostic_Logger**: A pluggable component that provides structured logging capabilities
- **Metrics_Recorder**: A pluggable component that collects and reports performance and operational metrics
- **Persistence_Engine**: A pluggable component that handles durable storage of Raft state
- **Membership_Manager**: A pluggable component that defines the logic for managing cluster membership changes
- **Raft_State_Machine**: The application-specific state machine that interacts with the network through the Network_Client and Network_Server interfaces
- **Election_Timeout**: The time period after which a follower becomes a candidate if no heartbeat is received
- **Heartbeat_Interval**: The frequency at which leaders send empty AppendEntries RPCs to maintain authority
- **Joint_Consensus**: A transitional configuration state used during cluster membership changes
- **Snapshot**: A compact representation of the state machine state at a specific log index
- **Commit_Waiting**: The mechanism by which client operations wait for log entries to be committed and applied to the state machine
- **Future_Collection**: The mechanism for collecting and waiting on multiple asynchronous operations (futures) simultaneously
- **Configuration_Change_Synchronization**: The process of ensuring configuration changes are properly committed before proceeding to the next phase
- **RPC_Error_Handling**: The comprehensive handling of network failures, timeouts, and retry logic for Raft RPC operations
- **State_Machine_Application**: The process of applying committed log entries to the application state machine
- **Commit_Index**: The highest log index known to be committed (replicated to majority)
- **Applied_Index**: The highest log index that has been applied to the state machine
- **Future_Type**: The generic future concept used throughout the Raft implementation, with kythira::Future<T> as the default concrete implementation
- **Future_Concept**: The generic interface requirements for future types, defined in `include/concepts/future.hpp`
- **Core_Implementation**: Generic Raft implementations in the `kythira` namespace that accept future types as template parameters
- **Heartbeat_Collection**: The mechanism for collecting heartbeat responses to determine leader validity
- **Election_Collection**: The mechanism for collecting vote responses during leader election

## Requirements

### Requirement 1

**User Story:** As a distributed systems developer, I want to integrate Raft consensus into my application, so that I can build fault-tolerant replicated services with strong consistency guarantees.

#### Acceptance Criteria

1. WHEN the Raft implementation is instantiated THEN the system SHALL provide a complete consensus algorithm implementation that satisfies all Raft safety and liveness properties
2. WHEN a concrete cluster implementation is defined THEN the system SHALL use template parameters to select the future type, RPC serializer, network client, network server, diagnostic logger, metrics recorder, persistence engine, and membership manager components
3. WHEN concrete component classes are defined THEN the system SHALL use template parameters to specify their dependency classes that conform to the corresponding concept declarations
4. WHEN the core Raft implementation is defined THEN the system SHALL accept future types as template parameters and use future concepts instead of concrete future types
5. WHEN core implementations are organized THEN the system SHALL place core implementations in the `kythira` namespace instead of the `raft` namespace
6. WHEN client commands are submitted to the Raft cluster THEN the system SHALL ensure linearizable semantics for all operations
7. WHEN network partitions or server failures occur THEN the system SHALL maintain safety properties and resume normal operation when connectivity is restored
8. WHEN the cluster has a majority of servers operational THEN the system SHALL remain available for client requests
9. WHEN servers crash and restart THEN the system SHALL recover their state from persistent storage and rejoin the cluster

### Requirement 2

**User Story:** As a systems architect, I want pluggable RPC serialization, so that I can choose between different serialization formats based on performance and compatibility requirements.

#### Acceptance Criteria

1. WHEN the RPC serializer concept is defined THEN the system SHALL support pluggable serialization implementations through C++ concepts
2. WHEN concrete RPC serializer classes are implemented THEN the system SHALL ensure they conform to the rpc_serializer concept declaration
3. WHEN JSON serialization is used THEN the system SHALL serialize and deserialize all Raft RPC messages using JSON format
4. WHEN Protocol Buffers serialization is used THEN the system SHALL serialize and deserialize all Raft RPC messages using protobuf format
5. WHEN serializing Raft messages THEN the system SHALL preserve all required fields for RequestVote and AppendEntries RPCs
6. WHEN deserializing Raft messages THEN the system SHALL validate message structure and reject malformed messages

### Requirement 3

**User Story:** As a network administrator, I want pluggable network transport with separate client and server components, so that I can deploy Raft over different network protocols based on infrastructure requirements.

#### Acceptance Criteria

1. WHEN the network client concept is defined THEN the system SHALL support pluggable client implementations through C++ concepts with generic future types
2. WHEN the network server concept is defined THEN the system SHALL support pluggable server implementations through C++ concepts with generic future types
3. WHEN concrete network client classes are defined THEN the system SHALL use template parameters to specify the RPC_Serializer type and future type that conform to their respective concepts
4. WHEN concrete network server classes are defined THEN the system SHALL use template parameters to specify the RPC_Serializer type and future type that conform to their respective concepts
5. WHEN the network client is instantiated THEN the system SHALL use the templated RPC_Serializer for encoding outgoing messages and return templated future types
6. WHEN the network server is instantiated THEN the system SHALL use the templated RPC_Serializer for decoding incoming messages and handle templated future types
7. WHEN transport implementations are organized THEN the system SHALL place network client and transport classes in the `kythira` namespace
8. WHEN HTTP transport is used THEN the system SHALL send and receive Raft RPCs over HTTP/1.1 or HTTP/2 connections
9. WHEN HTTPS transport is used THEN the system SHALL send and receive Raft RPCs over encrypted TLS connections with certificate validation
10. WHEN CoAP transport is used THEN the system SHALL send and receive Raft RPCs over UDP using the Constrained Application Protocol
11. WHEN CoAP over DTLS transport is used THEN the system SHALL send and receive Raft RPCs over encrypted DTLS connections with certificate validation
12. WHEN network simulator transport is used THEN the system SHALL send and receive Raft RPCs through the existing network simulator framework for testing and development
13. WHEN the state machine interacts with the network THEN the system SHALL ensure the state machine only communicates through the Network_Client and Network_Server interfaces
14. WHEN network operations fail THEN the system SHALL retry RPCs according to Raft timeout and failure handling requirements

### Requirement 4

**User Story:** As a system operator, I want pluggable diagnostic logging, so that I can integrate Raft with existing logging infrastructure and monitoring systems.

#### Acceptance Criteria

1. WHEN the diagnostic logger concept is defined THEN the system SHALL support pluggable logging implementations through C++ concepts
2. WHEN concrete diagnostic logger classes are implemented THEN the system SHALL ensure they conform to the diagnostic_logger concept declaration
3. WHEN log4cpp logging is used THEN the system SHALL output structured log messages using the log4cpp library
4. WHEN spdlog logging is used THEN the system SHALL output structured log messages using the spdlog library
5. WHEN Boost.Log logging is used THEN the system SHALL output structured log messages using the Boost.Log library
6. WHEN Raft state transitions occur THEN the system SHALL log all leader elections, term changes, and membership changes with appropriate severity levels

### Requirement 5

**User Story:** As a database developer, I want pluggable state machine persistence, so that I can choose storage backends optimized for my application's data patterns and performance requirements.

#### Acceptance Criteria

1. WHEN the persistence engine concept is defined THEN the system SHALL support pluggable storage implementations through C++ concepts
2. WHEN concrete persistence engine classes are implemented THEN the system SHALL ensure they conform to the persistence_engine concept declaration
3. WHEN RocksDB persistence is used THEN the system SHALL store Raft state and log entries using RocksDB with appropriate durability guarantees
4. WHEN in-memory persistence is used THEN the system SHALL store Raft state and log entries in memory for testing and development scenarios
5. WHEN persistent state is written THEN the system SHALL ensure all critical Raft state (currentTerm, votedFor, log entries) is durably stored before responding to RPCs
6. WHEN servers restart THEN the system SHALL restore all persistent state from the storage backend
7. WHEN log compaction occurs THEN the system SHALL create snapshots and safely remove obsolete log entries from persistent storage

### Requirement 6

**User Story:** As a Raft cluster member, I want leader election functionality, so that the cluster can automatically select a leader and maintain availability during failures.

#### Acceptance Criteria

1. WHEN a follower's election timeout expires THEN the system SHALL transition to candidate state and initiate a new election
2. WHEN a candidate receives votes from a majority of servers THEN the system SHALL transition to leader state and begin sending heartbeats
3. WHEN multiple candidates compete simultaneously THEN the system SHALL use randomized election timeouts to prevent split votes
4. WHEN a candidate or leader discovers a higher term THEN the system SHALL immediately transition to follower state
5. WHEN election safety is evaluated THEN the system SHALL ensure at most one leader can be elected in any given term

### Requirement 7

**User Story:** As a Raft leader, I want log replication functionality, so that I can replicate client commands across the cluster and ensure consistency.

#### Acceptance Criteria

1. WHEN the leader receives a client command THEN the system SHALL append the command to its local log and replicate it to followers
2. WHEN AppendEntries RPCs are sent THEN the system SHALL include consistency check information to detect log inconsistencies
3. WHEN log inconsistencies are detected THEN the system SHALL force follower logs to match the leader's log by overwriting conflicting entries
4. WHEN a log entry is replicated to a majority of servers THEN the system SHALL mark the entry as committed and apply it to the state machine
5. WHEN the leader commits entries THEN the system SHALL ensure all preceding entries are also committed due to the Log Matching Property

### Requirement 8

**User Story:** As a system designer, I want safety guarantees, so that the replicated state machine maintains consistency even during failures and network partitions.

#### Acceptance Criteria

1. WHEN evaluating election restrictions THEN the system SHALL only elect leaders whose logs contain all committed entries from previous terms
2. WHEN comparing log up-to-dateness THEN the system SHALL use term number as primary criterion and log length as secondary criterion
3. WHEN committing entries from previous terms THEN the system SHALL only commit them indirectly by committing an entry from the current term
4. WHEN the State Machine Safety Property is evaluated THEN the system SHALL ensure no two servers apply different commands at the same log index
5. WHEN the Leader Completeness Property is evaluated THEN the system SHALL ensure leaders contain all committed entries from previous terms

### Requirement 9

**User Story:** As a cluster administrator, I want dynamic membership changes, so that I can add or remove servers from the cluster without downtime.

#### Acceptance Criteria

1. WHEN the membership manager concept is defined THEN the system SHALL support pluggable membership management implementations through C++ concepts
2. WHEN a configuration change is requested THEN the system SHALL use joint consensus to safely transition between configurations
3. WHEN in joint consensus mode THEN the system SHALL require majorities from both old and new configurations for all decisions
4. WHEN new servers are added THEN the system SHALL first add them as non-voting members until they catch up with the log
5. WHEN the leader is removed from the cluster THEN the system SHALL step down after committing the new configuration
6. WHEN removed servers attempt to disrupt elections THEN the system SHALL ignore RequestVote RPCs from servers not in the current configuration

### Requirement 10

**User Story:** As a performance-conscious developer, I want log compaction functionality, so that the system can manage storage efficiently and handle long-running deployments.

#### Acceptance Criteria

1. WHEN log size exceeds configured thresholds THEN the system SHALL create snapshots of the state machine state
2. WHEN snapshots are created THEN the system SHALL include last included index, term, and cluster configuration metadata
3. WHEN followers lag significantly behind THEN the system SHALL use InstallSnapshot RPCs to bring them up to date
4. WHEN snapshot installation occurs THEN the system SHALL handle partial snapshots and resume interrupted transfers
5. WHEN snapshots are completed THEN the system SHALL safely discard log entries covered by the snapshot

### Requirement 11

**User Story:** As a client application developer, I want linearizable read/write operations, so that my application can rely on strong consistency guarantees.

#### Acceptance Criteria

1. WHEN clients submit write commands THEN the system SHALL assign unique serial numbers to prevent duplicate execution
2. WHEN the leader processes read-only operations THEN the system SHALL ensure it has not been deposed by exchanging heartbeats with a majority
3. WHEN new leaders are elected THEN the system SHALL commit a no-op entry to learn which entries are committed
4. WHEN clients retry operations THEN the system SHALL detect duplicates using serial numbers and return cached responses
5. WHEN linearizable reads are requested THEN the system SHALL ensure the leader has the most recent committed state before responding

### Requirement 12

**User Story:** As a cluster administrator, I want pluggable membership management, so that I can customize how new nodes are discovered, validated, and integrated into the cluster based on my deployment environment.

#### Acceptance Criteria

1. WHEN the membership manager concept is defined THEN the system SHALL support pluggable membership management implementations through C++ concepts
2. WHEN concrete membership manager classes are implemented THEN the system SHALL ensure they conform to the membership_manager concept declaration
3. WHEN new nodes request to join the cluster THEN the system SHALL use the membership manager to validate and authenticate the joining nodes
4. WHEN cluster topology changes THEN the system SHALL use the membership manager to determine the appropriate cluster configuration
5. WHEN nodes are removed from the cluster THEN the system SHALL use the membership manager to handle cleanup and resource deallocation
6. WHEN membership policies are enforced THEN the system SHALL delegate policy decisions to the pluggable membership manager implementation

### Requirement 13

**User Story:** As a system operator, I want pluggable metrics collection, so that I can monitor Raft cluster performance and behavior in production environments.

#### Acceptance Criteria

1. WHEN the metrics concept is defined THEN the system SHALL support pluggable metrics implementations through C++ concepts
2. WHEN a metrics object is created THEN the system SHALL allow configuration of a metric name and an array of dimension name-value pairs
3. WHEN recording count metrics THEN the system SHALL provide a method to add one to the metric value
4. WHEN recording numeric metrics THEN the system SHALL provide a method to add an arbitrary number to the metric value
5. WHEN recording timing metrics THEN the system SHALL provide a method to add a timespan in nanoseconds to the metric value
6. WHEN recording gauge metrics THEN the system SHALL provide a method to add a double-precision floating point value to the metric
7. WHEN Raft operations occur THEN the system SHALL emit metrics for elections, log replication, commits, and RPC latencies

### Requirement 14

**User Story:** As a testing engineer, I want comprehensive property-based testing support, so that I can verify the correctness of the Raft implementation across diverse scenarios.

#### Acceptance Criteria

1. WHEN property-based tests are executed THEN the system SHALL verify all Raft safety properties hold across randomly generated scenarios
2. WHEN testing leader election THEN the system SHALL verify the Election Safety Property across various failure patterns
3. WHEN testing log replication THEN the system SHALL verify the Log Matching Property and Leader Append-Only Property
4. WHEN testing safety guarantees THEN the system SHALL verify the Leader Completeness Property and State Machine Safety Property
5. WHEN testing with network failures THEN the system SHALL verify liveness properties are restored when connectivity is reestablished
6. WHEN concrete metrics classes are implemented THEN the system SHALL ensure they conform to the metrics concept declaration

### Requirement 15

**User Story:** As a client application developer, I want my submitted commands to wait for actual commit and state machine application, so that I can be certain my operations are durable and visible before proceeding.

#### Acceptance Criteria

1. WHEN a client submits a command via submit_command THEN the system SHALL return a generic future type that completes only after the command is committed and applied to the state machine
2. WHEN a log entry is committed (replicated to majority) THEN the system SHALL apply it to the state machine before fulfilling any associated client futures
3. WHEN the state machine application fails THEN the system SHALL propagate the error to the associated client future
4. WHEN a leader loses leadership before committing an entry THEN the system SHALL reject the associated client future with an appropriate error
5. WHEN multiple commands are submitted concurrently THEN the system SHALL ensure they are applied to the state machine in log order

### Requirement 16

**User Story:** As a Raft node implementation, I want proper async future collection mechanisms, so that I can efficiently wait for multiple network operations without blocking the main thread.

#### Acceptance Criteria

1. WHEN sending heartbeats to multiple followers THEN the system SHALL collect all heartbeat futures using generic future collection mechanisms and wait for a majority response
2. WHEN conducting leader election THEN the system SHALL collect all vote request futures using generic future collection and determine election outcome based on majority votes
3. WHEN replicating log entries to followers THEN the system SHALL collect replication futures using generic future collection and update commit index based on majority acknowledgment
4. WHEN any future in a collection times out THEN the system SHALL handle the timeout appropriately without blocking other operations
5. WHEN a future collection operation is cancelled THEN the system SHALL properly clean up all pending futures

### Requirement 17

**User Story:** As a cluster administrator, I want configuration changes to be properly synchronized, so that the cluster safely transitions between configurations without violating safety properties.

#### Acceptance Criteria

1. WHEN adding a server to the cluster THEN the system SHALL wait for the joint consensus configuration to be committed before proceeding to the final configuration
2. WHEN removing a server from the cluster THEN the system SHALL wait for each configuration phase to be committed before proceeding to the next phase
3. WHEN a configuration change is in progress THEN the system SHALL prevent new configuration changes until the current change is complete
4. WHEN a configuration change fails during any phase THEN the system SHALL rollback to the previous stable configuration
5. WHEN the leader changes during a configuration change THEN the system SHALL properly handle the transition and continue or abort the change as appropriate

### Requirement 18

**User Story:** As a system operator, I want comprehensive error handling for network operations, so that the Raft cluster can gracefully handle network failures and maintain availability.

#### Acceptance Criteria

1. WHEN heartbeat RPCs fail due to network errors THEN the system SHALL retry with exponential backoff up to configured limits
2. WHEN AppendEntries RPCs fail THEN the system SHALL retry the operation and handle different failure modes appropriately
3. WHEN InstallSnapshot RPCs fail THEN the system SHALL retry snapshot transfer with proper error recovery
4. WHEN RequestVote RPCs fail during election THEN the system SHALL handle the failure and continue the election process
5. WHEN network partitions occur THEN the system SHALL detect the partition and handle it according to Raft safety requirements
6. WHEN RPC timeouts occur THEN the system SHALL distinguish between network delays and actual failures

### Requirement 19

**User Story:** As a Raft implementation, I want proper commit index advancement with state machine synchronization, so that committed entries are reliably applied in order.

#### Acceptance Criteria

1. WHEN the commit index advances THEN the system SHALL apply all entries between the old and new commit index to the state machine
2. WHEN applying entries to the state machine THEN the system SHALL ensure sequential application in log order
3. WHEN state machine application succeeds THEN the system SHALL update the applied index and fulfill any waiting client futures
4. WHEN state machine application fails THEN the system SHALL halt further application and report the error
5. WHEN the applied index lags behind the commit index THEN the system SHALL catch up by applying pending entries

### Requirement 20

**User Story:** As a leader node, I want proper replication waiting mechanisms, so that I can accurately determine when entries are committed based on majority replication.

#### Acceptance Criteria

1. WHEN replicating entries to followers THEN the system SHALL track which followers have acknowledged each entry
2. WHEN a majority of followers acknowledge an entry THEN the system SHALL advance the commit index to include that entry
3. WHEN followers are slow to respond THEN the system SHALL continue replication without blocking other operations
4. WHEN a follower consistently fails to respond THEN the system SHALL mark it as unavailable but continue with the majority
5. WHEN the leader's own entry acknowledgment is counted THEN the system SHALL include the leader in majority calculations

### Requirement 21

**User Story:** As a client of the read_state operation, I want linearizable read consistency with proper leader validation, so that I receive the most recent committed state.

#### Acceptance Criteria

1. WHEN a read_state operation is requested THEN the system SHALL verify leader status by collecting heartbeat responses from a majority
2. WHEN heartbeat collection succeeds THEN the system SHALL return the current state machine state
3. WHEN heartbeat collection fails THEN the system SHALL reject the read request with a leadership error
4. WHEN the leader loses leadership during a read operation THEN the system SHALL abort the read and return an error
5. WHEN multiple read operations are concurrent THEN the system SHALL handle them efficiently without unnecessary heartbeat overhead

### Requirement 22

**User Story:** As a Raft node, I want proper future cancellation and cleanup mechanisms, so that cancelled operations don't leak resources or cause undefined behavior.

#### Acceptance Criteria

1. WHEN a node shuts down THEN the system SHALL cancel all pending futures and clean up resources
2. WHEN a leader steps down THEN the system SHALL cancel all pending client operations with appropriate errors
3. WHEN an operation times out THEN the system SHALL cancel the associated future and clean up any related state
4. WHEN futures are cancelled THEN the system SHALL ensure no callbacks are invoked after cancellation
5. WHEN cleaning up futures THEN the system SHALL prevent memory leaks and resource exhaustion

### Requirement 23

**User Story:** As a system integrator, I want configurable timeout and retry policies, so that I can tune the Raft implementation for different network environments and performance requirements.

#### Acceptance Criteria

1. WHEN configuring RPC timeouts THEN the system SHALL allow separate timeout values for different RPC types
2. WHEN configuring retry policies THEN the system SHALL support exponential backoff with configurable parameters
3. WHEN configuring heartbeat intervals THEN the system SHALL ensure the interval is compatible with election timeouts
4. WHEN network conditions change THEN the system SHALL adapt timeout and retry behavior within configured bounds
5. WHEN timeout configurations are invalid THEN the system SHALL reject the configuration with clear error messages

### Requirement 24

**User Story:** As a developer debugging Raft issues, I want comprehensive error reporting and logging, so that I can diagnose and resolve problems in production environments.

#### Acceptance Criteria

1. WHEN RPC operations fail THEN the system SHALL log detailed error information including failure type, target node, and retry attempts
2. WHEN commit waiting times out THEN the system SHALL log the timeout with context about pending operations
3. WHEN configuration changes fail THEN the system SHALL log the failure reason and current cluster state
4. WHEN future collection operations encounter errors THEN the system SHALL log which futures failed and why
5. WHEN state machine application fails THEN the system SHALL log the failing entry and error details

### Requirement 25

**User Story:** As a Raft implementation, I want to use generic future concepts as template parameters, so that I can maintain concept compliance and type safety while providing unified async programming interfaces.

#### Acceptance Criteria

1. WHEN performing async operations THEN the system SHALL use generic future concepts as template parameters, with kythira::Future<T> wrapper classes as the default concrete implementation
2. WHEN setting future values THEN the system SHALL use generic promise concepts as template parameters, with kythira::Promise<T> as the default concrete implementation
3. WHEN handling results with exceptions THEN the system SHALL use generic try concepts as template parameters, with kythira::Try<T> as the default concrete implementation
4. WHEN collecting multiple futures THEN the system SHALL use generic future collector concepts as template parameters, with kythira::FutureCollector as the default concrete implementation
5. WHEN creating futures from values or exceptions THEN the system SHALL use generic future factory concepts as template parameters, with kythira::FutureFactory as the default concrete implementation

### Requirement 26

**User Story:** As a developer, I want to parameterize the raft consensus classes with a single types template argument, so that I can provide all necessary types through a clean, concept-based interface while maintaining type safety.

#### Acceptance Criteria

1. WHEN defining Raft node classes THEN the system SHALL accept a single types template parameter that encapsulates all required type information
2. WHEN the types parameter is provided THEN the system SHALL extract future types, network client types, network server types, and all other component types from the unified interface
3. WHEN type safety is evaluated THEN the system SHALL use concepts to validate that the types parameter provides all required type definitions
4. WHEN instantiating Raft components THEN the system SHALL use the types parameter to automatically deduce all necessary template arguments
5. WHEN developers use the Raft implementation THEN the system SHALL provide a clean, single-parameter interface that eliminates complex multi-parameter template instantiation