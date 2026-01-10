# Raft Configuration Guide

This guide covers configuration options for timeouts, retry policies, and other tunable parameters in the Raft completion implementation.

## Overview

The Raft completion implementation provides extensive configuration options to tune behavior for different network environments, performance requirements, and reliability needs.

## Configuration Structure

### raft_configuration

The main configuration structure contains all tunable parameters:

```cpp
namespace raft {
    struct raft_configuration {
        // RPC timeout settings
        std::chrono::milliseconds rpc_timeout{5000};
        std::chrono::milliseconds heartbeat_timeout{1000};
        std::chrono::milliseconds election_timeout_min{2000};
        std::chrono::milliseconds election_timeout_max{4000};
        
        // Commit waiting settings
        std::chrono::milliseconds default_commit_timeout{30000};
        std::chrono::milliseconds commit_check_interval{100};
        
        // Future collection settings
        std::chrono::milliseconds majority_collection_timeout{5000};
        std::chrono::milliseconds heartbeat_collection_timeout{2000};
        
        // Configuration change settings
        std::chrono::milliseconds config_change_timeout{60000};
        std::chrono::milliseconds config_phase_timeout{30000};
        
        // Retry policy settings
        retry_policy default_retry_policy{};
        retry_policy heartbeat_retry_policy{};
        retry_policy append_entries_retry_policy{};
        retry_policy vote_request_retry_policy{};
        retry_policy snapshot_retry_policy{};
        
        // Performance settings
        std::size_t max_batch_size{100};
        std::size_t max_pending_operations{1000};
        bool enable_pipelining{true};
        
        // Logging and metrics
        log_level min_log_level{log_level::info};
        bool enable_detailed_metrics{false};
    };
}
```

## Timeout Configuration

### RPC Timeouts

#### rpc_timeout
**Default**: 5000ms  
**Purpose**: General RPC operation timeout  
**Tuning**: 
- Increase for high-latency networks
- Decrease for low-latency, reliable networks

```cpp
config.rpc_timeout = std::chrono::seconds(10);  // High-latency network
config.rpc_timeout = std::chrono::seconds(2);   // Low-latency network
```

#### heartbeat_timeout
**Default**: 1000ms  
**Purpose**: Timeout for individual heartbeat operations  
**Tuning**:
- Should be much smaller than election timeout
- Increase if network has high jitter

```cpp
// Rule of thumb: heartbeat_timeout << election_timeout_min
config.heartbeat_timeout = std::chrono::milliseconds(500);
config.election_timeout_min = std::chrono::milliseconds(2000);
```

#### election_timeout_min/max
**Default**: 2000ms - 4000ms  
**Purpose**: Range for randomized election timeouts  
**Tuning**:
- Increase for unstable networks
- Decrease for faster leader election

```cpp
// Stable network - faster elections
config.election_timeout_min = std::chrono::milliseconds(1000);
config.election_timeout_max = std::chrono::milliseconds(2000);

// Unstable network - avoid split votes
config.election_timeout_min = std::chrono::milliseconds(5000);
config.election_timeout_max = std::chrono::milliseconds(10000);
```

### Client Operation Timeouts

#### default_commit_timeout
**Default**: 30000ms  
**Purpose**: Default timeout for client operations waiting for commit  
**Tuning**:
- Increase for slow state machines
- Decrease for fast, reliable clusters

```cpp
// Slow state machine (database writes)
config.default_commit_timeout = std::chrono::minutes(2);

// Fast state machine (in-memory operations)
config.default_commit_timeout = std::chrono::seconds(10);
```

#### commit_check_interval
**Default**: 100ms  
**Purpose**: How often to check for timed-out commit operations  
**Tuning**:
- Decrease for faster timeout detection
- Increase to reduce CPU overhead

```cpp
// Fast timeout detection
config.commit_check_interval = std::chrono::milliseconds(50);

// Lower overhead
config.commit_check_interval = std::chrono::milliseconds(500);
```

### Future Collection Timeouts

#### majority_collection_timeout
**Default**: 5000ms  
**Purpose**: Timeout for collecting majority responses  
**Tuning**:
- Increase for large clusters
- Decrease for small, fast clusters

```cpp
// Large cluster (many nodes)
config.majority_collection_timeout = std::chrono::seconds(10);

// Small cluster (3-5 nodes)
config.majority_collection_timeout = std::chrono::seconds(2);
```

#### heartbeat_collection_timeout
**Default**: 2000ms  
**Purpose**: Timeout for collecting heartbeat responses during reads  
**Tuning**:
- Should be smaller than majority_collection_timeout
- Increase if read operations frequently timeout

```cpp
// Fast reads
config.heartbeat_collection_timeout = std::chrono::milliseconds(1000);

// Reliable reads
config.heartbeat_collection_timeout = std::chrono::seconds(5);
```

