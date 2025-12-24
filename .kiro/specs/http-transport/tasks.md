# Implementation Plan

- [x] 1. Set up project structure and dependencies
  - Create header files for HTTP transport components
  - Add cpp-httplib dependency to CMakeLists.txt
  - Add OpenSSL dependency for TLS support
  - Create exception types header
  - _Requirements: 1.4_

- [x] 2. Implement exception types
  - Create http_transport_error base exception class
  - Create http_client_error exception with status code
  - Create http_server_error exception with status code
  - Create http_timeout_error exception
  - Create serialization_error exception
  - _Requirements: 13.4, 13.5, 12.5_

- [x] 3. Implement configuration structures
  - Create cpp_httplib_client_config struct with connection pool size, timeouts, TLS settings
  - Create cpp_httplib_server_config struct with max connections, request size limits, TLS settings
  - _Requirements: 14.1, 14.2, 14.3, 14.4, 14.5, 14.6, 14.7_

- [x] 4. Implement cpp_httplib_client class
  - [x] 4.1 Create class template with RPC_Serializer parameter
    - Define class template with RPC_Serializer and Metrics template parameters
    - Add concept constraint for rpc_serializer
    - Add concept constraint for metrics
    - Add private members (serializer, node_id_to_url, http_client, config, metrics, mutex)
    - _Requirements: 2.1, 2.3, 16.1_

  - [x] 4.2 Implement constructor
    - Accept node_id_to_url_map, config, and metrics parameters
    - Initialize serializer instance
    - Initialize http_client with connection pooling
    - Store configuration
    - Store metrics instance
    - _Requirements: 14.1, 14.2, 14.3, 14.4, 16.1_

  - [x] 4.3 Implement send_request_vote method
    - Accept target node ID, request, and timeout
    - Look up base URL for target node
    - Serialize request using RPC_Serializer
    - Construct HTTP POST request to /v1/raft/request_vote
    - Set Content-Type, Content-Length, and User-Agent headers
    - Send request with timeout
    - Deserialize response on success
    - Return folly::Future with response or error
    - Emit metrics for request count, latency, size
    - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 15.1, 15.2, 15.3, 16.3_

  - [x] 4.4 Implement send_append_entries method
    - Accept target node ID, request, and timeout
    - Look up base URL for target node
    - Serialize request using RPC_Serializer
    - Construct HTTP POST request to /v1/raft/append_entries
    - Set Content-Type, Content-Length, and User-Agent headers
    - Send request with timeout
    - Deserialize response on success
    - Return folly::Future with response or error
    - Emit metrics for request count, latency, size
    - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5, 15.1, 15.2, 15.3, 16.3_

  - [x] 4.5 Implement send_install_snapshot method
    - Accept target node ID, request, and timeout
    - Look up base URL for target node
    - Serialize request using RPC_Serializer
    - Construct HTTP POST request to /v1/raft/install_snapshot
    - Set Content-Type, Content-Length, and User-Agent headers
    - Send request with timeout
    - Deserialize response on success
    - Return folly::Future with response or error
    - Emit metrics for request count, latency, size
    - _Requirements: 5.1, 5.2, 5.3, 5.4, 5.5, 15.1, 15.2, 15.3, 16.3_

  - [x] 4.6 Implement error handling for client
    - Handle connection failures and set future to error
    - Handle timeouts and set future to error with http_timeout_error
    - Handle 4xx status codes and set future to error with http_client_error
    - Handle 5xx status codes and set future to error with http_server_error
    - Handle deserialization failures and set future to error with serialization_error
    - Emit error metrics with appropriate error types
    - _Requirements: 13.4, 13.5, 13.6, 12.4, 12.5, 16.5_

  - [x] 4.7 Implement connection pooling
    - Configure cpp-httplib client with connection pool size
    - Enable HTTP keep-alive headers
    - Implement connection reuse for same target
    - Implement idle connection cleanup
    - Emit connection lifecycle metrics
    - _Requirements: 11.1, 11.2, 11.3, 11.4, 11.5, 16.6_

  - [x] 4.8 Implement TLS/HTTPS support for client
    - Detect HTTPS URLs and enable TLS
    - Configure certificate validation
    - Set CA cert path if provided
    - Enforce TLS 1.2 or higher
    - Handle certificate validation failures
    - _Requirements: 10.1, 10.3, 10.4, 10.5_

  - [x] 4.9 Write property test for POST method
    - **Property 1: POST method for all RPCs**
    - **Validates: Requirements 1.6**

  - [x] 4.10 Write property test for Content-Type header
    - **Property 3: Content-Type header matches serializer format**
    - **Validates: Requirements 2.9, 15.1, 15.4**

  - [x] 4.11 Write property test for Content-Length header
    - **Property 4: Content-Length header for requests**
    - **Validates: Requirements 15.2**

  - [x] 4.12 Write property test for User-Agent header
    - **Property 5: User-Agent header for requests**
    - **Validates: Requirements 15.3**

  - [x] 4.13 Write property test for connection reuse
    - **Property 8: Connection reuse for same target**
    - **Validates: Requirements 11.2**

  - [x] 4.14 Write property test for 4xx error handling
    - **Property 9: 4xx status codes produce client errors**
    - **Validates: Requirements 13.4**

  - [x] 4.15 Write property test for 5xx error handling
    - **Property 10: 5xx status codes produce server errors**
    - **Validates: Requirements 13.5**

  - [x] 4.16 Write unit tests for client
    - Test client conforms to network_client concept
    - Test client requires rpc_serializer concept
    - Test successful request/response for each RPC type
    - Test timeout enforcement
    - Test connection failure handling
    - Test URL mapping
    - Test HTTPS support
    - Test metrics emission
    - _Requirements: 1.1, 2.3, 3.4, 3.5, 4.4, 4.5, 5.4, 5.5, 10.1, 12.1, 12.2, 12.3, 16.3_

