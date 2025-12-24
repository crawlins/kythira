# CoAP Transport Troubleshooting Guide

## Overview

This guide provides systematic approaches to diagnosing and resolving common issues with the CoAP transport implementation in Raft consensus systems. It covers network connectivity problems, DTLS security issues, performance problems, and configuration errors.

## Diagnostic Tools and Techniques

### Logging Configuration

Enable comprehensive logging for troubleshooting:

```cpp
#include <raft/console_logger.hpp>

// Create logger with debug level
auto logger = std::make_shared<console_logger>(log_level::debug);

// Enable libcoap debug logging (implementation-dependent)
coap_set_log_level(LOG_DEBUG);

// Use logger with CoAP transport
auto client = coap_client<json_serializer>(endpoints, config, metrics, logger);
```

### Network Diagnostic Commands

```bash
# Test basic connectivity
ping target_host

# Test UDP connectivity
nc -u target_host 5683

# Check if CoAP port is open
nmap -sU -p 5683 target_host

# Monitor network traffic
tcpdump -i eth0 -n port 5683

# Check UDP statistics
netstat -su | grep -A 10 "Udp:"

# Monitor real-time connections
ss -u -a -n | grep :5683
```

### CoAP-Specific Tools

```bash
# Test CoAP connectivity (requires coap-client)
coap-client -m get coap://target_host:5683/.well-known/core

# Test CoAPS connectivity
coap-client -m get coaps://target_host:5684/.well-known/core

# Send test CoAP request
echo "test payload" | coap-client -m post -T text/plain coap://target_host:5683/test
```

## Common Issues and Solutions

### Connection Issues

#### Issue: "Connection refused" or "Network unreachable"

**Symptoms:**
- `coap_network_error` exceptions
- Immediate connection failures
- No network traffic visible

**Diagnosis:**
```bash
# Check if target host is reachable
ping target_host

# Check if CoAP port is open
telnet target_host 5683

# Check firewall rules
iptables -L | grep 5683
```

**Solutions:**

1. **Firewall Configuration**
   ```bash
   # Allow CoAP traffic
   iptables -A INPUT -p udp --dport 5683 -j ACCEPT
   iptables -A OUTPUT -p udp --sport 5683 -j ACCEPT
   
   # Allow CoAPS traffic
   iptables -A INPUT -p udp --dport 5684 -j ACCEPT
   iptables -A OUTPUT -p udp --sport 5684 -j ACCEPT
   ```

2. **Server Binding Issues**
   ```cpp
   // Ensure server binds to correct interface
   coap_server_config config;
   auto server = coap_server<json_serializer>(
       "0.0.0.0",  // Bind to all interfaces, not "127.0.0.1"
       5683,
       config,
       metrics
   );
   ```

3. **DNS Resolution**
   ```bash
   # Test DNS resolution
   nslookup target_host
   
   # Use IP addresses if DNS fails
   std::unordered_map<std::uint64_t, std::string> endpoints = {
       {1, "coap://192.168.1.10:5683"},  // Use IP instead of hostname
       {2, "coap://192.168.1.11:5683"}
   };
   ```

#### Issue: "Request timeout" errors

**Symptoms:**
- `coap_timeout_error` exceptions
- Requests taking longer than expected
- Intermittent failures

**Diagnosis:**
```bash
# Check network latency
ping -c 10 target_host

# Monitor packet loss
ping -c 100 target_host | grep "packet loss"

# Check network congestion
iperf3 -c target_host -u -b 10M
```

**Solutions:**

1. **Adjust Timeout Settings**
   ```cpp
   coap_client_config config;
   
   // For high-latency networks
   config.ack_timeout = std::chrono::milliseconds{5000};  // Increase from 2000ms
   config.max_retransmit = 6;  // Increase from 4
   
   // For low-latency networks
   config.ack_timeout = std::chrono::milliseconds{500};   // Decrease to 500ms
   config.max_retransmit = 2;  // Decrease to 2
   ```

