# CoAP Transport Performance Tuning Guide

## Overview

This guide provides recommendations for optimizing the performance of the CoAP transport implementation in Raft consensus systems. It covers configuration tuning, system-level optimizations, and monitoring strategies to achieve optimal throughput, latency, and resource utilization.

## Performance Characteristics

### CoAP vs HTTP Transport

| Metric | CoAP/UDP | HTTP/TCP |
|--------|----------|----------|
| Connection Overhead | None (connectionless) | High (3-way handshake) |
| Message Overhead | ~4-8 bytes | ~200-500 bytes |
| Latency | Lower (no connection setup) | Higher (connection setup) |
| Throughput | Moderate (UDP limitations) | High (TCP flow control) |
| Reliability | Application-level (CoAP) | Transport-level (TCP) |
| Memory Usage | Lower | Higher |

### Typical Performance Metrics

**Latency (RTT):**
- Local network: 1-5ms
- WAN: 50-200ms
- With DTLS: +10-50ms (handshake overhead)

**Throughput:**
- Small messages (<1KB): 1000-10000 msg/sec
- Large messages (>10KB): 100-1000 msg/sec
- Block transfer: 10-100 MB/sec

**Resource Usage:**
- Memory per session: 1-10KB
- CPU overhead: 5-15% for crypto operations
- Network bandwidth: 90-95% efficiency

## Configuration Tuning

### Client Configuration Optimization

#### Timeout and Retransmission Settings

```cpp
coap_client_config config;

// Aggressive settings for low-latency networks
config.ack_timeout = std::chrono::milliseconds{500};     // Default: 2000ms
config.ack_random_factor_ms = std::chrono::milliseconds{250}; // Default: 1000ms
config.max_retransmit = 2;                               // Default: 4

// Conservative settings for high-latency/lossy networks
config.ack_timeout = std::chrono::milliseconds{5000};
config.ack_random_factor_ms = std::chrono::milliseconds{2000};
config.max_retransmit = 6;
```

**Tuning Guidelines:**
- **Low latency networks**: Reduce `ack_timeout` to 500-1000ms
- **High latency networks**: Increase `ack_timeout` to 5000-10000ms
- **Lossy networks**: Increase `max_retransmit` to 6-8
- **Reliable networks**: Reduce `max_retransmit` to 2-3

#### Session Management

```cpp
coap_client_config config;

// High-throughput settings
config.max_sessions = 200;                               // Default: 100
config.session_timeout = std::chrono::seconds{600};     // Default: 300s

// Memory-constrained settings
config.max_sessions = 50;
config.session_timeout = std::chrono::seconds{120};
```

**Tuning Guidelines:**
- **High throughput**: Increase `max_sessions` to 200-500
- **Memory constrained**: Reduce `max_sessions` to 20-50
- **Frequent reconnections**: Increase `session_timeout`
- **Memory pressure**: Reduce `session_timeout`

#### Block Transfer Optimization

```cpp
coap_client_config config;

// High-throughput settings
config.max_block_size = 4096;                           // Default: 1024
config.enable_block_transfer = true;

// Low-memory settings
config.max_block_size = 512;
config.enable_block_transfer = true;

// Disable for small messages only
config.enable_block_transfer = false;  // Only if all messages < 1KB
```

**Block Size Guidelines:**
- **High bandwidth**: Use 2048-4096 byte blocks
- **Low bandwidth**: Use 512-1024 byte blocks
- **Memory constrained**: Use 256-512 byte blocks
- **Small messages only**: Disable block transfer

### Server Configuration Optimization

#### Concurrency Settings

```cpp
coap_server_config config;

// High-concurrency settings
config.max_concurrent_sessions = 500;                   // Default: 200
config.max_request_size = 128 * 1024;                  // Default: 64KB
config.session_timeout = std::chrono::seconds{300};    // Default: 300s

// Resource-constrained settings
config.max_concurrent_sessions = 100;
config.max_request_size = 32 * 1024;
config.session_timeout = std::chrono::seconds{120};
```

**Tuning Guidelines:**
- **High load**: Increase `max_concurrent_sessions` to 500-1000
- **Large snapshots**: Increase `max_request_size` to 1MB+
- **Memory pressure**: Reduce both limits
- **Short-lived connections**: Reduce `session_timeout`

#### DTLS Performance Settings

```cpp
coap_server_config config;
config.enable_dtls = true;

// Performance-optimized DTLS
config.session_timeout = std::chrono::seconds{1800};   // 30 minutes
// Enable session resumption for better performance
```

**DTLS Optimization:**
- **Session resumption**: Use longer session timeouts (30-60 minutes)
- **Hardware acceleration**: Enable OpenSSL hardware crypto
- **Cipher selection**: Use AES-GCM for best performance
- **Certificate caching**: Cache certificate validation results

