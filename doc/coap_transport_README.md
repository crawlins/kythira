# CoAP Transport for Raft Consensus

## Overview

The CoAP transport implementation provides a lightweight, efficient communication layer for Raft consensus systems using the Constrained Application Protocol (CoAP). This transport is particularly well-suited for IoT environments, constrained networks, and scenarios requiring low-overhead, UDP-based communication with optional DTLS security.

## Key Features

- **Lightweight Protocol**: CoAP over UDP with minimal overhead
- **Security**: Optional DTLS encryption with certificate or PSK authentication
- **Reliability**: Confirmable messages with automatic retransmission
- **Scalability**: Block-wise transfer for large messages
- **Flexibility**: Pluggable serialization formats (JSON, CBOR, custom)
- **Performance**: Optimized for low latency and high throughput
- **Standards Compliance**: Full RFC 7252 (CoAP) and RFC 6347 (DTLS) compliance

## Quick Start

### Basic Setup

```cpp
#include <raft/coap_transport.hpp>
#include <raft/json_serializer.hpp>

// Configure endpoints
std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coap://node1:5683"},
    {2, "coap://node2:5683"},
    {3, "coap://node3:5683"}
};

// Create client and server
auto client = coap_client<json_serializer>(endpoints, client_config, metrics);
auto server = coap_server<json_serializer>("0.0.0.0", 5683, server_config, metrics);

// Register handlers and start
server.register_request_vote_handler(vote_handler);
server.start();

// Send requests
auto future = client.send_request_vote(2, request, timeout);
```

### Secure Setup (CoAPS)

```cpp
// Configure DTLS
coap_client_config config;
config.enable_dtls = true;
config.cert_file = "/path/to/cert.pem";
config.key_file = "/path/to/key.pem";
config.ca_file = "/path/to/ca.pem";

// Use CoAPS endpoints
std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coaps://node1:5684"},
    {2, "coaps://node2:5684"},
    {3, "coaps://node3:5684"}
};
```

## Documentation

### Core Documentation

1. **[API Documentation](coap_transport_api.md)** - Complete API reference with examples
   - Client and server classes
   - Configuration options
   - Error handling
   - Advanced usage patterns

2. **[DTLS Configuration Guide](coap_dtls_configuration.md)** - Security setup and best practices
   - Certificate-based authentication
   - Pre-shared key (PSK) setup
   - Production security guidelines
   - Troubleshooting security issues

3. **[Performance Tuning Guide](coap_performance_tuning.md)** - Optimization recommendations
   - Configuration tuning
   - System-level optimizations
   - Monitoring and profiling
   - Capacity planning

4. **[Troubleshooting Guide](coap_troubleshooting.md)** - Diagnostic procedures and solutions
   - Common issues and fixes
   - Systematic troubleshooting process
   - Monitoring and alerting
   - Emergency procedures

### Architecture Overview

```
┌─────────────────────────────────────────────────────────────┐
│                        Raft Node                             │
└────────────┬──────────────────────────────┬─────────────────┘
             │                              │
             ▼                              ▼
┌────────────────────────┐      ┌────────────────────────────┐
│     coap_client        │      │      coap_server           │
│  <RPC_Serializer>      │      │   <RPC_Serializer>         │
└────────┬───────────────┘      └────────┬───────────────────┘
         │                               │
         ▼                               ▼
┌────────────────────────────────────────────────────────────┐
│                    libcoap / CoAP Library                   │
│  (CoAP protocol implementation with DTLS support)          │
└────────────────────────────────────────────────────────────┘
```

## Protocol Details

### CoAP Resources

- **RequestVote**: `POST /raft/request_vote`
- **AppendEntries**: `POST /raft/append_entries`
- **InstallSnapshot**: `POST /raft/install_snapshot`

### Message Types

- **Confirmable (CON)**: Reliable delivery with acknowledgment
- **Non-confirmable (NON)**: Best-effort delivery
- **Acknowledgment (ACK)**: Confirms receipt of confirmable message
- **Reset (RST)**: Indicates processing error

### Security Options

- **Plain CoAP**: Port 5683, no encryption
- **CoAPS (DTLS)**: Port 5684, encrypted communication
- **Certificate Auth**: X.509 certificates with PKI
- **PSK Auth**: Pre-shared keys for closed networks

## Configuration Examples

### Development Environment

```cpp
// Simple configuration for testing
coap_client_config dev_config;
dev_config.ack_timeout = std::chrono::milliseconds{1000};
dev_config.max_retransmit = 3;
dev_config.enable_dtls = false;  // Plain CoAP for development

std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coap://localhost:5683"},
    {2, "coap://localhost:5684"},
    {3, "coap://localhost:5685"}
};
```

### Production Environment

```cpp
// Production-ready configuration
coap_client_config prod_config;
prod_config.ack_timeout = std::chrono::milliseconds{2000};
prod_config.max_retransmit = 4;
prod_config.enable_dtls = true;
prod_config.cert_file = "/etc/raft/certs/node.crt";
prod_config.key_file = "/etc/raft/private/node.key";
prod_config.ca_file = "/etc/raft/certs/ca.crt";
prod_config.verify_peer_cert = true;
prod_config.max_sessions = 200;
prod_config.session_timeout = std::chrono::seconds{1800};

std::unordered_map<std::uint64_t, std::string> endpoints = {
    {1, "coaps://node1.example.com:5684"},
    {2, "coaps://node2.example.com:5684"},
    {3, "coaps://node3.example.com:5684"}
};
```

### High-Performance Configuration