### Configuration Change Timeouts

#### config_change_timeout
**Default**: 60000ms  
**Purpose**: Total timeout for entire configuration change  
**Tuning**:
- Increase for large clusters or slow networks
- Must account for two-phase protocol

```cpp
// Large cluster or slow network
config.config_change_timeout = std::chrono::minutes(5);

// Small, fast cluster
config.config_change_timeout = std::chrono::seconds(30);
```

#### config_phase_timeout
**Default**: 30000ms  
**Purpose**: Timeout for each phase of configuration change  
**Tuning**:
- Should be less than config_change_timeout / 2
- Increase if phases frequently timeout

```cpp
// Ensure phases don't exceed total timeout
config.config_phase_timeout = config.config_change_timeout / 3;
```

## Retry Policy Configuration

### retry_policy Structure

```cpp
struct retry_policy {
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{5000};
    double backoff_multiplier{2.0};
    std::size_t max_attempts{5};
    double jitter_factor{0.1};  // 10% jitter
    
    // Which errors should trigger retry
    bool retry_on_timeout{true};
    bool retry_on_network_error{true};
    bool retry_on_leadership_loss{false};  // Usually handled at higher level
};
```

### Default Retry Policy

```cpp
// Conservative default policy
config.default_retry_policy = retry_policy{
    .initial_delay = std::chrono::milliseconds(100),
    .max_delay = std::chrono::seconds(5),
    .backoff_multiplier = 2.0,
    .max_attempts = 3,
    .jitter_factor = 0.1
};
```

### Operation-Specific Retry Policies

#### Heartbeat Retry Policy

```cpp
// Heartbeats should retry quickly but not too aggressively
config.heartbeat_retry_policy = retry_policy{
    .initial_delay = std::chrono::milliseconds(50),
    .max_delay = std::chrono::milliseconds(500),
    .backoff_multiplier = 1.5,
    .max_attempts = 3,
    .jitter_factor = 0.2
};
```

#### AppendEntries Retry Policy

```cpp
// AppendEntries can be more aggressive since they're critical
config.append_entries_retry_policy = retry_policy{
    .initial_delay = std::chrono::milliseconds(100),
    .max_delay = std::chrono::seconds(2),
    .backoff_multiplier = 2.0,
    .max_attempts = 5,
    .jitter_factor = 0.1
};
```

#### Vote Request Retry Policy

```cpp
// Vote requests during elections should be fast
config.vote_request_retry_policy = retry_policy{
    .initial_delay = std::chrono::milliseconds(25),
    .max_delay = std::chrono::milliseconds(200),
    .backoff_multiplier = 1.5,
    .max_attempts = 2,  // Elections are time-sensitive
    .jitter_factor = 0.3
};
```

#### Snapshot Retry Policy

```cpp
// Snapshots are large and can take time
config.snapshot_retry_policy = retry_policy{
    .initial_delay = std::chrono::seconds(1),
    .max_delay = std::chrono::seconds(30),
    .backoff_multiplier = 2.0,
    .max_attempts = 3,
    .jitter_factor = 0.1
};
```

## Performance Configuration

### Batching and Pipelining

#### max_batch_size
**Default**: 100  
**Purpose**: Maximum number of entries to batch in a single AppendEntries RPC  
**Tuning**:
- Increase for higher throughput
- Decrease for lower latency

```cpp
// High throughput
config.max_batch_size = 500;

// Low latency
config.max_batch_size = 10;
```

#### max_pending_operations
**Default**: 1000  
**Purpose**: Maximum number of pending client operations  
**Tuning**:
- Increase for high client load
- Decrease to limit memory usage

```cpp
// High load server
config.max_pending_operations = 10000;

// Memory-constrained environment
config.max_pending_operations = 100;
```

#### enable_pipelining
**Default**: true  
**Purpose**: Enable pipelining of RPC operations  
**Tuning**:
- Disable for debugging or very unreliable networks
- Keep enabled for production

```cpp
// Production (recommended)
config.enable_pipelining = true;

// Debugging
config.enable_pipelining = false;
```

## Environment-Specific Configurations

### Local Development

```cpp
raft_configuration local_config{
    .rpc_timeout = std::chrono::seconds(1),
    .heartbeat_timeout = std::chrono::milliseconds(100),
    .election_timeout_min = std::chrono::milliseconds(500),
    .election_timeout_max = std::chrono::milliseconds(1000),
    .default_commit_timeout = std::chrono::seconds(5),
    .majority_collection_timeout = std::chrono::seconds(1),
    .config_change_timeout = std::chrono::seconds(10),
    .min_log_level = log_level::debug,
    .enable_detailed_metrics = true
};
```

### Production LAN