## System-Level Optimizations

### Operating System Tuning

#### UDP Buffer Sizes

```bash
# Increase UDP receive buffer size
echo 'net.core.rmem_max = 16777216' >> /etc/sysctl.conf
echo 'net.core.rmem_default = 262144' >> /etc/sysctl.conf

# Increase UDP send buffer size
echo 'net.core.wmem_max = 16777216' >> /etc/sysctl.conf
echo 'net.core.wmem_default = 262144' >> /etc/sysctl.conf

# Apply changes
sysctl -p
```

#### Network Interface Tuning

```bash
# Increase network interface queue length
echo 'net.core.netdev_max_backlog = 5000' >> /etc/sysctl.conf

# Increase maximum number of open files
echo 'fs.file-max = 65536' >> /etc/sysctl.conf

# Apply changes
sysctl -p
```

#### Process Limits

```bash
# Increase file descriptor limits
echo '* soft nofile 65536' >> /etc/security/limits.conf
echo '* hard nofile 65536' >> /etc/security/limits.conf

# Increase process limits
echo '* soft nproc 32768' >> /etc/security/limits.conf
echo '* hard nproc 32768' >> /etc/security/limits.conf
```

### CPU Optimization

#### Thread Pool Sizing

```cpp
// Configure folly executor for optimal performance
auto executor = std::make_shared<folly::CPUThreadPoolExecutor>(
    std::thread::hardware_concurrency() * 2,  // Thread count
    std::make_shared<folly::NamedThreadFactory>("CoAPWorker")
);

// Use with CoAP transport
// Note: Actual integration depends on implementation details
```

#### CPU Affinity

```bash
# Pin CoAP server process to specific CPU cores
taskset -c 0-3 ./raft_node_coap

# Or use systemd service configuration
[Service]
CPUAffinity=0 1 2 3
```

### Memory Optimization

#### Memory Pool Configuration

```cpp
// Example memory pool configuration (implementation-dependent)
coap_client_config config;
config.max_sessions = 100;  // Limit memory usage
config.max_block_size = 1024;  // Smaller blocks = less memory per request

coap_server_config server_config;
server_config.max_concurrent_sessions = 200;  // Balance concurrency vs memory
server_config.max_request_size = 64 * 1024;   // Limit per-request memory
```

#### Garbage Collection Tuning

```cpp
// Use RAII and smart pointers for automatic cleanup
class optimized_coap_client {
private:
    std::unordered_map<token_t, std::unique_ptr<pending_request>> _pending;
    
public:
    void cleanup_expired_requests() {
        auto now = std::chrono::steady_clock::now();
        auto it = _pending.begin();
        while (it != _pending.end()) {
            if (it->second->is_expired(now)) {
                it = _pending.erase(it);
            } else {
                ++it;
            }
        }
    }
};
```

## Application-Level Optimizations

### Message Batching

```cpp
// Batch multiple small operations into single requests
class batched_raft_client {
public:
    auto batch_append_entries(
        std::uint64_t target,
        const std::vector<log_entry>& entries,
        std::chrono::milliseconds timeout
    ) -> folly::Future<append_entries_response<>> {
        
        // Combine multiple entries into single request
        append_entries_request<> request;
        request.entries = entries;  // Send all entries at once
        
        return _client.send_append_entries(target, request, timeout);
    }
    
private:
    coap_client<cbor_serializer> _client;
};
```

### Connection Pooling

```cpp
// Reuse connections for multiple requests
class pooled_coap_client {
public:
    auto send_multiple_requests(
        std::uint64_t target,
        const std::vector<request_vote_request<>>& requests
    ) -> folly::Future<std::vector<request_vote_response<>>> {
        
        std::vector<folly::Future<request_vote_response<>>> futures;
        
        // All requests reuse the same session
        for (const auto& request : requests) {
            futures.push_back(_client.send_request_vote(target, request, timeout));
        }
        
        return folly::collectAll(futures);
    }
    
private:
    coap_client<cbor_serializer> _client;
};
```

### Serialization Optimization

```cpp
// Use efficient binary serialization
#include <raft/cbor_serializer.hpp>

// CBOR is typically 20-50% smaller than JSON
auto client = coap_client<cbor_serializer>(endpoints, config, metrics);

// Custom serializer for maximum efficiency
class optimized_serializer {
public:
    template<typename T>
    auto serialize(const T& obj) const -> std::vector<std::byte> {
        // Use zero-copy serialization where possible
        // Implement custom binary format for maximum efficiency
        return serialize_binary(obj);
    }
    
    auto content_format() const -> std::uint16_t {
        return 42;  // Custom content format
    }
};
```

## Monitoring and Profiling

### Performance Metrics

