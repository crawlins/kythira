# Implementation Plan

- [x] 1. Set up project structure and build system
  - Create CMake build configuration with C++23 standard
  - Configure folly dependency
  - Set up directory structure (include/, src/, tests/, examples/)
  - Create main header files for concepts and core types
  - _Requirements: 10.1-10.8_

- [x] 2. Implement core concepts
  - [x] 2.1 Implement address concept
    - Define address concept with equality, hashing, copy/move semantics
    - _Requirements: 2.3-2.6, 10.1_
  
  - [x] 2.2 Implement port concept
    - Define port concept with equality, hashing, copy/move semantics
    - _Requirements: 2.7-2.8, 10.2_
  
  - [x] 2.3 Implement try_type concept
    - Define try_type concept with value(), exception(), has_value(), has_exception()
    - _Requirements: 10.4_
  
  - [x] 2.4 Implement future concept
    - Define future concept with get(), then(), on_error(), is_ready(), wait()
    - Implement wait_for_any() and wait_for_all() functions
    - _Requirements: 10.4_
  
  - [x] 2.5 Implement endpoint concept
    - Define endpoint concept with address(), port(), equality operators
    - _Requirements: 10.8_
  
  - [x] 2.6 Implement network_edge concept
    - Define network_edge concept with latency() and reliability()
    - _Requirements: 10.8_
  
  - [x] 2.7 Implement message concept
    - Define message concept with source/destination address/port and payload accessors
    - _Requirements: 10.3_
  
  - [x] 2.8 Implement connection concept
    - Define connection concept with read(), write(), close(), is_open()
    - _Requirements: 10.5_
  
  - [x] 2.9 Implement listener concept
    - Define listener concept with accept(), close(), is_listening()
    - _Requirements: 10.6_
  
  - [x] 2.10 Implement network_node concept
    - Define network_node concept with send(), receive(), connect(), bind()
    - _Requirements: 10.7_
  
  - [x] 2.11 Implement network_simulator concept
    - Define network_simulator concept with add_node(), add_edge(), create_node(), start(), stop(), reset()
    - _Requirements: 10.8_

- [x] 2.12 Write unit tests for concept satisfaction
    - Test that std::string, unsigned long, in_addr, in6_addr satisfy address concept
    - Test that unsigned short, std::string satisfy port concept
    - _Requirements: 2.3-2.8_

- [x] 3. Implement core data structures
  - [x] 3.1 Implement Message class
    - Create Message template class with address and port type parameters
    - Implement constructor with source/destination address/port and optional payload
    - Implement accessor methods
    - _Requirements: 9.1-9.5_
  
  - [x] 3.2 Implement NetworkEdge struct
    - Create NetworkEdge with latency and reliability fields
    - Implement accessor methods
    - _Requirements: 1.1, 1.2_
  
  - [x] 3.3 Implement Endpoint struct
    - Create Endpoint template with address and port
    - Implement equality operators and hash specialization
    - _Requirements: 9.1-9.4_

  - [x] 3.4 Implement Future wrapper and collective operations
    - Create Try<T> class template that wraps folly::Try and satisfies try_type concept
    - Create Future<T> wrapper that adapts folly::Future to satisfy future concept
    - Implement wait_for_any() function for waiting on any future to complete
    - Implement wait_for_all() function for waiting on all futures to complete
    - _Requirements: 10.4_

- [x] 3.5 Write unit tests for data structures
    - Test Message construction with various address/port types
    - Test Message with empty and non-empty payloads
    - Test Endpoint equality and hashing
    - Test Try and Future wrapper functionality
    - Test wait_for_any() and wait_for_all() functions
    - _Requirements: 9.1-9.5, 10.4_

- [x] 4. Implement NetworkSimulator core
  - [x] 4.1 Implement NetworkSimulator class skeleton
    - Create NetworkSimulator template class with address and port parameters
    - Implement topology management (add_node, remove_node, add_edge, remove_edge)
    - Initialize internal data structures (topology graph, message queues, connections, listeners)
    - _Requirements: 1.1, 1.2, 2.1, 2.2_
  
  - [x] 4.2 Implement simulation control methods
    - Implement start(), stop(), reset() methods
    - Set up thread pool executor for async operations
    - _Requirements: 1.3, 1.4_
  
  - [x] 4.3 Implement message routing logic
    - Implement route_message() to find paths in directed graph
    - Implement apply_latency() to calculate delays from edge weights
    - Implement check_reliability() using random number generation
    - _Requirements: 1.3, 1.4, 1.5_
  
  - [x] 4.4 Implement message delivery
    - Implement deliver_message() to queue messages at destination nodes
    - Handle message queuing with thread safety
    - _Requirements: 4.2, 5.2_