- [x] 5. Implement cpp_httplib_server class
  - [x] 5.1 Create class template with RPC_Serializer parameter
    - Define class template with RPC_Serializer and Metrics template parameters
    - Add concept constraint for rpc_serializer
    - Add concept constraint for metrics
    - Add private members (serializer, http_server, handlers, bind_address, bind_port, config, metrics, running, mutex)
    - _Requirements: 2.2, 2.4, 16.2_

  - [x] 5.2 Implement constructor
    - Accept bind_address, bind_port, config, and metrics parameters
    - Initialize serializer instance
    - Initialize http_server instance
    - Store configuration
    - Store metrics instance
    - Initialize running flag to false
    - _Requirements: 14.5, 14.6, 14.7, 16.2_

  - [x] 5.3 Implement register_request_vote_handler method
    - Accept handler function for RequestVote RPCs
    - Store handler in member variable
    - _Requirements: 6.1_

  - [x] 5.4 Implement register_append_entries_handler method
    - Accept handler function for AppendEntries RPCs
    - Store handler in member variable
    - _Requirements: 7.1_

  - [x] 5.5 Implement register_install_snapshot_handler method
    - Accept handler function for InstallSnapshot RPCs
    - Store handler in member variable
    - _Requirements: 8.1_

  - [x] 5.6 Implement start method
    - Bind to configured address and port
    - Set up endpoint handlers for /v1/raft/request_vote, /v1/raft/append_entries, /v1/raft/install_snapshot
    - Start accepting HTTP connections
    - Set running flag to true
    - Emit server started metric
    - _Requirements: 9.1, 9.2, 16.7_

  - [x] 5.7 Implement stop method
    - Stop accepting new connections
    - Wait for in-flight requests to complete
    - Shut down http_server
    - Set running flag to false
    - Emit server stopped metric
    - _Requirements: 9.3, 9.4, 16.7_

  - [x] 5.8 Implement is_running method
    - Return current value of running flag
    - _Requirements: 9.5_

  - [x] 5.9 Implement endpoint handler for RequestVote
    - Extract request body from HTTP request
    - Deserialize request using RPC_Serializer
    - Invoke registered RequestVote handler
    - Serialize response using RPC_Serializer
    - Set Content-Type and Content-Length headers
    - Return HTTP 200 with serialized response
    - Emit metrics for request count, latency, size
    - _Requirements: 6.2, 6.3, 6.4, 6.5, 15.4, 15.5, 16.4_

  - [x] 5.10 Implement endpoint handler for AppendEntries
    - Extract request body from HTTP request
    - Deserialize request using RPC_Serializer
    - Invoke registered AppendEntries handler
    - Serialize response using RPC_Serializer
    - Set Content-Type and Content-Length headers
    - Return HTTP 200 with serialized response
    - Emit metrics for request count, latency, size
    - _Requirements: 7.2, 7.3, 7.4, 7.5, 15.4, 15.5, 16.4_

  - [x] 5.11 Implement endpoint handler for InstallSnapshot
    - Extract request body from HTTP request
    - Deserialize request using RPC_Serializer
    - Invoke registered InstallSnapshot handler
    - Serialize response using RPC_Serializer
    - Set Content-Type and Content-Length headers
    - Return HTTP 200 with serialized response
    - Emit metrics for request count, latency, size
    - _Requirements: 8.2, 8.3, 8.4, 8.5, 15.4, 15.5, 16.4_

  - [x] 5.12 Implement error handling for server
    - Handle malformed requests and return 400 Bad Request
    - Handle deserialization failures and return 400 Bad Request with error details
    - Handle handler exceptions and return 500 Internal Server Error
    - Handle oversized requests and return 413 Payload Too Large
    - Handle too many connections and return 503 Service Unavailable
    - Emit error metrics with appropriate error types
    - _Requirements: 13.1, 13.2, 13.3, 16.5_

  - [x] 5.13 Implement TLS/HTTPS support for server
    - Configure TLS if enable_ssl is true
    - Load SSL certificate and key from configured paths
    - Accept HTTPS connections
    - Enforce TLS 1.2 or higher
    - _Requirements: 10.2, 10.5_

  - [x] 5.14 Write property test for handler invocation
    - **Property 7: Handler invocation for all RPCs**
    - **Validates: Requirements 6.3, 7.3, 8.3**

  - [x] 5.15 Write property test for Content-Length header
    - **Property 6: Content-Length header for responses**
    - **Validates: Requirements 15.5**

  - [x] 5.16 Write unit tests for server
    - Test server conforms to network_server concept
    - Test server requires rpc_serializer concept
    - Test handler registration for each RPC type
    - Test successful request/response handling for each RPC type
    - Test server lifecycle (start, stop, is_running)
    - Test error handling (malformed request, deserialization failure, handler exception)
    - Test HTTPS support
    - Test metrics emission
    - _Requirements: 1.2, 2.4, 6.1, 6.5, 7.1, 7.5, 8.1, 8.5, 9.1, 9.2, 9.3, 9.4, 9.5, 10.2, 13.1, 13.2, 13.3, 16.4_

