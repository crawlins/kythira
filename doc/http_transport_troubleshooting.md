# HTTP Transport Troubleshooting Guide

This guide helps diagnose and resolve common issues with the HTTP transport implementation for Raft consensus.

## Table of Contents

- [Connection Problems](#connection-problems)
- [Server Request and Connection Limits](#server-request-and-connection-limits)
- [TLS/Certificate Issues](#tlscertificate-issues)
- [Performance Issues](#performance-issues)
- [Serialization Problems](#serialization-problems)
- [Configuration Issues](#configuration-issues)
- [Debugging Tools](#debugging-tools)
- [Common Error Messages](#common-error-messages)

## Connection Problems

### Connection Refused

**Symptoms:**
- Client receives "Connection refused" errors
- RPCs fail immediately without timeout

**Causes and Solutions:**

1. **Server not started**
   ```cpp
   // Ensure server is started
   server.start();
   if (!server.is_running()) {
       std::cerr << "Server failed to start\n";
   }
   ```

2. **Wrong port or address**
   ```cpp
   // Check server configuration
   raft::cpp_httplib_server<transport_types> server(
       "0.0.0.0",  // Bind to all interfaces, not just localhost
       8080,       // Ensure port matches client URLs
       config,
       metrics
   );

   // Check client URLs
   std::unordered_map<std::uint64_t, std::string> node_urls;
   node_urls[1] = "http://192.168.1.10:8080";  // Use correct IP/hostname
   ```

3. **Firewall blocking connections**
   ```bash
   # Test connectivity
   telnet server_host 8080
   curl -v http://server_host:8080/v1/raft/request_vote
   ```

4. **Port already in use**
   ```bash
   # Check if port is in use
   netstat -ln | grep :8080
   lsof -i :8080
   ```

### Connection Timeouts

**Symptoms:**
- RPCs timeout after configured duration
- Intermittent connection failures

**Causes and Solutions:**

1. **Network latency too high**
   ```cpp
   // Increase timeouts for high-latency networks
   client_config.connection_timeout = std::chrono::milliseconds{10000};
   client_config.request_timeout = std::chrono::milliseconds{30000};
   ```

2. **Server overloaded**
   ```cpp
   // Increase server capacity
   server_config.max_concurrent_connections = 200;
   server_config.request_timeout = std::chrono::seconds{60};
   ```

3. **Connection pool exhaustion**
   ```cpp
   // Increase connection pool size
   client_config.connection_pool_size = 20;
   ```

### Connection Drops

**Symptoms:**
- Established connections suddenly close
- Intermittent "Connection reset by peer" errors

**Causes and Solutions:**

1. **Keep-alive timeout too short**
   ```cpp
   // Increase keep-alive timeout
   client_config.keep_alive_timeout = std::chrono::milliseconds{120000};
   ```

2. **Load balancer or proxy interference**
   - Configure load balancer for persistent connections
   - Adjust proxy timeout settings
   - Consider direct node-to-node communication

3. **Network infrastructure issues**
   ```bash
   # Monitor network stability
   ping -c 100 target_host
   mtr target_host
   ```

4. **The server is at `max_concurrent_connections`.** New connections are
   closed before any request is read. See
   [Server Request and Connection Limits](#server-request-and-connection-limits).

## Server Request and Connection Limits

The three HTTP servers share two limit fields. What each one does:

| Field | cpp-httplib | Boost.Beast | Proxygen |
|---|---|---|---|
| `max_request_body_size` (default 10 MiB, inclusive) | `set_payload_max_length`: 413 | parser `body_limit`: 413, `Connection: close` | `Content-Length` checked from the headers, chunked bodies counted as they arrive: 413, `Connection: close` |
| `max_concurrent_connections` (default 100) | enforced: new connections past the limit are closed | enforced: new connections past the limit are reset | enforced: new sessions past the limit are reset |

**Body limit.** A request whose body is larger than the limit is answered
`413 Payload Too Large` with a `text/plain` body, and the handler never runs.
Beast and Proxygen close the connection after the 413, because unread body
bytes would otherwise be read as the next request. Proxygen refuses a declared
`Content-Length` before buffering a single body byte. `0` refuses every
non-empty body; no value means "unlimited". The client sees the RPC fail with
its usual HTTP error for a non-200 status. Size the limit above your largest
`install_snapshot` chunk.

**Connection limit.** The count is per server object, across every listener
it owns: a `"*"` bind's 0.0.0.0 and :: share one limit. Past the limit, the
server closes the new connection before reading from it: Beast and Proxygen
with an RST, cpp-httplib with an ordinary close. On Beast and cpp-httplib that
happens before any TLS handshake. On Proxygen it happens after the
session is built, so a refused TLS connection has still cost one handshake.
The client sees `Connection reset by peer` or EOF, which every client treats
as a transient network error, and Raft retries on its next heartbeat or
election timeout. A slot is released when the connection closes for any
reason, so later connections are admitted again. `0` is rejected with
`std::invalid_argument` when the server is constructed.

One node receives at most *peers × connections per peer* inbound connections.
The Proxygen and Beast clients keep up to `connection_pool_size` (default 10)
per target, so a five-node cluster reaches at most 4 × 10 = 40, under the
default 100. Several Raft groups sharing one transport share these
connections; raise the limit if many groups share one server.

**Handshake deadline (Beast).** A slot is held from accept, so a client that
connects to a TLS server and never finishes the handshake would hold it
forever; enough of them lock every peer out. Beast closes such a connection
after `handshake_timeout` (default 10 s, Beast-only, ignored without
`enable_ssl`; zero or negative is rejected with `std::invalid_argument`).
`request_timeout` applies once the handshake is done. Raise it only if peers
sit behind very slow links.

**Out of file descriptors (Beast).** When the process hits its descriptor
limit, accepting fails with `EMFILE`/`ENFILE` while the pending connection
keeps the listener ready. Beast then waits 10 ms before accepting again,
doubling up to 1 s, and resets after the next successful accept, the same
schedule as the raw TCP RPC servers. Each failure emits
`beast_http.server.accept_error` with an `error_class` of `backoff`,
`retry` or `fatal`; `fatal` (the listener itself is unusable) stops that
listener. A steady stream of `backoff` means `ulimit -n` is too low for the
connection limit.

**Metrics.** Beast emits `beast_http.server.request_too_large` and
`beast_http.server.connection_refused`; Proxygen emits the same names under
`proxygen_http.server.`. Neither HTTP server has a logger, so these counters
are the only record of a refusal. A steady stream of `connection_refused`
means the limit is too small for the cluster, or someone is opening
connections they do not use.

## TLS/Certificate Issues

### Certificate Verification Failures

**Symptoms:**
- "Certificate verification failed" errors
- HTTPS connections fail while HTTP works

**Causes and Solutions:**

1. **Self-signed certificates in production**
   ```cpp
   // For development only - never in production
   client_config.enable_ssl_verification = false;

   // For production - use proper CA-signed certificates
   client_config.enable_ssl_verification = true;
   client_config.ca_cert_path = "/etc/ssl/certs/ca-certificates.crt";
   ```

2. **Wrong CA certificate bundle**
   ```cpp
   // Specify correct CA bundle path
   client_config.ca_cert_path = "/path/to/correct/ca-bundle.crt";
   ```

3. **Hostname mismatch**
   ```cpp
   // Ensure URLs match certificate Common Name or SAN
   node_urls[1] = "https://node1.example.com:8443";  // Not IP address
   ```

4. **Expired certificates**
   ```bash
   # Check certificate validity
   openssl x509 -in server.crt -text -noout | grep -A2 "Validity"

   # Check remote certificate
   openssl s_client -connect server_host:8443 -servername server_host
   ```

### TLS Handshake Failures

**Symptoms:**
- "TLS handshake failed" errors
- Connection established but immediately closed

**Causes and Solutions:**

1. **TLS version mismatch**
   ```cpp
   // The transport enforces TLS 1.2+, ensure server supports it
   // Check server TLS configuration
   ```

2. **Cipher suite incompatibility**
   ```bash
   # Test TLS connection
   openssl s_client -connect server_host:8443 -tls1_2
   nmap --script ssl-enum-ciphers -p 8443 server_host
   ```

3. **Certificate/key mismatch**
   ```bash
   # Verify certificate and key match
   openssl x509 -noout -modulus -in server.crt | openssl md5
   openssl rsa -noout -modulus -in server.key | openssl md5
   # MD5 hashes should match
   ```

### Certificate Loading Errors

**Symptoms:**
- Server fails to start with TLS enabled
- "Failed to load certificate" errors

**Causes and Solutions:**

1. **File permissions**
   ```bash
   # Ensure certificate files are readable
   chmod 644 server.crt
   chmod 600 server.key  # Private key should be restricted
   chown app_user:app_group server.crt server.key
   ```

2. **File format issues**
   ```bash
   # Verify certificate format
   openssl x509 -in server.crt -text -noout

   # Convert if necessary
   openssl x509 -in server.crt -outform PEM -out server.pem
   ```

3. **Missing intermediate certificates**
   ```bash
   # Create certificate chain
   cat server.crt intermediate.crt > server-chain.crt
   ```

## Performance Issues

### High Latency

**Symptoms:**
- RPCs take longer than expected
- Raft leader elections timeout

**Causes and Solutions:**

1. **Connection establishment overhead**
   ```cpp
   // Increase connection pool size
   client_config.connection_pool_size = 50;

   // Enable keep-alive
   client_config.keep_alive_timeout = std::chrono::milliseconds{300000};
   ```

2. **Serialization overhead**
   ```cpp
   // Consider more efficient serializer
   struct fast_transport_types {
       using serializer_type = raft::protobuf_serializer;  // Instead of JSON
       template<typename T> using future_template = folly::Future<T>;
       using executor_type = folly::CPUThreadPoolExecutor;
       using metrics_type = raft::detailed_metrics;
   };
   ```

3. **Network configuration**
   ```bash
   # Optimize TCP settings
   echo 'net.ipv4.tcp_congestion_control = bbr' >> /etc/sysctl.conf
   echo 'net.core.rmem_max = 16777216' >> /etc/sysctl.conf
   echo 'net.core.wmem_max = 16777216' >> /etc/sysctl.conf
   sysctl -p
   ```

### Low Throughput

**Symptoms:**
- Cannot achieve expected RPC rate
- Connection pool frequently exhausted

**Causes and Solutions:**

1. **Insufficient connection pooling**
   ```cpp
   // Scale connection pool with load
   client_config.connection_pool_size = 100;  // Increase significantly
   ```

2. **Server connection limits**
   ```cpp
   // Increase server capacity
   server_config.max_concurrent_connections = 1000;
   server_config.max_request_body_size = 50 * 1024 * 1024;  // 50 MB
   ```

3. **Thread pool saturation**
   ```cpp
   // Use larger thread pool
   struct high_throughput_types {
       using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
       template<typename T> using future_template = folly::Future<T>;
       using executor_type = folly::CPUThreadPoolExecutor;  // Configure with more threads
       using metrics_type = raft::detailed_metrics;
   };
   ```

### Memory Usage Issues

**Symptoms:**
- High memory consumption
- Out of memory errors under load

**Causes and Solutions:**

1. **Connection pool not releasing connections**
   ```cpp
   // Reduce keep-alive timeout
   client_config.keep_alive_timeout = std::chrono::milliseconds{30000};

   // Limit connection pool size
   client_config.connection_pool_size = 10;
   ```

2. **Large request/response bodies**
   ```cpp
   // Limit request body size
   server_config.max_request_body_size = 1024 * 1024;  // 1 MB
   ```

3. **Memory leaks in handlers**
   ```cpp
   // Ensure handlers don't hold references
   server.register_append_entries_handler([](const auto& request) {
       // Process request without holding references
       raft::append_entries_response<> response;
       // ... process ...
       return response;  // Return by value
   });
   ```

## Serialization Problems

### Deserialization Failures

**Symptoms:**
- "Failed to deserialize request/response" errors
- 400 Bad Request responses from server

**Causes and Solutions:**

1. **Version mismatch between client and server**
   ```cpp
   // Ensure both use same serializer version
   using consistent_serializer = raft::json_rpc_serializer<std::vector<std::byte>>;
   ```

2. **Corrupted data transmission**
   ```cpp
   // Enable request/response logging for debugging
   // Check Content-Length headers match actual body size
   ```

3. **Character encoding issues**
   ```cpp
   // Ensure UTF-8 encoding for JSON
   // Verify Content-Type headers are set correctly
   ```

### Serialization Format Errors

**Symptoms:**
- Content-Type header mismatches
- Server returns 415 Unsupported Media Type

**Causes and Solutions:**

1. **Mismatched serializers**
   ```cpp
   // Ensure client and server use same serializer
   struct consistent_types {
       using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
       // ... other types ...
   };
   ```

2. **Custom serializer issues**
   ```cpp
   // Verify custom serializer implements rpc_serializer concept
   static_assert(raft::rpc_serializer<MyCustomSerializer>);
   ```

## Configuration Issues

### transport_types Concept Violations

**Symptoms:**
- Compilation errors about concept requirements
- Template instantiation failures

**Causes and Solutions:**

1. **Missing required type members**
   ```cpp
   // Ensure all required types are defined
   struct complete_transport_types {
       using serializer_type = raft::json_rpc_serializer<std::vector<std::byte>>;
       template<typename T> using future_template = folly::Future<T>;
       using executor_type = folly::CPUThreadPoolExecutor;
       using metrics_type = raft::noop_metrics;  // Don't forget metrics_type
   };
   ```

2. **Concept constraint violations**
   ```cpp
   // Verify types satisfy their concepts
   static_assert(raft::rpc_serializer<typename MyTypes::serializer_type>);
   static_assert(raft::metrics<typename MyTypes::metrics_type>);
   static_assert(raft::future<typename MyTypes::template future_template<int>, int>);
   ```

### URL Configuration Problems

**Symptoms:**
- "Node not found" errors
- RPCs sent to wrong endpoints

**Causes and Solutions:**

1. **Missing node URLs**
   ```cpp
   // Ensure all target nodes have URLs configured
   std::unordered_map<std::uint64_t, std::string> node_urls;
   node_urls[1] = "http://node1:8080";
   node_urls[2] = "http://node2:8080";
   node_urls[3] = "http://node3:8080";  // Don't forget all nodes
   ```

2. **Incorrect URL format**
   ```cpp
   // Use proper URL format
   node_urls[1] = "http://node1:8080";     // ✓ Correct
   node_urls[2] = "node2:8080";            // ✗ Missing protocol
   node_urls[3] = "http://node3:8080/";    // ✓ Trailing slash OK
   ```

## Debugging Tools

### Logging Configuration

```cpp
// Enable detailed logging (implementation-specific)
// This is conceptual - actual logging depends on your logging framework

class debug_metrics : public raft::noop_metrics {
public:
    void record_request_sent(const std::string& rpc_type, std::uint64_t target) override {
        std::cout << "Sending " << rpc_type << " to node " << target << std::endl;
    }

    void record_request_latency(const std::string& rpc_type,
                               std::chrono::milliseconds latency) override {
        std::cout << rpc_type << " latency: " << latency.count() << "ms" << std::endl;
    }

    void record_error(const std::string& error_type, const std::string& details) override {
        std::cerr << "Error [" << error_type << "]: " << details << std::endl;
    }
};
```

### Network Debugging

```bash
# Monitor HTTP traffic
tcpdump -i any -A -s 0 port 8080

# Test HTTP endpoints directly
curl -v -X POST http://node1:8080/v1/raft/request_vote \
  -H "Content-Type: application/json" \
  -d '{"term":1,"candidate_id":1,"last_log_index":0,"last_log_term":0}'

# Test HTTPS endpoints
curl -v -X POST https://node1:8443/v1/raft/request_vote \
  -H "Content-Type: application/json" \
  -d '{"term":1,"candidate_id":1,"last_log_index":0,"last_log_term":0}' \
  --cacert ca.crt

# Check certificate details
openssl s_client -connect node1:8443 -servername node1 -showcerts
```

### Performance Profiling

```cpp
// Add timing measurements
auto start = std::chrono::steady_clock::now();
auto future = client.send_request_vote(target, request, timeout);
auto response = std::move(future).get();
auto end = std::chrono::steady_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
std::cout << "RPC took " << duration.count() << "ms" << std::endl;
```

```bash
# System-level profiling
perf record -g ./your_raft_application
perf report

# Memory profiling
valgrind --tool=massif ./your_raft_application
```

## Common Error Messages

### Client Errors

| Error Message | Cause | Solution |
|---------------|-------|----------|
| "Connection refused" | Server not running or wrong port | Check server status and port configuration |
| "Connection timeout" | Network latency or server overload | Increase timeouts or check network |
| "Certificate verification failed" | Invalid or expired certificate | Check certificate validity and CA bundle |
| "TLS handshake failed" | TLS version or cipher mismatch | Verify TLS configuration compatibility |
| "Node not found in URL map" | Missing node URL configuration | Add URL for target node ID |
| "Request timeout" | RPC took too long | Increase request timeout or check server performance |
| "does not implement RPC 'request_pre_vote'" (or `'timeout_now'`) | Peer answered 404/501 on an extension endpoint: an older build, or no handler registered | Expected during a rolling upgrade; see [HTTP Transport Endpoints](http_transport_endpoints.md) |

### Server Errors

| Error Message | Cause | Solution |
|---------------|-------|----------|
| "Failed to bind to address" | Port in use or permission denied | Check port availability and permissions |
| "Failed to load certificate" | Certificate file issues | Verify certificate file path and format |
| "Handler not registered" | Missing RPC handler | Register handler before starting server |
| "Deserialization failed" | Invalid request format | Check client serializer compatibility |
| `413 Payload Too Large` | Request body over `max_request_body_size` | Increase `max_request_body_size` or send smaller snapshot chunks |
| `Connection reset by peer` on connect (Beast, Proxygen) | Server at `max_concurrent_connections` | Increase `max_concurrent_connections`; check `*.server.connection_refused` |

### Compilation Errors

| Error Message | Cause | Solution |
|---------------|-------|----------|
| "transport_types concept not satisfied" | Missing or invalid type members | Ensure all required types are defined correctly |
| "rpc_serializer concept not satisfied" | Invalid serializer type | Use compliant serializer implementation |
| "future concept not satisfied" | Invalid future template | Use a compliant future type (`folly::Future`, `kythira::stdexec_backend::Future`) — `std::future` does not satisfy `kythira::future` (no `isReady()`, no `wait(milliseconds) -> bool`) and cannot be used here |
| "Template instantiation failed" | Type compatibility issues | Check type compatibility and concept requirements |

## Performance Tuning Tips

### Connection Management

1. **Right-size connection pools**
   - Start with 10 connections per target
   - Scale based on RPC rate and latency
   - Monitor connection pool utilization

2. **Optimize keep-alive settings**
   - Use longer keep-alive for stable networks
   - Shorter keep-alive for dynamic environments
   - Balance memory usage vs connection overhead

### Request Optimization

1. **Batch operations when possible**
   - Combine multiple log entries in AppendEntries
   - Use larger snapshot chunks for InstallSnapshot

2. **Optimize serialization**
   - Consider binary formats for high-throughput scenarios
   - Profile serialization overhead
   - Cache serialized data when appropriate

### Server Tuning

1. **Scale server resources**
   - Increase connection limits for high-load scenarios
   - Tune request timeouts based on processing time
   - Monitor server resource utilization

2. **Optimize handler performance**
   - Minimize handler execution time
   - Avoid blocking operations in handlers
   - Use efficient data structures

### Network Optimization

1. **TCP tuning**
   - Increase TCP buffer sizes for high-bandwidth networks
   - Use appropriate congestion control algorithms
   - Optimize for your network characteristics

2. **Infrastructure considerations**
   - Place nodes close to minimize latency
   - Use dedicated network interfaces for Raft traffic
   - Consider network topology in deployment

## Getting Help

If you encounter issues not covered in this guide:

1. **Check the logs** - Enable detailed logging and examine error messages
2. **Test incrementally** - Start with simple configurations and add complexity
3. **Use debugging tools** - Network captures, profilers, and monitoring tools
4. **Verify configuration** - Double-check all configuration parameters
5. **Test connectivity** - Use basic tools like curl and telnet to verify connectivity

For additional support, refer to:
- [HTTP Transport Design Document](../.kiro/specs/http-transport/design.md)
- [HTTP Transport Requirements](../.kiro/specs/http-transport/requirements.md)
- [Example Implementation](../examples/raft/http_transport_example.cpp)