2. **Network Buffer Tuning**
   ```bash
   # Increase UDP buffer sizes
   echo 'net.core.rmem_max = 16777216' >> /etc/sysctl.conf
   echo 'net.core.wmem_max = 16777216' >> /etc/sysctl.conf
   sysctl -p
   ```

3. **Check Server Load**
   ```cpp
   coap_server_config config;
   config.max_concurrent_sessions = 500;  // Increase if server is overloaded
   ```

### DTLS Security Issues

#### Issue: "DTLS handshake failed"

**Symptoms:**
- `coap_security_error` exceptions
- Connection attempts that hang or timeout
- "SSL handshake failed" in logs

**Diagnosis:**
```bash
# Test DTLS connectivity
openssl s_client -dtls1_2 -connect target_host:5684

# Check certificate validity
openssl x509 -in cert.pem -text -noout

# Verify certificate chain
openssl verify -CAfile ca-cert.pem cert.pem
```

**Solutions:**

1. **Certificate Issues**
   ```bash
   # Check certificate expiration
   openssl x509 -in cert.pem -dates -noout
   
   # Verify certificate matches hostname
   openssl x509 -in cert.pem -text -noout | grep -A 1 "Subject Alternative Name"
   ```

   ```cpp
   // Fix certificate configuration
   coap_client_config config;
   config.enable_dtls = true;
   config.cert_file = "/path/to/valid-cert.pem";
   config.key_file = "/path/to/matching-key.pem";
   config.ca_file = "/path/to/ca-cert.pem";
   config.verify_peer_cert = true;
   ```

2. **Clock Synchronization**
   ```bash
   # Ensure system clocks are synchronized
   ntpdate -s time.nist.gov
   
   # Enable NTP daemon
   systemctl enable ntp
   systemctl start ntp
   ```

3. **PSK Configuration Issues**
   ```cpp
   // Ensure identical PSK on all nodes
   coap_client_config config;
   config.enable_dtls = true;
   config.psk_identity = "exact-same-identity";  // Must match exactly
   
   // Verify PSK key format
   std::string psk_hex = "deadbeef12345678";
   config.psk_key.clear();
   for (size_t i = 0; i < psk_hex.length(); i += 2) {
       auto byte_val = static_cast<std::byte>(
           std::stoul(psk_hex.substr(i, 2), nullptr, 16));
       config.psk_key.push_back(byte_val);
   }
   ```

#### Issue: "Certificate verification failed"

**Symptoms:**
- DTLS handshake completes but connection is rejected
- "Certificate verification failed" in logs
- `coap_security_error` with certificate details

**Solutions:**

1. **Certificate Chain Issues**
   ```bash
   # Create complete certificate chain
   cat server-cert.pem intermediate-cert.pem > cert-chain.pem
   ```

   ```cpp
   config.cert_file = "/path/to/cert-chain.pem";  // Use complete chain
   ```

2. **Hostname Verification**
   ```cpp
   // Add Subject Alternative Names to certificate
   // Or use IP addresses in certificate
   std::unordered_map<std::uint64_t, std::string> endpoints = {
       {1, "coaps://192.168.1.10:5684"},  // Use IP if hostname doesn't match
   };
   ```

3. **CA Certificate Issues**
   ```bash
   # Verify CA certificate is correct
   openssl x509 -in ca-cert.pem -text -noout
   
   # Check if CA signed the server certificate
   openssl verify -CAfile ca-cert.pem server-cert.pem
   ```

### Message Serialization Issues

#### Issue: "Serialization failed" or "Deserialization failed"

**Symptoms:**
- `serialization_error` exceptions
- Messages sent but not received correctly
- "Invalid message format" errors

**Diagnosis:**
```cpp
// Test serialization independently
json_serializer serializer;
request_vote_request<> request{.term = 1, .candidate_id = 1};

try {
    auto serialized = serializer.serialize(request);
    auto deserialized = serializer.deserialize<request_vote_request<>>(serialized);
    std::cout << "Serialization test passed" << std::endl;
} catch (const std::exception& e) {
    std::cerr << "Serialization test failed: " << e.what() << std::endl;
}
```

