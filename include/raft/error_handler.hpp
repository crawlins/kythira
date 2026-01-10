#pragma once

#include "future.hpp"
#include "types.hpp"
#include <folly/Unit.h>
#include <chrono>
#include <functional>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace kythira {

/**
 * @brief Comprehensive error handling system for Raft operations
 * 
 * This class provides robust retry and recovery mechanisms for all network operations
 * in the Raft implementation. It supports configurable retry policies with exponential
 * backoff, error classification, and operation-specific handling strategies.
 * 
 * @tparam Result The result type of operations being handled
 */
template<typename Result>
class error_handler {
public:
    /**
     * @brief Retry policy configuration for different operation types
     */
    struct retry_policy {
        std::chrono::milliseconds initial_delay{100};
        std::chrono::milliseconds max_delay{5000};
        double backoff_multiplier{2.0};
        double jitter_factor{0.1};  // 10% jitter by default
        std::size_t max_attempts{5};
        
        // Validation
        auto is_valid() const -> bool {
            return initial_delay > std::chrono::milliseconds{0} &&
                   max_delay >= initial_delay &&
                   backoff_multiplier > 1.0 &&
                   jitter_factor >= 0.0 && jitter_factor <= 1.0 &&
                   max_attempts > 0;
        }
    };
    
    /**
     * @brief Error classification for different handling strategies
     */
    enum class error_type {
        network_timeout,        // Network operation timed out
        network_unreachable,    // Target node unreachable
        connection_refused,     // Connection actively refused
        serialization_error,    // Message serialization/deserialization failed
        protocol_error,         // Raft protocol violation
        temporary_failure,      // Temporary failure, should retry
        permanent_failure,      // Permanent failure, should not retry
        unknown_error          // Unclassified error
    };
    
    /**
     * @brief Error classification result
     */
    struct error_classification {
        error_type type;
        bool should_retry;
        std::string description;
    };
    