```cpp
class performance_metrics : public metrics_recorder {
public:
    void record_request_sent(const std::string& rpc_type, std::uint64_t target) override {
        _requests_sent.increment({{"rpc_type", rpc_type}, {"target", std::to_string(target)}});
        _request_start_times[generate_request_id()] = std::chrono::steady_clock::now();
    }
    
    void record_request_completed(const std::string& rpc_type, 
                                 std::chrono::milliseconds duration,
                                 bool success) override {
        _request_duration.record(duration.count(), {{"rpc_type", rpc_type}, {"success", success ? "true" : "false"}});
        _requests_completed.increment({{"rpc_type", rpc_type}, {"success", success ? "true" : "false"}});
    }
    
    void record_dtls_handshake(std::chrono::milliseconds duration, bool success) {
        _dtls_handshake_duration.record(duration.count(), {{"success", success ? "true" : "false"}});
    }
    
    void record_block_transfer(std::size_t total_size, std::size_t block_count) {
        _block_transfer_size.record(total_size);
        _block_transfer_count.record(block_count);
    }
    
private:
    prometheus::Counter _requests_sent;
    prometheus::Counter _requests_completed;
    prometheus::Histogram _request_duration;
    prometheus::Histogram _dtls_handshake_duration;
    prometheus::Histogram _block_transfer_size;
    prometheus::Histogram _block_transfer_count;
};
```

### Key Performance Indicators (KPIs)

1. **Latency Metrics**
   - Request-response round-trip time
   - DTLS handshake duration
   - Block transfer completion time

2. **Throughput Metrics**
   - Requests per second
   - Bytes per second
   - Messages per second

3. **Error Metrics**
   - Timeout rate
   - Retransmission rate
   - DTLS handshake failure rate

4. **Resource Metrics**
   - Active sessions count
   - Memory usage per session
   - CPU utilization

### Profiling Tools

#### Application Profiling

```bash
# Profile with perf
perf record -g ./raft_node_coap
perf report

# Profile with Valgrind
valgrind --tool=callgrind ./raft_node_coap
kcachegrind callgrind.out.*
```

#### Network Profiling

```bash
# Monitor network traffic
iftop -i eth0 -P

# Analyze packet loss
ss -u -a -n | grep :5684

# Monitor UDP statistics
netstat -su | grep -A 10 "Udp:"
```

#### System Profiling

```bash
# Monitor system resources
htop
iotop
iostat 1

# Monitor network buffers
cat /proc/net/udp
cat /proc/sys/net/core/rmem_*
```

## Performance Testing

### Load Testing Framework

```cpp
#include <chrono>
#include <vector>
#include <thread>
#include <atomic>

class coap_load_tester {
public:
    struct test_results {
        std::size_t total_requests;
        std::size_t successful_requests;
        std::size_t failed_requests;
        std::chrono::milliseconds total_duration;
        std::chrono::milliseconds avg_latency;
        std::chrono::milliseconds p95_latency;
        std::chrono::milliseconds p99_latency;
    };
    
    auto run_load_test(
        coap_client<cbor_serializer>& client,
        std::uint64_t target_node,
        std::size_t num_requests,
        std::size_t concurrent_requests,
        std::chrono::milliseconds test_duration
    ) -> test_results {
        
        std::atomic<std::size_t> completed_requests{0};
        std::atomic<std::size_t> failed_requests{0};
        std::vector<std::chrono::milliseconds> latencies;
        std::mutex latencies_mutex;
        
        auto start_time = std::chrono::steady_clock::now();
        auto end_time = start_time + test_duration;
        
        std::vector<std::thread> workers;
        for (std::size_t i = 0; i < concurrent_requests; ++i) {
            workers.emplace_back([&]() {
                while (std::chrono::steady_clock::now() < end_time) {
                    auto request_start = std::chrono::steady_clock::now();
                    
                    request_vote_request<> request{
                        .term = 1,
                        .candidate_id = 1,
                        .last_log_index = 0,
                        .last_log_term = 0
                    };
                    
                    try {
                        auto response = client.send_request_vote(
                            target_node, 
                            request, 
                            std::chrono::milliseconds{5000}
                        ).get();
                        
                        auto request_end = std::chrono::steady_clock::now();
                        auto latency = std::chrono::duration_cast<std::chrono::milliseconds>(
                            request_end - request_start);
                        
                        {
                            std::lock_guard<std::mutex> lock(latencies_mutex);
                            latencies.push_back(latency);
                        }
                        
                        completed_requests++;
                    } catch (const std::exception& e) {
                        failed_requests++;
                    }
                }
            });
        }
        
        for (auto& worker : workers) {
            worker.join();
        }
        
        auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        
        // Calculate percentiles
        std::sort(latencies.begin(), latencies.end());
        auto p95_index = static_cast<std::size_t>(latencies.size() * 0.95);
        auto p99_index = static_cast<std::size_t>(latencies.size() * 0.99);
        
        return test_results{
            .total_requests = completed_requests + failed_requests,
            .successful_requests = completed_requests,
            .failed_requests = failed_requests,
            .total_duration = total_duration,
            .avg_latency = std::accumulate(latencies.begin(), latencies.end(), 
                                         std::chrono::milliseconds{0}) / latencies.size(),
            .p95_latency = latencies.empty() ? std::chrono::milliseconds{0} : latencies[p95_index],
            .p99_latency = latencies.empty() ? std::chrono::milliseconds{0} : latencies[p99_index]
        };
    }
};
```

