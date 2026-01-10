# Raft Completion Usage Examples

This document provides practical examples of using the Raft completion components in real-world scenarios.

## Basic Usage Examples

### Simple Client Operations

#### Submit Command with Error Handling

```cpp
#include "raft/raft.hpp"
#include "raft/completion_exceptions.hpp"

class RaftClient {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
public:
    auto submit_user_command(const std::string& command_data) 
        -> kythira::Future<std::string> {
        
        // Convert command to bytes
        std::vector<std::byte> command_bytes;
        std::transform(command_data.begin(), command_data.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        // Submit with 30-second timeout
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) -> std::string {
                // Convert result back to string
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            })
            .thenError([](const std::exception& e) -> std::string {
                // Handle specific error types
                if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
                    throw std::runtime_error(
                        "Command timed out after " + 
                        std::to_string(timeout_ex->get_timeout().count()) + "ms");
                } else if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    throw std::runtime_error(
                        "Leadership lost during command execution");
                } else {
                    throw std::runtime_error("Command failed: " + std::string(e.what()));
                }
            });
    }
};
```

#### Read State with Linearizability

```cpp
class RaftReader {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
public:
    auto read_current_state() -> kythira::Future<std::map<std::string, std::string>> {
        return raft_node->read_state(std::chrono::seconds(5))
            .thenValue([](std::vector<std::byte> state_bytes) {
                // Deserialize state from bytes
                return deserialize_state_map(state_bytes);
            })
            .thenError([](const std::exception& e) -> std::map<std::string, std::string> {
                if (auto collection_ex = dynamic_cast<const raft::future_collection_exception*>(&e)) {
                    throw std::runtime_error(
                        "Failed to verify leadership: " + 
                        std::to_string(collection_ex->get_failed_count()) + " nodes unreachable");
                } else {
                    throw std::runtime_error("Read failed: " + std::string(e.what()));
                }
            });
    }
    
private:
    auto deserialize_state_map(const std::vector<std::byte>& bytes) 
        -> std::map<std::string, std::string> {
        // Implementation depends on your serialization format
        // This is a simplified example
        std::map<std::string, std::string> result;
        // ... deserialization logic ...
        return result;
    }
};
```

### Configuration Management

#### Add Server to Cluster

```cpp
class RaftClusterManager {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
public:
    auto add_server_to_cluster(std::uint64_t new_node_id) 
        -> kythira::Future<bool> {
        
        return raft_node->add_server(new_node_id)
            .thenValue([new_node_id](bool success) -> bool {
                if (success) {
                    std::cout << "Successfully added server " << new_node_id 
                              << " to cluster" << std::endl;
                } else {
                    std::cout << "Failed to add server " << new_node_id 
                              << " to cluster" << std::endl;
                }
                return success;
            })
            .thenError([new_node_id](const std::exception& e) -> bool {
                if (auto config_ex = dynamic_cast<const raft::configuration_change_exception*>(&e)) {
                    std::cerr << "Configuration change failed in phase " 
                              << config_ex->get_phase() << ": " 
                              << config_ex->get_reason() << std::endl;
                } else {
                    std::cerr << "Failed to add server " << new_node_id 
                              << ": " << e.what() << std::endl;
                }
                return false;
            });
    }
    
    auto remove_server_from_cluster(std::uint64_t node_id) 
        -> kythira::Future<bool> {
        
        return raft_node->remove_server(node_id)
            .thenValue([node_id](bool success) -> bool {
                if (success) {
                    std::cout << "Successfully removed server " << node_id 
                              << " from cluster" << std::endl;
                } else {
                    std::cout << "Failed to remove server " << node_id 
                              << " from cluster" << std::endl;
                }
                return success;
            })
            .thenError([node_id](const std::exception& e) -> bool {
                std::cerr << "Failed to remove server " << node_id 
                          << ": " << e.what() << std::endl;
                return false;
            });
    }
};
```

## Advanced Usage Patterns

### Batch Operations

