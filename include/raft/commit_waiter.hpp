#pragma once

#include "future.hpp"
#include "types.hpp"
#include "completion_exceptions.hpp"
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <optional>
#include <string>

namespace kythira {

/**
 * @brief CommitWaiter manages pending client operations waiting for commit and state machine application
 * 
 * This class tracks client operations that are waiting for their log entries to be committed
 * (replicated to majority) and applied to the state machine. It provides timeout handling
 * and cancellation support.
 * 
 * @tparam LogIndex The log index type (must satisfy log_index concept)
 */
template<typename LogIndex = std::uint64_t>
requires kythira::log_index<LogIndex>
class commit_waiter {
public:
    using log_index_t = LogIndex;
    
private:
    /**
     * @brief Structure to track a pending client operation
     */
    struct pending_operation {
        log_index_t entry_index;
        std::function<void(std::vector<std::byte>)> fulfill_callback;
        std::function<void(std::exception_ptr)> reject_callback;
        std::chrono::steady_clock::time_point submitted_at;
        std::optional<std::chrono::milliseconds> timeout;
        
        pending_operation(
            log_index_t index,
            std::function<void(std::vector<std::byte>)> fulfill,
            std::function<void(std::exception_ptr)> reject,
            std::chrono::steady_clock::time_point submitted,
            std::optional<std::chrono::milliseconds> timeout_duration
        ) : entry_index(index)
          , fulfill_callback(std::move(fulfill))
          , reject_callback(std::move(reject))
          , submitted_at(submitted)
          , timeout(timeout_duration) {}
        
        // Move-only semantics
        pending_operation(const pending_operation&) = delete;
        pending_operation& operator=(const pending_operation&) = delete;
        pending_operation(pending_operation&&) = default;
        pending_operation& operator=(pending_operation&&) = default;
        
        /**
         * @brief Check if this operation has timed out
         */
        auto is_timed_out() const -> bool {
            if (!timeout.has_value()) {
                return false;
            }
            auto elapsed = std::chrono::steady_clock::now() - submitted_at;
            return elapsed > timeout.value();
        }
    };
    
    // Map from log index to vector of pending operations for that index
    std::unordered_map<log_index_t, std::vector<pending_operation>> _pending_operations;
    
    // Mutex for protecting shared state
    mutable std::mutex _mutex;
    
public:
    /**
     * @brief Default constructor
     */
    commit_waiter() = default;
    
    // Move-only semantics (promises are move-only)
    commit_waiter(const commit_waiter&) = delete;
    commit_waiter& operator=(const commit_waiter&) = delete;
    commit_waiter(commit_waiter&&) = default;
    commit_waiter& operator=(commit_waiter&&) = default;
    
    /**
     * @brief Register a new operation that waits for commit and state machine application
     * 
     * @param index The log index of the entry to wait for
     * @param fulfill_callback Callback to call when the operation is fulfilled
     * @param reject_callback Callback to call when the operation is rejected
     * @param timeout Optional timeout duration (nullopt means no timeout)
     */
    auto register_operation(
        log_index_t index,
        std::function<void(std::vector<std::byte>)> fulfill_callback,
        std::function<void(std::exception_ptr)> reject_callback,
        std::optional<std::chrono::milliseconds> timeout = std::nullopt
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Create pending operation
        pending_operation op(
            index,
            std::move(fulfill_callback),
            std::move(reject_callback),
            std::chrono::steady_clock::now(),
            timeout
        );
        
        // Add to pending operations map
        _pending_operations[index].push_back(std::move(op));
    }
    
    /**
     * @brief Notify that entries up to commit_index are committed and applied to state machine
     * 
     * This method fulfills all pending operations for entries with index <= commit_index.
     * The state machine results should be provided for each fulfilled operation.
     * 
     * @param commit_index The highest log index that has been committed and applied
     * @param get_result Function to get the state machine result for a given log index
     */
    template<typename ResultFunction>
    auto notify_committed_and_applied(
        log_index_t commit_index,
        ResultFunction&& get_result
    ) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Find all operations that can be fulfilled
        auto it = _pending_operations.begin();
        while (it != _pending_operations.end()) {
            if (it->first <= commit_index) {
                // Fulfill all operations for this index
                for (auto& op : it->second) {
                    try {
                        auto result = get_result(it->first);
                        op.fulfill_callback(std::move(result));
                    } catch (...) {
                        op.reject_callback(std::current_exception());
                    }
                }
                
                // Remove fulfilled operations
                it = _pending_operations.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    /**
     * @brief Notify that entries up to commit_index are committed and applied (simple version)
     * 
     * This version assumes all operations return empty results.
     * 
     * @param commit_index The highest log index that has been committed and applied
     */
    auto notify_committed_and_applied(log_index_t commit_index) -> void {
        notify_committed_and_applied(commit_index, [](log_index_t) {
            return std::vector<std::byte>{};
        });
    }
    
    /**
     * @brief Cancel all pending operations with the given reason
     * 
     * This is typically called when leadership is lost or the node shuts down.
     * 
     * @param reason The reason for cancellation (used in exception message)
     */
    auto cancel_all_operations(const std::string& reason) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Use base completion exception for general cancellations
        auto exception = std::make_exception_ptr(
            raft_completion_exception("Operation cancelled: " + reason)
        );
        
        for (auto& [index, operations] : _pending_operations) {
            for (auto& op : operations) {
                op.reject_callback(exception);
            }
        }
        
        _pending_operations.clear();
    }
    
    /**
     * @brief Cancel all pending operations due to leadership loss
     * 
     * This specialized method uses the leadership_lost_exception with term information.
     * 
     * @tparam TermId The term ID type
     * @param old_term The previous term when this node was leader
     * @param new_term The new term that caused leadership loss
     */
    template<typename TermId>
    requires kythira::term_id<TermId>
    auto cancel_all_operations_leadership_lost(TermId old_term, TermId new_term) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        
        // Use specific leadership lost exception with term information
        auto exception = std::make_exception_ptr(
            leadership_lost_exception<TermId>(old_term, new_term)
        );
        
        for (auto& [index, operations] : _pending_operations) {
            for (auto& op : operations) {
                op.reject_callback(exception);
            }
        }
        
        _pending_operations.clear();
    }
    