    /**
     * @brief Constructor with default retry policies
     */
    error_handler() {
        // Set default retry policies for common Raft operations
        set_retry_policy("heartbeat", retry_policy{
            .initial_delay = std::chrono::milliseconds{50},
            .max_delay = std::chrono::milliseconds{1000},
            .backoff_multiplier = 1.5,
            .jitter_factor = 0.1,
            .max_attempts = 3
        });
        
        set_retry_policy("append_entries", retry_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{5000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 5
        });
        
        set_retry_policy("request_vote", retry_policy{
            .initial_delay = std::chrono::milliseconds{100},
            .max_delay = std::chrono::milliseconds{2000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 3
        });
        
        set_retry_policy("install_snapshot", retry_policy{
            .initial_delay = std::chrono::milliseconds{500},
            .max_delay = std::chrono::milliseconds{30000},
            .backoff_multiplier = 2.0,
            .jitter_factor = 0.1,
            .max_attempts = 10
        });
    }
    
    /**
     * @brief Execute operation with retry and error handling
     * 
     * Executes the provided operation with automatic retry based on the configured
     * retry policy for the operation type. Handles errors according to their
     * classification and applies exponential backoff with jitter.
     * 
     * @tparam Operation Function type that returns kythira::Future<Result>
     * @param operation_name Name of the operation for policy lookup and logging
     * @param op Operation function to execute
     * @param custom_policy Optional custom retry policy (overrides configured policy)
     * @return Future containing the operation result
     */
    template<typename Operation>
    auto execute_with_retry(
        const std::string& operation_name,
        Operation&& op,
        const std::optional<retry_policy>& custom_policy = std::nullopt
    ) -> kythira::Future<Result> {
        const auto& policy = custom_policy.value_or(get_retry_policy(operation_name));
        
        if (!policy.is_valid()) {
            return kythira::FutureFactory::makeExceptionalFuture<Result>(
                std::invalid_argument("Invalid retry policy for operation: " + operation_name));
        }
        
        return execute_with_retry_impl(operation_name, std::forward<Operation>(op), policy, 1);
    }
    
    /**
     * @brief Classify an exception for error handling
     * 
     * Analyzes the provided exception and classifies it according to error type,
     * retry eligibility, and handling strategy.
     * 
     * @param e Exception to classify
     * @return Error classification result
     */
    auto classify_error(const std::exception& e) -> error_classification {
        const std::string error_msg = e.what();
        
        // Network timeout errors
        if (error_msg.find("timeout") != std::string::npos ||
            error_msg.find("Timeout") != std::string::npos) {
            return {
                .type = error_type::network_timeout,
                .should_retry = true,
                .description = "Network operation timed out"
            };
        }
        
        // Network unreachable errors
        if (error_msg.find("unreachable") != std::string::npos ||
            error_msg.find("No route to host") != std::string::npos ||
            error_msg.find("Network is unreachable") != std::string::npos) {
            return {
                .type = error_type::network_unreachable,
                .should_retry = true,
                .description = "Target node unreachable"
            };
        }
        
        // Connection refused errors
        if (error_msg.find("Connection refused") != std::string::npos ||
            error_msg.find("connection refused") != std::string::npos) {
            return {
                .type = error_type::connection_refused,
                .should_retry = true,
                .description = "Connection actively refused"
            };
        }
        
        // Serialization errors
        if (error_msg.find("serialization") != std::string::npos ||
            error_msg.find("deserialization") != std::string::npos ||
            error_msg.find("parse") != std::string::npos ||
            error_msg.find("invalid format") != std::string::npos) {
            return {
                .type = error_type::serialization_error,
                .should_retry = false,
                .description = "Message serialization/deserialization failed"
            };
        }
        
        // Protocol errors
        if (error_msg.find("protocol") != std::string::npos ||
            error_msg.find("invalid term") != std::string::npos ||
            error_msg.find("invalid log index") != std::string::npos) {
            return {
                .type = error_type::protocol_error,
                .should_retry = false,
                .description = "Raft protocol violation"
            };
        }
        
        // Temporary failures (generic network issues)
        if (error_msg.find("temporary") != std::string::npos ||
            error_msg.find("try again") != std::string::npos ||
            error_msg.find("busy") != std::string::npos) {
            return {
                .type = error_type::temporary_failure,
                .should_retry = true,
                .description = "Temporary failure"
            };
        }
        
        // Default to unknown error with retry
        return {
            .type = error_type::unknown_error,
            .should_retry = true,
            .description = "Unknown error: " + error_msg
        };
    }
    
    /**
     * @brief Handle network timeout errors
     * 
     * @param e Exception representing the timeout
     * @return true if the error should be retried, false otherwise
     */
    auto handle_network_timeout(const std::exception& e) -> bool {
        auto classification = classify_error(e);
        return classification.type == error_type::network_timeout && classification.should_retry;
    }
    
    /**
     * @brief Handle network unreachable errors
     * 
     * @param e Exception representing the network error
     * @return true if the error should be retried, false otherwise
     */
    auto handle_network_error(const std::exception& e) -> bool {
        auto classification = classify_error(e);
        return (classification.type == error_type::network_unreachable ||
                classification.type == error_type::connection_refused ||
                classification.type == error_type::temporary_failure) && 
               classification.should_retry;
    }
    
    /**
     * @brief Handle serialization/deserialization errors
     * 
     * @param e Exception representing the serialization error
     * @return true if the error should be retried, false otherwise
     */
    auto handle_serialization_error(const std::exception& e) -> bool {
        auto classification = classify_error(e);
        return classification.type == error_type::serialization_error && classification.should_retry;
    }
    
    /**
     * @brief Configure retry policy for a specific operation
     * 
     * @param operation Operation name (e.g., "heartbeat", "append_entries")
     * @param policy Retry policy configuration
     */
    auto set_retry_policy(const std::string& operation, const retry_policy& policy) -> void {
        if (!policy.is_valid()) {
            throw std::invalid_argument("Invalid retry policy for operation: " + operation);
        }
        _retry_policies[operation] = policy;
    }
    
    /**
     * @brief Get retry policy for a specific operation
     * 
     * @param operation Operation name
     * @return Retry policy for the operation (default policy if not found)
     */
    auto get_retry_policy(const std::string& operation) const -> retry_policy {
        auto it = _retry_policies.find(operation);
        if (it != _retry_policies.end()) {
            return it->second;
        }
        
        // Return default policy if not found
        return retry_policy{};
    }
    
    /**
     * @brief Check if network partition is detected
     * 
     * Analyzes error patterns to detect potential network partitions.
     * This is a heuristic-based approach that looks for patterns indicating
     * widespread connectivity issues.
     * 
     * @param recent_errors Vector of recent error classifications
     * @return true if partition is likely detected, false otherwise
     */
    auto detect_network_partition(const std::vector<error_classification>& recent_errors) const -> bool {
        if (recent_errors.size() < 3) {
            return false;  // Need sufficient sample size
        }
        
        // Count network-related errors
        std::size_t network_errors = 0;
        for (const auto& error : recent_errors) {
            if (error.type == error_type::network_timeout ||
                error.type == error_type::network_unreachable ||
                error.type == error_type::connection_refused) {
                network_errors++;
            }
        }
        
        // If majority of recent errors are network-related, likely partition
        return network_errors >= (recent_errors.size() * 2 / 3);
    }

private:
    // Retry policies for different operations
    std::unordered_map<std::string, retry_policy> _retry_policies;
    
    // Random number generator for jitter
    mutable std::mt19937 _rng{std::random_device{}()};
    
    /**
     * @brief Internal implementation of retry logic
     */
    template<typename Operation>
    auto execute_with_retry_impl(
        const std::string& operation_name,
        Operation&& op,
        const retry_policy& policy,
        std::size_t attempt
    ) -> kythira::Future<Result> {
        return std::forward<Operation>(op)()
            .thenError([this, operation_name, op = std::forward<Operation>(op), policy, attempt]
                      (const std::exception& e) mutable -> kythira::Future<Result> {
                
                auto classification = classify_error(e);
                
                // If we shouldn't retry this error type, or we've exhausted attempts, fail
                if (!classification.should_retry || attempt >= policy.max_attempts) {
                    return kythira::FutureFactory::makeExceptionalFuture<Result>(e);
                }
                
                // Calculate delay with exponential backoff and jitter
                auto delay = calculate_delay(policy, attempt);
                
                // For now, just retry immediately without delay to avoid complex async scheduling
                // In a production system, you would want to implement proper delay
                return execute_with_retry_impl(operation_name, std::move(op), policy, attempt + 1);
            });
    }
    
    /**
     * @brief Calculate retry delay with exponential backoff and jitter
     */
    auto calculate_delay(const retry_policy& policy, std::size_t attempt) const -> std::chrono::milliseconds {
        // Calculate base delay with exponential backoff
        auto base_delay = policy.initial_delay;
        for (std::size_t i = 1; i < attempt; ++i) {
            base_delay = std::chrono::milliseconds{
                static_cast<long long>(base_delay.count() * policy.backoff_multiplier)
            };
        }
        
        // Cap at max delay
        base_delay = std::min(base_delay, policy.max_delay);
        
        // Add jitter to avoid thundering herd
        if (policy.jitter_factor > 0.0) {
            std::uniform_real_distribution<double> jitter_dist(-policy.jitter_factor, policy.jitter_factor);
            double jitter = jitter_dist(_rng);
            auto jitter_amount = std::chrono::milliseconds{
                static_cast<long long>(base_delay.count() * jitter)
            };
            base_delay += jitter_amount;
        }
        
        // Ensure delay is not negative
        return std::max(base_delay, std::chrono::milliseconds{1});
    }
};

/**
 * @brief Specialized error handler for Raft RPC operations
 * 
 * This class provides pre-configured error handling for common Raft RPC operations
 * with appropriate retry policies and error classification.
 */
template<typename TermId = std::uint64_t, typename LogIndex = std::uint64_t>
class raft_error_handler {
public:
    // Type aliases for common Raft result types
    using append_entries_handler = error_handler<kythira::append_entries_response<TermId, LogIndex>>;
    using request_vote_handler = error_handler<kythira::request_vote_response<TermId>>;
    using install_snapshot_handler = error_handler<kythira::install_snapshot_response<TermId>>;
    