```cpp
class RaftBatchClient {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
public:
    auto submit_batch_commands(const std::vector<std::string>& commands)
        -> kythira::Future<std::vector<std::string>> {
        
        // Submit all commands concurrently
        std::vector<kythira::Future<std::string>> command_futures;
        command_futures.reserve(commands.size());
        
        for (const auto& command : commands) {
            auto future = submit_single_command(command);
            command_futures.push_back(std::move(future));
        }
        
        // Wait for all commands to complete
        return kythira::FutureCollector::collectAll(std::move(command_futures))
            .thenValue([](std::vector<kythira::Try<std::string>> results) {
                std::vector<std::string> successful_results;
                
                for (auto& try_result : results) {
                    if (try_result.hasValue()) {
                        successful_results.push_back(std::move(try_result.value()));
                    } else {
                        // Log failed command but continue with others
                        std::cerr << "Command failed: " << try_result.exception() << std::endl;
                    }
                }
                
                return successful_results;
            });
    }
    
private:
    auto submit_single_command(const std::string& command) 
        -> kythira::Future<std::string> {
        
        std::vector<std::byte> command_bytes;
        std::transform(command.begin(), command.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) {
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            });
    }
};
```

### Retry Logic with Exponential Backoff

```cpp
class ResilientRaftClient {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    static constexpr int MAX_RETRIES = 3;
    
public:
    auto submit_command_with_retry(const std::string& command)
        -> kythira::Future<std::string> {
        return submit_command_with_retry_impl(command, 0);
    }
    
private:
    auto submit_command_with_retry_impl(const std::string& command, int attempt)
        -> kythira::Future<std::string> {
        
        std::vector<std::byte> command_bytes;
        std::transform(command.begin(), command.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) {
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            })
            .thenError([this, command, attempt](const std::exception& e) 
                -> kythira::Future<std::string> {
                
                bool should_retry = false;
                
                // Determine if we should retry based on error type
                if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    should_retry = true;  // Always retry on leadership loss
                } else if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
                    should_retry = (attempt < MAX_RETRIES);  // Retry timeouts with limit
                } else if (auto collection_ex = dynamic_cast<const raft::future_collection_exception*>(&e)) {
                    should_retry = (attempt < MAX_RETRIES);  // Retry collection failures
                }
                
                if (should_retry && attempt < MAX_RETRIES) {
                    // Calculate delay with exponential backoff
                    auto delay = std::chrono::milliseconds(100 * (1 << attempt));  // 100ms, 200ms, 400ms
                    
                    std::cout << "Retrying command (attempt " << (attempt + 1) 
                              << ") after " << delay.count() << "ms delay" << std::endl;
                    
                    // Wait before retry
                    return kythira::FutureFactory::makeFuture()
                        .delayed(delay)
                        .thenValue([this, command, attempt](auto) {
                            return submit_command_with_retry_impl(command, attempt + 1);
                        });
                } else {
                    // Give up and propagate error
                    return kythira::FutureFactory::makeExceptionalFuture<std::string>(
                        std::current_exception());
                }
            });
    }
};
```

### Leader Discovery and Failover

```cpp
class RaftClusterClient {
private:
    std::vector<std::shared_ptr<raft::node</* template params */>>> cluster_nodes;
    std::optional<std::size_t> current_leader_index;
    
public:
    auto submit_command(const std::string& command) 
        -> kythira::Future<std::string> {
        
        if (current_leader_index && *current_leader_index < cluster_nodes.size()) {
            return try_submit_to_node(*current_leader_index, command);
        } else {
            return discover_leader_and_submit(command);
        }
    }
    
private:
    auto try_submit_to_node(std::size_t node_index, const std::string& command)
        -> kythira::Future<std::string> {
        
        auto& node = cluster_nodes[node_index];
        
        std::vector<std::byte> command_bytes;
        std::transform(command.begin(), command.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) {
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            })
            .thenError([this, command](const std::exception& e) 
                -> kythira::Future<std::string> {
                
                if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    // Leader changed, clear cached leader and retry
                    current_leader_index.reset();
                    std::cout << "Leadership lost, discovering new leader..." << std::endl;
                    return discover_leader_and_submit(command);
                } else {
                    // Other error, propagate
                    return kythira::FutureFactory::makeExceptionalFuture<std::string>(
                        std::current_exception());
                }
            });
    }
    
    auto discover_leader_and_submit(const std::string& command)
        -> kythira::Future<std::string> {
        
        return try_nodes_sequentially(0, command);
    }
    
    auto try_nodes_sequentially(std::size_t node_index, const std::string& command)
        -> kythira::Future<std::string> {
        
        if (node_index >= cluster_nodes.size()) {
            return kythira::FutureFactory::makeExceptionalFuture<std::string>(
                std::runtime_error("No leader found in cluster"));
        }
        
        return try_submit_to_node(node_index, command)
            .thenError([this, node_index, command](const std::exception& e) 
                -> kythira::Future<std::string> {
                
                // If this node is not the leader, try the next one
                std::cout << "Node " << node_index << " is not leader, trying next..." << std::endl;
                return try_nodes_sequentially(node_index + 1, command);
            });
    }
};
```