- [x] 4.5 Write property test for topology edge preservation
    - **Property 1: Topology Edge Latency Preservation**
    - **Validates: Requirements 1.1**

- [x] 4.6 Write property test for reliability preservation
    - **Property 2: Topology Edge Reliability Preservation**
    - **Validates: Requirements 1.2**

- [x] 4.7 Write unit tests for NetworkSimulator
    - Test add/remove nodes and edges
    - Test topology queries
    - Test simulation start/stop/reset
    - _Requirements: 1.1, 1.2_

- [x] 5. Implement NetworkNode class
  - [x] 5.1 Implement NetworkNode skeleton
    - Create NetworkNode template class
    - Implement constructor with address and simulator reference
    - Implement address() accessor
    - _Requirements: 2.1, 2.2_
  
  - [x] 5.2 Implement connectionless send operation
    - Implement send() methods with and without timeout
    - Return folly::Future<bool>
    - Handle timeout by returning false
    - _Requirements: 4.1, 4.2, 4.3_
  
  - [x] 5.3 Implement connectionless receive operation
    - Implement receive() methods with and without timeout
    - Return folly::Future<Message>
    - Handle timeout by throwing TimeoutException
    - _Requirements: 5.1, 5.2, 5.3_
  
  - [x] 5.4 Implement ephemeral port allocation
    - Implement allocate_ephemeral_port() to assign unique unused ports
    - Track used ports to avoid conflicts
    - _Requirements: 6.3, 7.3_

- [x] 5.5 Write property test for send success
    - **Property 6: Send Success Result**
    - **Validates: Requirements 4.2**

- [x] 5.6 Write property test for send timeout
    - **Property 7: Send Timeout Result**
    - **Validates: Requirements 4.3**

- [x] 5.7 Write property test for send non-delivery guarantee
    - **Property 8: Send Does Not Guarantee Delivery**
    - **Validates: Requirements 4.4**

- [x] 5.8 Write property test for receive message
    - **Property 9: Receive Returns Sent Message**
    - **Validates: Requirements 5.2**

- [x] 5.9 Write property test for receive timeout
    - **Property 10: Receive Timeout Exception**
    - **Validates: Requirements 5.3**

- [x] 6. Implement connection-oriented client operations
  - [x] 6.1 Implement connect() methods
    - Implement connect() with destination address/port
    - Implement connect() with source port parameter
    - Implement connect() with timeout parameter
    - Return folly::Future<Connection>
    - Handle timeout by throwing TimeoutException
    - _Requirements: 6.1, 6.2, 6.3, 6.4, 6.5_

- [x] 6.2 Write property test for connect with specified source port
    - **Property 11: Connect Uses Specified Source Port**
    - **Validates: Requirements 6.2**

- [x] 6.3 Write property test for ephemeral port uniqueness
    - **Property 12: Connect Assigns Unique Ephemeral Ports**
    - **Validates: Requirements 6.3**

- [x] 6.4 Write property test for successful connection
    - **Property 13: Successful Connection Returns Connection Object**
    - **Validates: Requirements 6.4**

- [x] 6.5 Write property test for connect timeout
    - **Property 14: Connect Timeout Exception**
    - **Validates: Requirements 6.5**

- [x] 7. Implement connection-oriented server operations
  - [x] 7.1 Implement bind() methods
    - Implement bind() without port (random port assignment)
    - Implement bind() with specific port
    - Implement bind() with timeout parameter
    - Return folly::Future<Listener>
    - Handle timeout by throwing TimeoutException
    - _Requirements: 7.1, 7.2, 7.3, 7.4, 7.5_

- [x] 7.2 Write property test for successful bind
    - **Property 15: Successful Bind Returns Listener**
    - **Validates: Requirements 7.2**

- [x] 7.3 Write property test for bind timeout
    - **Property 16: Bind Timeout Exception**
    - **Validates: Requirements 7.3**

- [x] 8. Implement Connection class
  - [x] 8.1 Implement Connection skeleton
    - Create Connection template class
    - Implement constructor with local/remote endpoints and simulator reference
    - Implement local_endpoint() and remote_endpoint() accessors
    - Implement is_open() method
    - _Requirements: 8.1, 8.2_
  
  - [x] 8.2 Implement read() methods
    - Implement read() with and without timeout
    - Return folly::Future<std::vector<std::byte>>
    - Handle timeout by throwing TimeoutException
    - Use condition variable for blocking on data availability
    - _Requirements: 8.1, 8.2, 8.3_
  
  - [x] 8.3 Implement write() methods
    - Implement write() with and without timeout
    - Return folly::Future<bool>
    - Route data through simulator with latency/reliability
    - Handle timeout by throwing TimeoutException
    - _Requirements: 8.4, 8.5, 8.6_
  
  - [x] 8.4 Implement close() method
    - Set connection state to closed
    - Wake up any blocked read operations
    - _Requirements: 8.1_