### Benchmark Scenarios

#### Throughput Test

```cpp
// Test maximum requests per second
auto results = tester.run_load_test(
    client,
    target_node,
    10000,  // num_requests
    50,     // concurrent_requests
    std::chrono::seconds{60}  // test_duration
);

std::cout << "Throughput: " << results.successful_requests / 60.0 << " req/sec" << std::endl;
```

#### Latency Test

```cpp
// Test latency under normal load
auto results = tester.run_load_test(
    client,
    target_node,
    1000,   // num_requests
    5,      // concurrent_requests (low concurrency)
    std::chrono::seconds{30}
);

std::cout << "Average latency: " << results.avg_latency.count() << "ms" << std::endl;
std::cout << "P95 latency: " << results.p95_latency.count() << "ms" << std::endl;
```

#### Stress Test

```cpp
// Test behavior under extreme load
auto results = tester.run_load_test(
    client,
    target_node,
    50000,  // num_requests
    200,    // concurrent_requests (high concurrency)
    std::chrono::minutes{5}
);

std::cout << "Success rate: " << 
    (100.0 * results.successful_requests / results.total_requests) << "%" << std::endl;
```

## Deployment Recommendations

### Production Configuration Template

```cpp
// High-performance production configuration
coap_client_config prod_client_config;
prod_client_config.ack_timeout = std::chrono::milliseconds{1000};
prod_client_config.ack_random_factor_ms = std::chrono::milliseconds{500};
prod_client_config.max_retransmit = 3;
prod_client_config.max_sessions = 200;
prod_client_config.session_timeout = std::chrono::seconds{1800};
prod_client_config.max_block_size = 2048;
prod_client_config.enable_block_transfer = true;
prod_client_config.enable_dtls = true;

coap_server_config prod_server_config;
prod_server_config.max_concurrent_sessions = 500;
prod_server_config.max_request_size = 256 * 1024;  // 256KB
prod_server_config.session_timeout = std::chrono::seconds{1800};
prod_server_config.max_block_size = 2048;
prod_server_config.enable_block_transfer = true;
prod_server_config.enable_dtls = true;
```

### Capacity Planning

#### Memory Requirements

```
Per Client Session: ~2-5KB
Per Server Session: ~3-8KB
Per Pending Request: ~1-2KB
Per Block Transfer: max_block_size * 2

Example for 100 clients, 200 server sessions:
Client memory: 100 * 5KB = 500KB
Server memory: 200 * 8KB = 1.6MB
Total base memory: ~2MB
```

#### CPU Requirements

```
Baseline CPU: 1-2% per 100 req/sec
DTLS overhead: +50-100% CPU usage
Block transfer: +10-20% CPU usage
Serialization: +5-15% CPU usage (depends on format)

Example for 1000 req/sec with DTLS:
Base: 10-20% CPU
DTLS: +10-20% CPU
Total: 20-40% CPU per core
```

#### Network Bandwidth

```
CoAP overhead: ~4-8 bytes per message
DTLS overhead: ~20-40 bytes per message
Block transfer efficiency: ~95-98%

Example for 1000 req/sec, 1KB average message:
Base traffic: 1000 * 1KB = 1MB/sec
CoAP overhead: 1000 * 8 bytes = 8KB/sec
DTLS overhead: 1000 * 40 bytes = 40KB/sec
Total: ~1.05MB/sec
```

### Monitoring Dashboard

Key metrics to monitor in production:

1. **Request Metrics**
   - Requests per second
   - Success rate (%)
   - Average latency (ms)
   - P95/P99 latency (ms)

2. **Connection Metrics**
   - Active sessions
   - DTLS handshake success rate
   - Session reuse rate

3. **System Metrics**
   - CPU utilization (%)
   - Memory usage (MB)
   - Network bandwidth (MB/s)
   - UDP packet loss rate

4. **Error Metrics**
   - Timeout rate
   - Retransmission rate
   - Authentication failures

This comprehensive performance tuning guide provides the foundation for optimizing CoAP transport performance in production Raft deployments.