```cpp
raft_configuration lan_config{
    .rpc_timeout = std::chrono::seconds(5),
    .heartbeat_timeout = std::chrono::milliseconds(500),
    .election_timeout_min = std::chrono::milliseconds(2000),
    .election_timeout_max = std::chrono::milliseconds(4000),
    .default_commit_timeout = std::chrono::seconds(30),
    .majority_collection_timeout = std::chrono::seconds(5),
    .config_change_timeout = std::chrono::seconds(60),
    .max_batch_size = 200,
    .max_pending_operations = 5000,
    .min_log_level = log_level::info
};
```

### Production WAN

```cpp
raft_configuration wan_config{
    .rpc_timeout = std::chrono::seconds(15),
    .heartbeat_timeout = std::chrono::seconds(2),
    .election_timeout_min = std::chrono::seconds(10),
    .election_timeout_max = std::chrono::seconds(20),
    .default_commit_timeout = std::chrono::minutes(2),
    .majority_collection_timeout = std::chrono::seconds(15),
    .config_change_timeout = std::chrono::minutes(5),
    .max_batch_size = 50,  // Smaller batches for unreliable network
    .min_log_level = log_level::warning
};

// More aggressive retry for WAN
wan_config.default_retry_policy.max_attempts = 7;
wan_config.default_retry_policy.max_delay = std::chrono::seconds(30);
```

### High-Performance Configuration

```cpp
raft_configuration high_perf_config{
    .rpc_timeout = std::chrono::seconds(2),
    .heartbeat_timeout = std::chrono::milliseconds(200),
    .election_timeout_min = std::chrono::milliseconds(1000),
    .election_timeout_max = std::chrono::milliseconds(2000),
    .default_commit_timeout = std::chrono::seconds(10),
    .commit_check_interval = std::chrono::milliseconds(25),
    .majority_collection_timeout = std::chrono::seconds(2),
    .max_batch_size = 1000,
    .max_pending_operations = 20000,
    .enable_pipelining = true
};
```

## Adaptive Configuration

### Network Condition Detection

```cpp
class AdaptiveRaftConfig {
private:
    raft_configuration base_config;
    std::chrono::steady_clock::time_point last_adjustment;
    double current_latency_ms{0.0};
    double current_loss_rate{0.0};
    
public:
    auto adjust_for_network_conditions(
        double avg_latency_ms,
        double packet_loss_rate
    ) -> void {
        current_latency_ms = avg_latency_ms;
        current_loss_rate = packet_loss_rate;
        
        // Adjust timeouts based on latency
        auto latency_multiplier = std::max(1.0, avg_latency_ms / 10.0);
        base_config.rpc_timeout = std::chrono::milliseconds(
            static_cast<int>(5000 * latency_multiplier));
        
        // Adjust retry policy based on loss rate
        if (packet_loss_rate > 0.05) {  // > 5% loss
            base_config.default_retry_policy.max_attempts = 7;
            base_config.default_retry_policy.initial_delay = 
                std::chrono::milliseconds(200);
        } else {
            base_config.default_retry_policy.max_attempts = 3;
            base_config.default_retry_policy.initial_delay = 
                std::chrono::milliseconds(100);
        }
    }
    
    auto get_config() const -> const raft_configuration& {
        return base_config;
    }
};
```

### Load-Based Adjustment

```cpp
class LoadAwareRaftConfig {
private:
    raft_configuration base_config;
    
public:
    auto adjust_for_load(
        std::size_t current_pending_ops,
        double cpu_usage,
        double memory_usage
    ) -> void {
        // Adjust batch size based on load
        if (cpu_usage > 0.8) {
            // High CPU - reduce batch size to lower latency
            base_config.max_batch_size = 50;
        } else if (cpu_usage < 0.3) {
            // Low CPU - increase batch size for throughput
            base_config.max_batch_size = 500;
        }
        
        // Adjust pending operations limit based on memory
        if (memory_usage > 0.9) {
            base_config.max_pending_operations = 500;
        } else if (memory_usage < 0.5) {
            base_config.max_pending_operations = 10000;
        }
        
        // Adjust commit timeout based on pending operations
        if (current_pending_ops > base_config.max_pending_operations * 0.8) {
            // High load - increase timeout to avoid spurious failures
            base_config.default_commit_timeout = std::chrono::minutes(2);
        } else {
            base_config.default_commit_timeout = std::chrono::seconds(30);
        }
    }
};
```

## Validation and Best Practices

### Configuration Validation