- [x] 8.5 Write property test for connection read-write round trip
    - **Property 19: Connection Read-Write Round Trip**
    - **Validates: Requirements 8.2**

- [x] 8.6 Write property test for read timeout
    - **Property 20: Read Timeout Exception**
    - **Validates: Requirements 8.3**

- [x] 8.7 Write property test for successful write
    - **Property 21: Successful Write Returns True**
    - **Validates: Requirements 8.5**

- [x] 8.8 Write property test for write timeout
    - **Property 22: Write Timeout Exception**
    - **Validates: Requirements 8.6**

- [x] 9. Implement Listener class
  - [x] 9.1 Implement Listener skeleton
    - Create Listener template class
    - Implement constructor with local endpoint and simulator reference
    - Implement local_endpoint() accessor
    - Implement is_listening() method
    - _Requirements: 7.1, 7.2_
  
  - [x] 9.2 Implement accept() methods
    - Implement accept() with and without timeout
    - Return folly::Future<Connection>
    - Handle timeout by throwing TimeoutException
    - Use condition variable for blocking on pending connections
    - _Requirements: 7.4, 7.5, 7.6_
  
  - [x] 9.3 Implement close() method
    - Set listener state to not listening
    - Wake up any blocked accept operations
    - _Requirements: 7.1_

- [x] 9.4 Write property test for accept on client connect
    - **Property 17: Accept Returns Connection on Client Connect**
    - **Validates: Requirements 7.5**

- [x] 9.5 Write property test for accept timeout
    - **Property 18: Accept Timeout Exception**
    - **Validates: Requirements 7.6**

- [x] 10. Implement exception types
  - Create NetworkException base class
  - Create TimeoutException
  - Create ConnectionClosedException
  - Create PortInUseException
  - Create NodeNotFoundException
  - Create NoRouteException
  - _Requirements: 4.3, 5.3, 6.5, 7.3, 7.6, 8.3, 8.6_

- [x] 11. Implement latency and reliability simulation
  - [x] 11.1 Implement latency simulation
    - Use timer-based scheduling for message delivery delays
    - Apply edge latency weights to message routing
    - _Requirements: 1.3_
  
  - [x] 11.2 Implement reliability simulation
    - Use std::bernoulli_distribution for probabilistic message drops
    - Apply edge reliability weights to message routing
    - Seed RNG for reproducible tests
    - _Requirements: 1.4_

- [x] 11.3 Write property test for latency application
    - **Property 3: Latency Application**
    - **Validates: Requirements 1.3**

- [x] 11.4 Write property test for reliability application
    - **Property 4: Reliability Application**
    - **Validates: Requirements 1.4**

- [x] 11.5 Write property test for graph-based routing
    - **Property 5: Graph-Based Routing**
    - **Validates: Requirements 1.5**

- [x] 12. Implement thread safety
  - Add std::shared_mutex to NetworkSimulator for topology access
  - Add mutexes to Connection for buffer access
  - Add mutexes to Listener for pending connection queue
  - Add mutexes to NetworkNode for port allocation
  - Use condition variables for blocking operations
  - _Requirements: All async operations_

- [x] 13. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.

- [x] 14. Create example programs
  - [x] 14.1 Create basic connectionless example
    - Demonstrate send/receive operations
    - Show timeout handling
    - Follow example program guidelines (run all scenarios, clear pass/fail, exit codes)
    - _Requirements: 4.1-4.4, 5.1-5.3_
  
  - [x] 14.2 Create connection-oriented client-server example
    - Demonstrate bind/listen/accept on server
    - Demonstrate connect on client
    - Show read/write operations
    - Follow example program guidelines
    - _Requirements: 6.1-6.5, 7.1-7.6, 8.1-8.6_
  
  - [x] 14.3 Create network topology example
    - Demonstrate topology configuration with latency/reliability
    - Show message routing through multi-hop paths
    - Demonstrate reliability-based message drops
    - Follow example program guidelines
    - _Requirements: 1.1-1.5_
  
  - [x] 14.4 Add examples to CMake build system
    - Create examples/CMakeLists.txt
    - Add executable targets for each example
    - Link against network simulator library
    - Set output directory to build/examples/
    - _Requirements: All_

- [x] 15. Integration testing
  - [x] 15.1 Write integration test for client-server communication
    - Full connection establishment, data transfer, and teardown
    - _Requirements: 6.1-6.5, 7.1-7.6, 8.1-8.6_
  
  - [x] 15.2 Write integration test for multi-node topology
    - Messages routed through intermediate nodes
    - _Requirements: 1.1-1.5_
  
  - [x] 15.3 Write integration test for concurrent operations
    - Multiple nodes sending/receiving simultaneously
    - _Requirements: All async operations_

- [x] 16. Final checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.