**Solutions:**

1. **Serializer Mismatch**
   ```cpp
   // Ensure all nodes use the same serializer
   auto client = coap_client<json_serializer>(endpoints, config, metrics);
   auto server = coap_server<json_serializer>("0.0.0.0", 5683, config, metrics);
   ```

2. **Content-Format Mismatch**
   ```cpp
   // Verify Content-Format option matches serializer
   class debug_serializer {
   public:
       auto content_format() const -> std::uint16_t {
           return 50;  // JSON content format
       }
       
       template<typename T>
       auto serialize(const T& obj) const -> std::vector<std::byte> {
           // Add debug logging
           std::cout << "Serializing object of type: " << typeid(T).name() << std::endl;
           return json_serialize(obj);
       }
   };
   ```

3. **Message Size Issues**
   ```cpp
   coap_server_config config;
   config.max_request_size = 256 * 1024;  // Increase if messages are large
   config.enable_block_transfer = true;   // Enable for large messages
   ```

### Performance Issues

#### Issue: High latency or low throughput

**Symptoms:**
- Requests taking longer than expected
- Low requests per second
- High CPU or memory usage

**Diagnosis:**
```bash
# Monitor system resources
htop
iotop

# Check network utilization
iftop -i eth0

# Profile application
perf record -g ./raft_node
perf report
```

**Solutions:**

1. **Configuration Tuning**
   ```cpp
   coap_client_config config;
   config.max_sessions = 200;  // Increase session pool
   config.max_block_size = 2048;  // Larger blocks for better throughput
   
   coap_server_config server_config;
   server_config.max_concurrent_sessions = 500;  // Handle more concurrent requests
   ```

2. **System Tuning**
   ```bash
   # Increase UDP buffer sizes
   echo 'net.core.rmem_max = 16777216' >> /etc/sysctl.conf
   echo 'net.core.wmem_max = 16777216' >> /etc/sysctl.conf
   
   # Increase file descriptor limits
   echo '* soft nofile 65536' >> /etc/security/limits.conf
   echo '* hard nofile 65536' >> /etc/security/limits.conf
   ```

3. **Application Optimization**
   ```cpp
   // Use efficient serializer
   auto client = coap_client<cbor_serializer>(endpoints, config, metrics);
   
   // Batch operations where possible
   std::vector<folly::Future<request_vote_response<>>> futures;
   for (auto target : targets) {
       futures.push_back(client.send_request_vote(target, request, timeout));
   }
   auto results = folly::collectAll(futures).get();
   ```

#### Issue: Memory leaks or high memory usage

**Symptoms:**
- Continuously increasing memory usage
- Out of memory errors
- System becoming unresponsive

**Diagnosis:**
```bash
# Monitor memory usage
ps aux | grep raft_node
top -p $(pgrep raft_node)

# Use memory profiler
valgrind --tool=memcheck --leak-check=full ./raft_node
```

**Solutions:**

1. **Session Management**
   ```cpp
   coap_client_config config;
   config.session_timeout = std::chrono::seconds{300};  // Shorter timeout
   config.max_sessions = 100;  // Limit total sessions
   ```

2. **Request Cleanup**
   ```cpp
   // Ensure proper cleanup of pending requests
   class managed_coap_client {
   private:
       std::unordered_map<token_t, std::unique_ptr<pending_request>> _pending;
       
   public:
       void periodic_cleanup() {
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

3. **Block Transfer Limits**
   ```cpp
   coap_server_config config;
   config.max_request_size = 64 * 1024;  // Limit per-request memory
   config.max_block_size = 1024;  // Smaller blocks
   ```

### Block Transfer Issues

#### Issue: Large message transfer failures

**Symptoms:**
- Timeouts when sending large messages
- Partial message reception
- "Block transfer failed" errors

**Diagnosis:**
```bash
# Monitor block transfer in logs
grep -i "block" /var/log/raft.log