    /**
     * @brief Cancel operations that have timed out
     * 
     * This should be called periodically to clean up timed-out operations.
     * 
     * @return Number of operations that were cancelled due to timeout
     */
    auto cancel_timed_out_operations() -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        
        std::size_t cancelled_count = 0;
        
        auto it = _pending_operations.begin();
        while (it != _pending_operations.end()) {
            auto& operations = it->second;
            
            // Remove timed-out operations
            auto op_it = operations.begin();
            while (op_it != operations.end()) {
                if (op_it->is_timed_out()) {
                    // Use specific timeout exception with entry index and timeout details
                    auto timeout_exception = std::make_exception_ptr(
                        commit_timeout_exception<log_index_t>(
                            op_it->entry_index, 
                            op_it->timeout.value_or(std::chrono::milliseconds{0})
                        )
                    );
                    op_it->reject_callback(timeout_exception);
                    op_it = operations.erase(op_it);
                    ++cancelled_count;
                } else {
                    ++op_it;
                }
            }
            
            // Remove empty operation vectors
            if (operations.empty()) {
                it = _pending_operations.erase(it);
            } else {
                ++it;
            }
        }
        
        return cancelled_count;
    }
    
    /**
     * @brief Cancel operations for a specific log index
     * 
     * @param index The log index to cancel operations for
     * @param reason The reason for cancellation
     * @return Number of operations cancelled
     */
    auto cancel_operations_for_index(log_index_t index, const std::string& reason) -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        
        auto it = _pending_operations.find(index);
        if (it == _pending_operations.end()) {
            return 0;
        }
        
        auto exception = std::make_exception_ptr(
            raft_completion_exception("Operation cancelled: " + reason)
        );
        
        std::size_t cancelled_count = it->second.size();
        for (auto& op : it->second) {
            op.reject_callback(exception);
        }
        
        _pending_operations.erase(it);
        return cancelled_count;
    }
    
    /**
     * @brief Cancel operations for all indices after the specified index
     * 
     * This is useful when state machine application fails and we need to cancel
     * all operations that were waiting for entries that couldn't be applied.
     * 
     * @param after_index Cancel operations for indices > after_index
     * @param reason The reason for cancellation
     * @return Number of operations cancelled
     */
    auto cancel_operations_after_index(log_index_t after_index, const std::string& reason) -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        
        auto exception = std::make_exception_ptr(
            raft_completion_exception("Operation cancelled: " + reason)
        );
        
        std::size_t cancelled_count = 0;
        auto it = _pending_operations.begin();
        while (it != _pending_operations.end()) {
            if (it->first > after_index) {
                // Cancel all operations for this index
                for (auto& op : it->second) {
                    op.reject_callback(exception);
                    ++cancelled_count;
                }
                
                // Remove this index from pending operations
                it = _pending_operations.erase(it);
            } else {
                ++it;
            }
        }
        
        return cancelled_count;
    }
    
    /**
     * @brief Get the number of pending operations
     * 
     * @return Total number of pending operations across all indices
     */
    auto get_pending_count() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        
        std::size_t total = 0;
        for (const auto& [index, operations] : _pending_operations) {
            total += operations.size();
        }
        return total;
    }
    
    /**
     * @brief Get the number of pending operations for a specific index
     * 
     * @param index The log index to check
     * @return Number of pending operations for the given index
     */
    auto get_pending_count_for_index(log_index_t index) const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        
        auto it = _pending_operations.find(index);
        if (it == _pending_operations.end()) {
            return 0;
        }
        return it->second.size();
    }
    
    /**
     * @brief Check if there are any pending operations
     * 
     * @return true if there are pending operations, false otherwise
     */
    auto has_pending_operations() const -> bool {
        std::lock_guard<std::mutex> lock(_mutex);
        return !_pending_operations.empty();
    }
};

} // namespace kythira