```cpp
// Optimized for high throughput
coap_client_config perf_config;
perf_config.ack_timeout = std::chrono::milliseconds{500};
perf_config.max_retransmit = 2;
perf_config.max_sessions = 500;
perf_config.max_block_size = 4096;
perf_config.enable_block_transfer = true;

coap_server_config server_config;
server_config.max_concurrent_sessions = 1000;
server_config.max_request_size = 256 * 1024;  // 256KB
server_config.max_block_size = 4096;
```

## Performance Characteristics

### Typical Metrics

| Scenario | Latency | Throughput | Memory Usage |
|----------|---------|------------|--------------|
| Local Network | 1-5ms | 5000-10000 req/sec | 2-5MB |
| WAN | 50-200ms | 1000-5000 req/sec | 5-10MB |
| With DTLS | +10-50ms | 80-90% of plain CoAP | +20-30% |
| Block Transfer | Variable | 10-100 MB/sec | Block size dependent |

### Optimization Guidelines

1. **Low Latency**: Reduce `ack_timeout`, use smaller block sizes
2. **High Throughput**: Increase session limits, use larger blocks
3. **Memory Constrained**: Reduce session limits, smaller blocks
4. **Secure**: Enable DTLS with session resumption

## Comparison with Other Transports

| Feature | CoAP/UDP | HTTP/TCP | gRPC/HTTP2 |
|---------|----------|----------|------------|
| Connection Overhead | None | High | Medium |
| Message Size | 4-8 bytes | 200-500 bytes | 100-200 bytes |
| Latency | Lowest | Medium | Medium |
| Throughput | Medium | Highest | High |
| Security | DTLS | TLS | TLS |
| Complexity | Low | Medium | High |
| IoT Suitability | Excellent | Poor | Fair |

## Dependencies

### Required Libraries

- **libcoap**: CoAP protocol implementation (v4.3.0+)
- **OpenSSL**: DTLS/TLS support (v1.1.1+)
- **folly**: Asynchronous operations (v2023.01.01+)

### Build Requirements

```cmake
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBCOAP REQUIRED libcoap-3)

target_link_libraries(your_target PRIVATE
    ${LIBCOAP_LIBRARIES}
    Folly::folly
    OpenSSL::SSL
    OpenSSL::Crypto
)
```

## Testing

### Unit Tests

Run the comprehensive test suite:

```bash
cd build
ctest -R coap_.*_test --verbose
```

### Integration Tests

Test end-to-end scenarios:

```bash
cd build
ctest -R coap_integration_test --verbose
```

### Property-Based Tests

Verify correctness properties:

```bash
cd build
ctest -R coap_.*_property_test --verbose
```

### Example Programs

Run example programs to verify functionality:

```bash
cd build
ctest -L example --verbose
```

## Monitoring and Observability

### Key Metrics

- **Request Metrics**: Success rate, latency percentiles, throughput
- **Connection Metrics**: Active sessions, handshake success rate
- **Error Metrics**: Timeout rate, retransmission rate, security failures
- **Resource Metrics**: Memory usage, CPU utilization, network bandwidth

### Logging

Enable comprehensive logging for troubleshooting:

```cpp
// Set log level for detailed diagnostics
auto logger = std::make_shared<console_logger>(log_level::debug);
coap_set_log_level(LOG_DEBUG);  // Enable libcoap debugging
```

### Health Checks

Implement health check endpoints:

```cpp
auto health_status = health_checker.check_health(client, target_nodes);
if (!health_status.overall_healthy) {
    // Handle unhealthy state
    for (const auto& issue : health_status.issues) {
        logger->error("Health check issue: {}", issue);
    }
}
```

## Best Practices

### Security

1. **Always use DTLS in production**
2. **Implement proper certificate validation**
3. **Use strong cipher suites**
4. **Regularly rotate certificates and keys**
5. **Monitor security events**

### Performance

1. **Tune timeouts for your network conditions**
2. **Use appropriate block sizes**
3. **Monitor and adjust session limits**
4. **Enable session resumption for DTLS**
5. **Use efficient serialization formats**

### Reliability

1. **Implement proper error handling**
2. **Use confirmable messages for critical operations**
3. **Monitor timeout and retransmission rates**
4. **Implement circuit breaker patterns**
5. **Plan for network partitions**

### Operations

1. **Implement comprehensive monitoring**
2. **Set up alerting for key metrics**
3. **Document runbooks for common issues**
4. **Test disaster recovery procedures**
5. **Maintain up-to-date documentation**

## Support and Contributing

### Getting Help

1. Check the [Troubleshooting Guide](coap_troubleshooting.md) for common issues
2. Review the [API Documentation](coap_transport_api.md) for usage questions
3. Consult the [Performance Guide](coap_performance_tuning.md) for optimization
4. Check the [DTLS Guide](coap_dtls_configuration.md) for security setup

### Reporting Issues

When reporting issues, please include:

- CoAP transport version
- libcoap version
- Operating system and version
- Network configuration
- Relevant log messages
- Steps to reproduce

### Contributing

Contributions are welcome! Please:

1. Follow the existing code style
2. Add tests for new functionality
3. Update documentation as needed
4. Ensure all tests pass

## License

This CoAP transport implementation is part of the Raft consensus library and follows the same licensing terms.

## Changelog

### Version 1.0.0
- Initial release with full CoAP/DTLS support
- Certificate and PSK authentication
- Block-wise transfer for large messages
- Comprehensive test suite and documentation

---

For detailed information on any aspect of the CoAP transport, please refer to the specific documentation files linked above.