# Check network MTU
ip link show eth0 | grep mtu
```

**Solutions:**

1. **Block Size Optimization**
   ```cpp
   coap_client_config config;
   config.enable_block_transfer = true;
   config.max_block_size = 512;  // Smaller blocks for unreliable networks
   
   // Or larger blocks for high-bandwidth networks
   config.max_block_size = 4096;
   ```

2. **Timeout Adjustments**
   ```cpp
   // Increase timeouts for large transfers
   auto future = client.send_install_snapshot(
       target,
       large_snapshot_request,
       std::chrono::minutes{5}  // Longer timeout for large snapshots
   );
   ```

3. **Network MTU Issues**
   ```bash
   # Check and adjust MTU
   ip link set dev eth0 mtu 1500
   
   # Test path MTU discovery
   tracepath target_host
   ```

## Systematic Troubleshooting Process

### Step 1: Identify the Problem Layer

1. **Network Layer**: Can basic UDP packets reach the target?
2. **CoAP Layer**: Are CoAP messages being sent and received?
3. **DTLS Layer**: Is the security handshake completing?
4. **Application Layer**: Are Raft messages being processed correctly?

### Step 2: Gather Information

```cpp
// Enable comprehensive logging
class debug_metrics : public metrics_recorder {
public:
    void record_request_sent(const std::string& rpc_type, std::uint64_t target) override {
        std::cout << "Sending " << rpc_type << " to node " << target << std::endl;
    }
    
    void record_request_completed(const std::string& rpc_type, 
                                 std::chrono::milliseconds duration,
                                 bool success) override {
        std::cout << rpc_type << " completed in " << duration.count() 
                  << "ms, success: " << success << std::endl;
    }
    