## State Machine Integration

### Custom State Machine

```cpp
class KeyValueStateMachine {
private:
    std::map<std::string, std::string> data;
    mutable std::shared_mutex mutex;
    
public:
    // Apply a command to the state machine
    auto apply_command(const std::vector<std::byte>& command) 
        -> std::vector<std::byte> {
        
        // Deserialize command
        auto cmd = deserialize_command(command);
        
        std::unique_lock<std::shared_mutex> lock(mutex);
        
        std::vector<std::byte> result;
        
        if (cmd.type == "SET") {
            data[cmd.key] = cmd.value;
            result = serialize_response("OK");
        } else if (cmd.type == "GET") {
            auto it = data.find(cmd.key);
            if (it != data.end()) {
                result = serialize_response(it->second);
            } else {
                result = serialize_response("NOT_FOUND");
            }
        } else if (cmd.type == "DELETE") {
            auto erased = data.erase(cmd.key);
            result = serialize_response(erased > 0 ? "DELETED" : "NOT_FOUND");
        } else {
            result = serialize_response("UNKNOWN_COMMAND");
        }
        
        return result;
    }
    
    // Get current state for reads
    auto get_state() const -> std::vector<std::byte> {
        std::shared_lock<std::shared_mutex> lock(mutex);
        return serialize_state(data);
    }
    
private:
    struct Command {
        std::string type;
        std::string key;
        std::string value;
    };
    
    auto deserialize_command(const std::vector<std::byte>& bytes) -> Command {
        // Implementation depends on your serialization format
        // This is a simplified example using JSON-like format
        Command cmd;
        // ... deserialization logic ...
        return cmd;
    }
    
    auto serialize_response(const std::string& response) -> std::vector<std::byte> {
        std::vector<std::byte> result;
        std::transform(response.begin(), response.end(),
                      std::back_inserter(result),
                      [](char c) { return static_cast<std::byte>(c); });
        return result;
    }
    
    auto serialize_state(const std::map<std::string, std::string>& state) 
        -> std::vector<std::byte> {
        // Serialize entire state map
        std::string state_str = "{";
        bool first = true;
        for (const auto& [key, value] : state) {
            if (!first) state_str += ",";
            state_str += "\"" + key + "\":\"" + value + "\"";
            first = false;
        }
        state_str += "}";
        
        std::vector<std::byte> result;
        std::transform(state_str.begin(), state_str.end(),
                      std::back_inserter(result),
                      [](char c) { return static_cast<std::byte>(c); });
        return result;
    }
};
```

### Integrating State Machine with Raft

```cpp
class RaftKeyValueServer {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    KeyValueStateMachine state_machine;
    
public:
    auto set_value(const std::string& key, const std::string& value)
        -> kythira::Future<std::string> {
        
        // Create SET command
        std::string command_str = "SET " + key + " " + value;
        std::vector<std::byte> command_bytes;
        std::transform(command_str.begin(), command_str.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) {
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            });
    }
    
    auto get_value(const std::string& key) 
        -> kythira::Future<std::string> {
        
        // For reads, we can use read_state for linearizable consistency
        return raft_node->read_state(std::chrono::seconds(5))
            .thenValue([key](std::vector<std::byte> state_bytes) {
                // Parse state and extract value for key
                std::string state_str;
                std::transform(state_bytes.begin(), state_bytes.end(),
                              std::back_inserter(state_str),
                              [](std::byte b) { return static_cast<char>(b); });
                
                // Simple JSON-like parsing (in production, use proper JSON library)
                auto key_pos = state_str.find("\"" + key + "\":");
                if (key_pos != std::string::npos) {
                    auto value_start = state_str.find("\"", key_pos + key.length() + 3);
                    auto value_end = state_str.find("\"", value_start + 1);
                    if (value_start != std::string::npos && value_end != std::string::npos) {
                        return state_str.substr(value_start + 1, value_end - value_start - 1);
                    }
                }
                return std::string("NOT_FOUND");
            });
    }
    
    auto delete_value(const std::string& key)
        -> kythira::Future<std::string> {
        
        // Create DELETE command
        std::string command_str = "DELETE " + key;
        std::vector<std::byte> command_bytes;
        std::transform(command_str.begin(), command_str.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([](std::vector<std::byte> result) {
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            });
    }
};
```