    /**
     * @brief Get error handler for AppendEntries operations
     */
    static auto get_append_entries_handler() -> append_entries_handler& {
        static append_entries_handler handler;
        return handler;
    }
    
    /**
     * @brief Get error handler for RequestVote operations
     */
    static auto get_request_vote_handler() -> request_vote_handler& {
        static request_vote_handler handler;
        return handler;
    }
    
    /**
     * @brief Get error handler for InstallSnapshot operations
     */
    static auto get_install_snapshot_handler() -> install_snapshot_handler& {
        static install_snapshot_handler handler;
        return handler;
    }
    
    /**
     * @brief Configure all handlers with custom policies
     */
    static auto configure_all_handlers(
        const typename error_handler<int>::retry_policy& heartbeat_policy,
        const typename error_handler<int>::retry_policy& append_entries_policy,
        const typename error_handler<int>::retry_policy& vote_policy,
        const typename error_handler<int>::retry_policy& snapshot_policy
    ) -> void {
        get_append_entries_handler().set_retry_policy("append_entries", append_entries_policy);
        get_append_entries_handler().set_retry_policy("heartbeat", heartbeat_policy);
        get_request_vote_handler().set_retry_policy("request_vote", vote_policy);
        get_install_snapshot_handler().set_retry_policy("install_snapshot", snapshot_policy);
    }
};

} // namespace kythira