    void record_error(const std::string& error_type, const std::string& details) {
        std::cerr << "Error [" << error_type << "]: " << details << std::endl;
    }
};
```

### Step 3: Isolate the Issue

```cpp
// Test individual components
class component_tester {
public:
    // Test basic connectivity
    bool test_connectivity(const std::string& endpoint) {
        try {
            // Simple ping-like test
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Connectivity test failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Test DTLS handshake
    bool test_dtls_handshake(const std::string& endpoint) {
        try {
            // Attempt DTLS connection
            return true;
        } catch (const coap_security_error& e) {
            std::cerr << "DTLS test failed: " << e.what() << std::endl;
            return false;
        }
    }
    
    // Test serialization
    bool test_serialization() {
        try {
            json_serializer serializer;
            request_vote_request<> request{.term = 1, .candidate_id = 1};
            auto data = serializer.serialize(request);
            auto result = serializer.deserialize<request_vote_request<>>(data);
            return request.term == result.term && request.candidate_id == result.candidate_id;
        } catch (const std::exception& e) {
            std::cerr << "Serialization test failed: " << e.what() << std::endl;
            return false;
        }
    }
};
```

### Step 4: Apply Targeted Solutions

Based on the isolated issue, apply specific solutions from the sections above.

### Step 5: Verify the Fix

```cpp
// Comprehensive verification test
class verification_tester {
public:
    bool run_full_test(coap_client<json_serializer>& client, std::uint64_t target) {
        try {
            // Test all RPC types
            request_vote_request<> vote_req{.term = 1, .candidate_id = 1};
            auto vote_resp = client.send_request_vote(target, vote_req, 
                                                     std::chrono::seconds{5}).get();
            
            append_entries_request<> append_req{.term = 1, .leader_id = 1};
            auto append_resp = client.send_append_entries(target, append_req, 
                                                         std::chrono::seconds{5}).get();
            
            std::cout << "All RPC tests passed" << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Verification test failed: " << e.what() << std::endl;
            return false;
        }
    }
};
```

## Monitoring and Alerting

### Key Metrics to Monitor

```cpp
class production_metrics : public metrics_recorder {
public:
    void record_request_sent(const std::string& rpc_type, std::uint64_t target) override {
        _requests_sent.increment({{"rpc_type", rpc_type}, {"target", std::to_string(target)}});
    }
    
    void record_request_completed(const std::string& rpc_type, 
                                 std::chrono::milliseconds duration,
                                 bool success) override {
        _request_duration.record(duration.count(), {{"rpc_type", rpc_type}});
        if (!success) {
            _request_failures.increment({{"rpc_type", rpc_type}});
        }
    }
    
    void record_dtls_handshake_failure(const std::string& reason) {
        _dtls_failures.increment({{"reason", reason}});
    }
    
    void record_timeout(const std::string& rpc_type, std::uint64_t target) {
        _timeouts.increment({{"rpc_type", rpc_type}, {"target", std::to_string(target)}});
    }
    
private:
    prometheus::Counter _requests_sent;
    prometheus::Histogram _request_duration;
    prometheus::Counter _request_failures;
    prometheus::Counter _dtls_failures;
    prometheus::Counter _timeouts;
};
```

### Alert Conditions

1. **High Error Rate**: > 5% of requests failing
2. **High Latency**: P95 latency > 1000ms
3. **DTLS Failures**: > 1% of handshakes failing
4. **Timeout Rate**: > 2% of requests timing out
5. **Memory Usage**: > 80% of available memory
6. **Connection Failures**: > 10 connection failures per minute

### Health Check Endpoint

```cpp
class health_checker {
public:
    struct health_status {
        bool overall_healthy;
        std::vector<std::string> issues;
        std::unordered_map<std::string, double> metrics;
    };
    
    auto check_health(coap_client<json_serializer>& client,
                     const std::vector<std::uint64_t>& targets) -> health_status {
        health_status status;
        status.overall_healthy = true;
        
        for (auto target : targets) {
            try {
                auto start = std::chrono::steady_clock::now();
                request_vote_request<> request{.term = 1, .candidate_id = 1};
                auto response = client.send_request_vote(target, request, 
                                                        std::chrono::seconds{2}).get();
                auto duration = std::chrono::steady_clock::now() - start;
                
                status.metrics["latency_to_" + std::to_string(target)] = 
                    std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
                    
            } catch (const std::exception& e) {
                status.overall_healthy = false;
                status.issues.push_back("Failed to reach node " + std::to_string(target) + 
                                       ": " + e.what());
            }
        }
        
        return status;
    }
};
```

## Emergency Procedures

### Service Recovery

1. **Immediate Actions**
   ```bash
   # Restart the service
   systemctl restart raft-node
   
   # Check service status
   systemctl status raft-node
   
   # Monitor logs
   journalctl -u raft-node -f
   ```

2. **Fallback Configuration**
   ```cpp
   // Minimal working configuration
   coap_client_config emergency_config;
   emergency_config.ack_timeout = std::chrono::milliseconds{5000};
   emergency_config.max_retransmit = 6;
   emergency_config.enable_dtls = false;  // Disable DTLS if causing issues
   emergency_config.max_sessions = 50;    // Reduce resource usage
   ```

3. **Network Isolation**
   ```bash
   # Isolate problematic node
   iptables -A INPUT -s problematic_node_ip -j DROP
   iptables -A OUTPUT -d problematic_node_ip -j DROP
   ```

### Data Collection for Support

```bash
# Collect system information
uname -a > debug_info.txt
cat /proc/meminfo >> debug_info.txt
cat /proc/cpuinfo >> debug_info.txt

# Collect network information
ip addr show >> debug_info.txt
netstat -tuln >> debug_info.txt
ss -tuln >> debug_info.txt

# Collect application logs
journalctl -u raft-node --since "1 hour ago" > raft_logs.txt

# Collect network traces
tcpdump -i eth0 -w network_trace.pcap port 5683 or port 5684
```

This comprehensive troubleshooting guide provides systematic approaches to diagnosing and resolving issues with CoAP transport in production Raft deployments.