- [x] 6. Integration testing
  - [x] 6.1 Write property test for serialization round-trip
    - **Property 2: Serialization round-trip preserves content**
    - **Validates: Requirements 16.2, 2.5, 2.6, 2.7, 2.8**

  - [x] 6.2 Write integration test for client-server communication
    - Create client and server instances with JSON serializer
    - Test RequestVote RPC end-to-end
    - Test AppendEntries RPC end-to-end
    - Test InstallSnapshot RPC end-to-end
    - Verify metrics are emitted correctly
    - _Requirements: 3.2, 3.4, 4.2, 4.4, 5.2, 5.4, 16.3, 16.4_

  - [x] 6.3 Write integration test for concurrent requests
    - Send multiple concurrent requests from client to server
    - Verify all requests are handled correctly
    - Verify connection pooling works correctly
    - _Requirements: 11.2, 17.4_

  - [x] 6.4 Write integration test for TLS/HTTPS
    - Create client and server with TLS enabled
    - Test successful HTTPS communication
    - Test certificate validation failure
    - _Requirements: 10.1, 10.2, 10.3, 10.4_

- [x] 7. Documentation and examples
  - [x] 7.1 Create example program demonstrating HTTP transport usage
    - Create example with JSON serializer
    - Show client-server setup
    - Show all three RPC types
    - Show error handling
    - Show metrics collection
    - Follow example program guidelines (comprehensive scenario coverage, exit codes)
    - _Requirements: All_

  - [x] 7.2 Update README with HTTP transport documentation
    - Add HTTP transport to features list
    - Add usage example
    - Add configuration documentation
    - Add TLS/HTTPS setup instructions
    - _Requirements: All_

- [x] 8. Checkpoint - Ensure all tests pass
  - Ensure all tests pass, ask the user if questions arise.
