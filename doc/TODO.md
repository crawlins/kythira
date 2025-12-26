## TODO: Outstanding Tasks and Improvements

### High Priority - Core Implementation

#### Raft Implementation Completion
- [ ] **Complete commit waiting mechanism** (include/raft/raft.hpp:858)
  - Implement proper waiting for entry commit and state machine application
- [ ] **Generic future collection mechanism** (include/raft/raft.hpp:1180, 2845)
  - Replace temporary fixes with proper async future handling for heartbeats and elections
- [ ] **Configuration change waiting** (include/raft/raft.hpp:1364)
  - Implement proper configuration change waiting mechanism
- [ ] **Error handling for RPC operations** (include/raft/raft.hpp:3120, 3390, 3502)
  - Add error handling for heartbeat, AppendEntries, and InstallSnapshot failures

#### Network Simulator Improvements
- [ ] **Generic executor abstraction** (include/network_simulator/simulator.hpp:27, 88)
  - Replace hardcoded folly::CPUThreadPoolExecutor with generic executor concept
- [ ] **Generic future delay mechanism** (include/network_simulator/simulator.hpp:339)
  - Implement proper latency simulation for generic futures
- [ ] **Message waiting mechanisms** (include/network_simulator/simulator.hpp:406, 426)
  - Implement proper message waiting and timeout handling
- [ ] **Timeout handling for generic futures** (include/network_simulator/simulator.hpp:599)
  - Add proper timeout support for connection establishment

#### Test Infrastructure
- [ ] **Re-enable future constraint tests** (tests/network_concept_compliance_property_test.cpp:243)
  - Fix future constraints implementation and re-enable disabled tests

### Medium Priority - Feature Completion

#### RPC Encoders/Decoders
✅ JSON
- [ ] **CBOR** - Binary encoding for efficient serialization
- [ ] **Protocol Buffers** - Schema-based serialization

#### Compressors/Decompressors
- [ ] **Zstd** - High-performance compression
- [ ] **Zlib** - Standard compression support

#### Transport Layer
✅ HTTP - Basic implementation complete
- [ ] **CoAP Transport** - Complete implementation (currently stub)
  - [ ] Block transfer support
  - [ ] Multicast delivery
  - [ ] DTLS connection establishment
  - [ ] Certificate validation
  - [ ] Confirmable/Non-confirmable messages
  - [ ] Content format negotiation
  - [ ] Connection pooling and reuse
  - [ ] Concurrent request processing

### Low Priority - Testing and Integration

#### Multi-Node Testing
- [ ] **Multi-node Raft cluster testing**
  - Current tests use simplified single-node implementations
  - Need proper cluster initialization and membership management
- [ ] **Network partition and recovery testing**
  - Test Raft behavior under network failures
- [ ] **Cross-node communication validation**
  - Verify RPC serialization and network transport integration

#### HTTP Transport Enhancements
- [ ] **HTTP client property tests** - Currently documented stubs
- [ ] **HTTP server property tests** - Currently documented stubs
- [ ] **Connection pooling optimization**
- [ ] **Keep-alive and persistent connection management**

#### Performance and Reliability
- [ ] **Performance benchmarking framework**
- [ ] **Memory usage optimization**
- [ ] **Resource leak detection and prevention**
- [ ] **Stress testing under high load**

### Architecture Improvements

#### Generic Programming Support
- [ ] **Concept-based executor abstraction**
  - Make system work with different executor implementations
- [ ] **Future type abstraction improvements**
  - Better support for different future implementations
- [ ] **Serializer concept refinement**
  - More flexible serialization framework

#### Error Handling and Resilience
- [ ] **Comprehensive error handling strategy**
  - Consistent error propagation across all components
- [ ] **Graceful degradation mechanisms**
  - System behavior under partial failures
- [ ] **Recovery and retry logic**
  - Automatic recovery from transient failures

### Documentation and Examples
- [ ] **API documentation completion**
- [ ] **Usage examples for all major features**
- [ ] **Migration guides for API changes**
- [ ] **Performance tuning guidelines**

### Notes
- Many CoAP tests are currently stub implementations that verify interfaces exist
- HTTP tests require actual server setup and are currently documented stubs
- Raft implementation has several temporary fixes that bypass proper async handling
- Network simulator needs generic future support throughout