## Error Handling Patterns

### Comprehensive Error Handler

```cpp
class RaftErrorHandler {
public:
    template<typename T>
    static auto handle_raft_error(const std::exception& e) -> std::string {
        if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
            return "Commit timeout for entry " + 
                   std::to_string(timeout_ex->get_entry_index()) + 
                   " after " + std::to_string(timeout_ex->get_timeout().count()) + "ms";
                   
        } else if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
            return "Leadership lost: term changed from " + 
                   std::to_string(leadership_ex->get_old_term()) + 
                   " to " + std::to_string(leadership_ex->get_new_term());
                   
        } else if (auto collection_ex = dynamic_cast<const raft::future_collection_exception*>(&e)) {
            return "Future collection failed for operation '" + 
                   collection_ex->get_operation() + "': " + 
                   std::to_string(collection_ex->get_failed_count()) + " futures failed";
                   
        } else if (auto config_ex = dynamic_cast<const raft::configuration_change_exception*>(&e)) {
            return "Configuration change failed in phase '" + 
                   config_ex->get_phase() + "': " + config_ex->get_reason();
                   
        } else if (auto completion_ex = dynamic_cast<const raft::raft_completion_exception*>(&e)) {
            return "Raft completion error: " + std::string(completion_ex->what());
            
        } else {
            return "Unknown error: " + std::string(e.what());
        }
    }
    
    template<typename T>
    static auto create_error_handler() {
        return [](const std::exception& e) -> T {
            auto error_msg = handle_raft_error<T>(e);
            std::cerr << "Raft operation failed: " << error_msg << std::endl;
            
            // Log error details for debugging
            // logger.error("Raft error", {{"error", error_msg}, {"type", typeid(e).name()}});
            
            // Re-throw with more context
            throw std::runtime_error("Raft operation failed: " + error_msg);
        };
    }
};
```

### Circuit Breaker Pattern

```cpp
class RaftCircuitBreaker {
private:
    enum class State { CLOSED, OPEN, HALF_OPEN };
    
    State state{State::CLOSED};
    std::size_t failure_count{0};
    std::chrono::steady_clock::time_point last_failure_time;
    
    static constexpr std::size_t FAILURE_THRESHOLD = 5;
    static constexpr auto TIMEOUT_DURATION = std::chrono::seconds(30);
    
public:
    template<typename Operation>
    auto execute(Operation&& op) -> decltype(op()) {
        if (state == State::OPEN) {
            if (std::chrono::steady_clock::now() - last_failure_time > TIMEOUT_DURATION) {
                state = State::HALF_OPEN;
                std::cout << "Circuit breaker transitioning to HALF_OPEN" << std::endl;
            } else {
                throw std::runtime_error("Circuit breaker is OPEN");
            }
        }
        
        try {
            auto result = op();
            
            // Success - reset failure count
            if (state == State::HALF_OPEN) {
                state = State::CLOSED;
                std::cout << "Circuit breaker transitioning to CLOSED" << std::endl;
            }
            failure_count = 0;
            
            return result;
            
        } catch (const std::exception& e) {
            failure_count++;
            last_failure_time = std::chrono::steady_clock::now();
            
            if (failure_count >= FAILURE_THRESHOLD) {
                state = State::OPEN;
                std::cout << "Circuit breaker transitioning to OPEN after " 
                          << failure_count << " failures" << std::endl;
            }
            
            throw;  // Re-throw the exception
        }
    }
    
    auto get_state() const -> State { return state; }
    auto get_failure_count() const -> std::size_t { return failure_count; }
};
```

## Monitoring and Observability

