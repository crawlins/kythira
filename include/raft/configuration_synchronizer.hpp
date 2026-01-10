#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <mutex>
#include "future.hpp"
#include "types.hpp"
#include "completion_exceptions.hpp"

namespace kythira {

/**
 * @brief Configuration synchronizer for managing safe configuration changes
 * 
 * This class implements the two-phase configuration change protocol required
 * by the Raft consensus algorithm. It ensures that configuration changes are
 * properly synchronized and committed before proceeding to the next phase.
 * 
 * The synchronizer manages the transition from:
 * 1. Current configuration (C_old) 
 * 2. Joint consensus configuration (C_old,new)
 * 3. Final configuration (C_new)
 * 
 * Each phase must be committed before proceeding to the next phase to maintain
 * Raft safety properties.
 */
template<typename NodeId = std::uint64_t, typename LogIndex = std::uint64_t, typename FutureType = kythira::Future<bool>>
requires kythira::node_id<NodeId> && kythira::log_index<LogIndex>
class configuration_synchronizer {
public:
    using future_type = FutureType;
    using promise_type = kythira::Promise<bool>;
    using node_id_t = NodeId;
    using log_index_t = LogIndex;
    
private:
    enum class config_change_phase {
        none,                    // No configuration change in progress
        joint_consensus,         // Waiting for joint consensus to be committed
        final_configuration      // Waiting for final configuration to be committed
    };
    
    // Current phase of configuration change
    config_change_phase _current_phase{config_change_phase::none};
    
    // Target configuration we're transitioning to
    std::optional<cluster_configuration<NodeId>> _target_configuration;
    
    // Promise to fulfill when configuration change completes
    std::optional<promise_type> _change_promise;
    
    // Log index of the joint consensus configuration entry
    std::optional<LogIndex> _joint_config_index;
    
    // Log index of the final configuration entry
    std::optional<LogIndex> _final_config_index;
    
    // Timeout for the configuration change operation
    std::chrono::steady_clock::time_point _change_started_at;
    std::chrono::milliseconds _change_timeout{std::chrono::seconds(60)};
    
    // Mutex for thread safety
    mutable std::mutex _mutex;
    
public:
    /**
     * @brief Start a configuration change with proper synchronization
     * 
     * @param new_config The target configuration to transition to
     * @param timeout Maximum time to wait for the configuration change
     * @return Future that completes when the configuration change is done
     */
    auto start_configuration_change(
        const cluster_configuration<NodeId>& new_config,
        std::chrono::milliseconds timeout = std::chrono::seconds(60)
    ) -> future_type {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_current_phase != config_change_phase::none) {
            return kythira::FutureFactory::makeExceptionalFuture<bool>(
                std::make_exception_ptr(
                    configuration_change_exception("start", "Configuration change already in progress")
                ));
        }
        
        _target_configuration = new_config;
        _current_phase = config_change_phase::joint_consensus;
        _change_timeout = timeout;
        _change_started_at = std::chrono::steady_clock::now();
        
        // Create promise for the configuration change completion
        promise_type promise;
        auto future = promise.getFuture();
        _change_promise = std::move(promise);
        
        return std::move(future);
    }
    
    /**
     * @brief Notify that a configuration entry has been committed
     * 
     * This method should be called when configuration entries are committed
     * to advance the configuration change through its phases.
     * 
     * @param config The configuration that was committed
     * @param committed_index The log index at which it was committed
     */
    auto notify_configuration_committed(
        const cluster_configuration<NodeId>& config,
        LogIndex committed_index
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_current_phase == config_change_phase::none) {
            // No configuration change in progress, ignore
            return;
        }
        
        if (_current_phase == config_change_phase::joint_consensus) {
            // Joint consensus configuration was committed
            if (config.is_joint_consensus()) {
                _joint_config_index = committed_index;
                _current_phase = config_change_phase::final_configuration;
                // Continue to final configuration phase
                // The caller should append the final configuration entry
            }
        } else if (_current_phase == config_change_phase::final_configuration) {
            // Final configuration was committed
            if (!config.is_joint_consensus() && 
                _target_configuration && 
                config.nodes() == _target_configuration->nodes()) {
                
                _final_config_index = committed_index;
                
                // Configuration change completed successfully
                if (_change_promise) {
                    _change_promise->setValue(true);
                }
                
                // Reset state
                reset_state();
            }
        }
    }
    
    /**
     * @brief Cancel ongoing configuration change
     * 
     * @param reason Reason for cancellation (for logging/debugging)
     */
    auto cancel_configuration_change(const std::string& reason) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_current_phase == config_change_phase::none) {
            return; // No change in progress
        }
        
        if (_change_promise) {
            std::string phase_name = (_current_phase == config_change_phase::joint_consensus) 
                ? "joint_consensus" : "final_configuration";
            _change_promise->setException(
                configuration_change_exception(phase_name, reason));
        }
        
        reset_state();
    }
    
    /**
     * @brief Check if a configuration change is in progress
     * 
     * @return true if a configuration change is currently in progress
     */
    auto is_configuration_change_in_progress() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current_phase != config_change_phase::none;
    }
    
    /**
     * @brief Get the current phase of configuration change
     * 
     * @return Current configuration change phase
     */
    auto get_current_phase() const -> config_change_phase {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current_phase;
    }
    
    /**
     * @brief Check if the configuration change has timed out
     * 
     * @return true if the current configuration change has exceeded its timeout
     */
    auto is_timed_out() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_current_phase == config_change_phase::none) {
            return false;
        }
        
        auto elapsed = std::chrono::steady_clock::now() - _change_started_at;
        return elapsed > _change_timeout;
    }
    
    /**
     * @brief Handle timeout for configuration change
     * 
     * Should be called periodically to check for and handle timeouts
     */
    auto handle_timeout() -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        if (_current_phase == config_change_phase::none) {
            return;
        }
        
        if (is_timed_out()) {
            if (_change_promise) {
                std::string phase_name = (_current_phase == config_change_phase::joint_consensus) 
                    ? "joint_consensus" : "final_configuration";
                _change_promise->setException(
                    configuration_change_exception(phase_name, "Configuration change timed out"));
            }
            
            reset_state();
        }
    }
    
    /**
     * @brief Get the target configuration being transitioned to
     * 
     * @return Optional target configuration, nullopt if no change in progress
     */
    auto get_target_configuration() const -> std::optional<cluster_configuration<NodeId>> {
        std::lock_guard<std::mutex> lock(_mutex);
        return _target_configuration;
    }
    
    /**
     * @brief Check if we're waiting for joint consensus to be committed
     * 
     * @return true if currently in joint consensus phase
     */
    auto is_waiting_for_joint_consensus() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current_phase == config_change_phase::joint_consensus;
    }
    
    /**
     * @brief Check if we're waiting for final configuration to be committed
     * 
     * @return true if currently in final configuration phase
     */
    auto is_waiting_for_final_configuration() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return _current_phase == config_change_phase::final_configuration;
    }

private:
    /**
     * @brief Reset internal state after configuration change completion/cancellation
     */
    auto reset_state() -> void {
        _current_phase = config_change_phase::none;
        _target_configuration.reset();
        _change_promise.reset();
        _joint_config_index.reset();
        _final_config_index.reset();
    }
};

} // namespace kythira