```cpp
auto validate_configuration(const raft_configuration& config) -> std::vector<std::string> {
    std::vector<std::string> errors;
    
    // Validate timeout relationships
    if (config.heartbeat_timeout >= config.election_timeout_min) {
        errors.push_back("heartbeat_timeout must be less than election_timeout_min");
    }
    
    if (config.election_timeout_min >= config.election_timeout_max) {
        errors.push_back("election_timeout_min must be less than election_timeout_max");
    }
    
    if (config.config_phase_timeout >= config.config_change_timeout) {
        errors.push_back("config_phase_timeout must be less than config_change_timeout");
    }
    
    // Validate retry policy
    if (config.default_retry_policy.max_attempts == 0) {
        errors.push_back("retry policy max_attempts must be > 0");
    }
    
    if (config.default_retry_policy.backoff_multiplier <= 1.0) {
        errors.push_back("retry policy backoff_multiplier must be > 1.0");
    }
    
    // Validate performance settings
    if (config.max_batch_size == 0) {
        errors.push_back("max_batch_size must be > 0");
    }
    
    if (config.max_pending_operations == 0) {
        errors.push_back("max_pending_operations must be > 0");
    }
    
    return errors;
}
```

### Best Practices

1. **Start Conservative**: Begin with longer timeouts and reduce as needed
2. **Monitor Metrics**: Use detailed metrics to guide configuration tuning
3. **Test Under Load**: Validate configuration under realistic load conditions
4. **Document Changes**: Keep track of configuration changes and their effects

### Common Pitfalls

1. **Too Aggressive Timeouts**: Can cause spurious failures and instability
2. **Inconsistent Retry Policies**: Different operations need different retry strategies
3. **Ignoring Network Characteristics**: Configuration must match network conditions
4. **Static Configuration**: Consider adaptive configuration for varying conditions

## Monitoring Configuration Effectiveness

### Key Metrics to Monitor

```cpp
// Timeout rates
metrics.gauge("raft.timeouts.commit_rate", commit_timeout_rate);
metrics.gauge("raft.timeouts.rpc_rate", rpc_timeout_rate);
metrics.gauge("raft.timeouts.election_rate", election_timeout_rate);

// Retry statistics
metrics.histogram("raft.retries.attempts", retry_attempts);
metrics.histogram("raft.retries.delay", retry_delay_ms);

// Performance metrics
metrics.histogram("raft.operations.commit_latency", commit_latency_ms);
metrics.histogram("raft.operations.read_latency", read_latency_ms);
metrics.gauge("raft.operations.pending_count", pending_operations);

// Configuration change metrics
metrics.histogram("raft.config_change.duration", config_change_duration_ms);
metrics.counter("raft.config_change.timeouts").increment();
```

### Alerting Thresholds

```cpp
// Alert if timeout rates are too high
if (commit_timeout_rate > 0.05) {  // > 5%
    alert("High commit timeout rate: consider increasing default_commit_timeout");
}

if (rpc_timeout_rate > 0.10) {  // > 10%
    alert("High RPC timeout rate: consider increasing rpc_timeout");
}

// Alert if retry rates are excessive
if (avg_retry_attempts > 2.0) {
    alert("High retry rate: check network conditions and retry policies");
}

// Alert if pending operations are backing up
if (pending_operations > max_pending_operations * 0.9) {
    alert("Pending operations near limit: consider increasing max_pending_operations");
}
```

## Configuration Examples

### Complete Production Configuration

```cpp
raft_configuration create_production_config() {
    raft_configuration config;
    
    // Basic timeouts
    config.rpc_timeout = std::chrono::seconds(5);
    config.heartbeat_timeout = std::chrono::milliseconds(500);
    config.election_timeout_min = std::chrono::milliseconds(2000);
    config.election_timeout_max = std::chrono::milliseconds(4000);
    
    // Client operation timeouts
    config.default_commit_timeout = std::chrono::seconds(30);
    config.commit_check_interval = std::chrono::milliseconds(100);
    
    // Future collection timeouts
    config.majority_collection_timeout = std::chrono::seconds(5);
    config.heartbeat_collection_timeout = std::chrono::seconds(2);
    
    // Configuration change timeouts
    config.config_change_timeout = std::chrono::seconds(60);
    config.config_phase_timeout = std::chrono::seconds(25);
    
    // Retry policies
    config.default_retry_policy = {
        .initial_delay = std::chrono::milliseconds(100),
        .max_delay = std::chrono::seconds(5),
        .backoff_multiplier = 2.0,
        .max_attempts = 3,
        .jitter_factor = 0.1
    };
    
    config.heartbeat_retry_policy = {
        .initial_delay = std::chrono::milliseconds(50),
        .max_delay = std::chrono::milliseconds(500),
        .backoff_multiplier = 1.5,
        .max_attempts = 3,
        .jitter_factor = 0.2
    };
    
    // Performance settings
    config.max_batch_size = 200;
    config.max_pending_operations = 5000;
    config.enable_pipelining = true;
    
    // Logging
    config.min_log_level = log_level::info;
    config.enable_detailed_metrics = false;
    
    return config;
}
```

This configuration provides a solid foundation for production deployments and can be adjusted based on specific requirements and monitoring feedback.