### Metrics Collection

```cpp
class RaftMetricsCollector {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
    // Metrics storage (in production, use proper metrics library)
    std::atomic<std::size_t> total_commands{0};
    std::atomic<std::size_t> successful_commands{0};
    std::atomic<std::size_t> failed_commands{0};
    std::atomic<std::size_t> timeout_commands{0};
    std::atomic<std::size_t> leadership_lost_commands{0};
    
public:
    auto submit_command_with_metrics(const std::string& command)
        -> kythira::Future<std::string> {
        
        total_commands++;
        auto start_time = std::chrono::steady_clock::now();
        
        std::vector<std::byte> command_bytes;
        std::transform(command.begin(), command.end(),
                      std::back_inserter(command_bytes),
                      [](char c) { return static_cast<std::byte>(c); });
        
        return raft_node->submit_command(command_bytes, std::chrono::seconds(30))
            .thenValue([this, start_time](std::vector<std::byte> result) {
                successful_commands++;
                
                auto duration = std::chrono::steady_clock::now() - start_time;
                auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(duration);
                
                // Record latency metric
                record_latency_metric(duration_ms.count());
                
                std::string result_str;
                std::transform(result.begin(), result.end(),
                              std::back_inserter(result_str),
                              [](std::byte b) { return static_cast<char>(b); });
                return result_str;
            })
            .thenError([this](const std::exception& e) -> std::string {
                failed_commands++;
                
                // Record specific error types
                if (auto timeout_ex = dynamic_cast<const raft::commit_timeout_exception<>*>(&e)) {
                    timeout_commands++;
                } else if (auto leadership_ex = dynamic_cast<const raft::leadership_lost_exception<>*>(&e)) {
                    leadership_lost_commands++;
                }
                
                throw;  // Re-throw
            });
    }
    
    auto get_metrics() const -> std::map<std::string, std::size_t> {
        return {
            {"total_commands", total_commands.load()},
            {"successful_commands", successful_commands.load()},
            {"failed_commands", failed_commands.load()},
            {"timeout_commands", timeout_commands.load()},
            {"leadership_lost_commands", leadership_lost_commands.load()}
        };
    }
    
private:
    auto record_latency_metric(long latency_ms) -> void {
        // In production, use proper histogram/percentile tracking
        std::cout << "Command latency: " << latency_ms << "ms" << std::endl;
    }
};
```

### Health Checking

```cpp
class RaftHealthChecker {
private:
    std::shared_ptr<raft::node</* template params */>> raft_node;
    
public:
    struct HealthStatus {
        bool is_healthy{false};
        bool is_leader{false};
        std::string status_message;
        std::chrono::milliseconds last_operation_latency{0};
    };
    
    auto check_health() -> kythira::Future<HealthStatus> {
        HealthStatus status;
        status.is_leader = raft_node->is_leader();
        
        if (status.is_leader) {
            // For leaders, test a simple read operation
            return check_leader_health();
        } else {
            // For followers, just check if node is running
            status.is_healthy = raft_node->is_running();
            status.status_message = status.is_healthy ? "Follower healthy" : "Node not running";
            return kythira::FutureFactory::makeFuture(status);
        }
    }
    
private:
    auto check_leader_health() -> kythira::Future<HealthStatus> {
        auto start_time = std::chrono::steady_clock::now();
        
        return raft_node->read_state(std::chrono::seconds(5))
            .thenValue([start_time](std::vector<std::byte> state) {
                HealthStatus status;
                status.is_healthy = true;
                status.is_leader = true;
                status.status_message = "Leader healthy";
                
                auto duration = std::chrono::steady_clock::now() - start_time;
                status.last_operation_latency = 
                    std::chrono::duration_cast<std::chrono::milliseconds>(duration);
                
                return status;
            })
            .thenError([](const std::exception& e) {
                HealthStatus status;
                status.is_healthy = false;
                status.is_leader = false;  // Probably lost leadership
                status.status_message = "Health check failed: " + std::string(e.what());
                return status;
            });
    }
};
```

These examples demonstrate practical usage patterns for the Raft completion components, covering everything from basic operations to advanced patterns like retry logic, circuit breakers, and monitoring. They provide a solid foundation for building production-ready applications using the Raft consensus algorithm.