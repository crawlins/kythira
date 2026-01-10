#pragma once

#include "future.hpp"
#include "../raft/completion_exceptions.hpp"
#include <vector>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cstddef>

namespace kythira {

/**
 * @brief Raft future collection mechanism for async operation coordination
 * 
 * This class provides specialized future collection operations for Raft consensus,
 * including majority collection, timeout handling, and cancellation cleanup.
 * It uses the kythira::FutureCollector static methods for the underlying operations.
 * 
 * @tparam T The value type of the futures being collected
 */
template<typename T>
class raft_future_collector {
public:
    // Delete constructors to make this a static-only utility class
    raft_future_collector() = delete;
    raft_future_collector(const raft_future_collector&) = delete;
    raft_future_collector(raft_future_collector&&) = delete;
    raft_future_collector& operator=(const raft_future_collector&) = delete;
    raft_future_collector& operator=(raft_future_collector&&) = delete;
    
    /**
     * @brief Collect futures and wait for majority response
     * 
     * Waits for a majority of the provided futures to complete (successfully or with failure).
     * A majority is defined as (futures.size() / 2) + 1.
     * The caller is responsible for checking the success status of individual responses.
     * 
     * @param futures Vector of futures to collect from
     * @param timeout Maximum time to wait for majority response
     * @return Future containing vector of results from majority (may include failed responses)
     * @throws std::invalid_argument if futures vector is empty
     */
    static auto collect_majority(
        std::vector<kythira::Future<T>> futures,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<std::vector<T>> {
        if (futures.empty()) {
            return kythira::FutureFactory::makeExceptionalFuture<std::vector<T>>(
                kythira::future_collection_exception("collect_majority", 0));
        }
        
        const std::size_t majority_count = (futures.size() / 2) + 1;
        
        // Add timeout to all futures
        std::vector<kythira::Future<T>> timed_futures;
        timed_futures.reserve(futures.size());
        for (auto& future : futures) {
            timed_futures.push_back(std::move(future).within(timeout));
        }
        
        // Collect all futures and then check if we have majority
        return kythira::FutureCollector::collectAll(std::move(timed_futures))
            .thenValue([majority_count](std::vector<kythira::Try<T>> results) {
                std::vector<T> completed_results;
                completed_results.reserve(results.size());
                std::size_t failed_count = 0;
                
                // Extract all completed results (both successful and failed responses)
                for (auto& try_result : results) {
                    if (try_result.hasValue()) {
                        if constexpr (!std::is_void_v<T>) {
                            completed_results.push_back(std::move(try_result.value()));
                        }
                    } else {
                        failed_count++;
                    }
                    // Note: We ignore futures that failed with exceptions (timeouts, network errors)
                    // These are different from "failed responses" which are valid responses with success=false
                }
                
                if constexpr (std::is_void_v<T>) {
                    // For void futures, count successful completions
                    std::size_t completed_count = 0;
                    for (auto& try_result : results) {
                        if (try_result.hasValue()) {
                            completed_count++;
                        }
                    }
                    if (completed_count >= majority_count) {
                        return std::vector<T>{}; // Empty vector indicates success for void
                    } else {
                        throw kythira::future_collection_exception("collect_majority", failed_count);
                    }
                } else {
                    if (completed_results.size() >= majority_count) {
                        return completed_results;
                    } else {
                        throw kythira::future_collection_exception("collect_majority", failed_count);
                    }
                }
            });
    }
    
    /**
     * @brief Collect all futures with timeout handling
     * 
     * Waits for all futures to complete, handling timeouts gracefully.
     * Returns Try<T> results for each future, preserving order.
     * 
     * @param futures Vector of futures to collect
     * @param timeout Maximum time to wait for each future
     * @return Future containing vector of Try<T> results
     */
    static auto collect_all_with_timeout(
        std::vector<kythira::Future<T>> futures,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<std::vector<kythira::Try<T>>> {
        if (futures.empty()) {
            return kythira::FutureFactory::makeFuture(std::vector<kythira::Try<T>>{});
        }
        
        // Add timeout to all futures
        std::vector<kythira::Future<T>> timed_futures;
        timed_futures.reserve(futures.size());
        for (auto& future : futures) {
            timed_futures.push_back(std::move(future).within(timeout));
        }
        
        return kythira::FutureCollector::collectAll(std::move(timed_futures));
    }
    
    /**
     * @brief Collect any future with timeout
     * 
     * Returns the first future to complete successfully, with timeout handling.
     * 
     * @param futures Vector of futures to collect from
     * @param timeout Maximum time to wait for any future
     * @return Future containing tuple of index and result from first completed future
     */
    static auto collect_any_with_timeout(
        std::vector<kythira::Future<T>> futures,
        std::chrono::milliseconds timeout
    ) -> kythira::Future<std::tuple<std::size_t, T>> {
        if (futures.empty()) {
            return kythira::FutureFactory::makeExceptionalFuture<std::tuple<std::size_t, T>>(
                kythira::future_collection_exception("collect_any_with_timeout", 0));
        }
        
        // Add timeout to all futures
        std::vector<kythira::Future<T>> timed_futures;
        timed_futures.reserve(futures.size());
        for (auto& future : futures) {
            timed_futures.push_back(std::move(future).within(timeout));
        }
        
        if constexpr (std::is_void_v<T>) {
            return kythira::FutureCollector::collectAnyWithoutException(std::move(timed_futures))
                .thenValue([](std::size_t index) {
                    return std::make_tuple(index, T{});
                });
        } else {
            return kythira::FutureCollector::collectAnyWithoutException(std::move(timed_futures));
        }
    }
    
    /**
     * @brief Cancel all futures in a collection
     * 
     * Attempts to cancel all futures in the provided vector.
     * Note: Cancellation support depends on the underlying future implementation.
     * 
     * @param futures Vector of futures to cancel
     */
    static auto cancel_collection(std::vector<kythira::Future<T>>& futures) -> void {
        // Note: folly::Future doesn't have direct cancellation support
        // This method is provided for interface completeness and future extensibility
        // In practice, cancellation is typically handled by the underlying operations
        // (e.g., network timeouts, operation cancellation tokens)
        
        // Clear the futures vector to release resources
        futures.clear();
    }
    
    /**
     * @brief Collect futures with custom strategy
     * 
     * Collects futures using a custom strategy (all, majority, any, or specific count).
     * 
     * @param futures Vector of futures to collect
     * @param strategy Collection strategy
     * @param timeout Maximum time to wait
     * @param count Specific count for 'count' strategy (ignored for other strategies)
     * @return Future containing results based on the strategy
     */
    enum class collection_strategy {
        all,        // Wait for all futures
        majority,   // Wait for majority of futures
        any,        // Wait for any single future
        count       // Wait for specific count of futures
    };
    
    static auto collect_with_strategy(
        std::vector<kythira::Future<T>> futures,
        collection_strategy strategy,
        std::chrono::milliseconds timeout,
        std::size_t count = 0
    ) -> kythira::Future<std::vector<T>> {
        switch (strategy) {
            case collection_strategy::all:
                return collect_all_with_timeout(std::move(futures), timeout)
                    .thenValue([](std::vector<kythira::Try<T>> results) {
                        std::vector<T> successful_results;
                        for (auto& try_result : results) {
                            if (try_result.hasValue()) {
                                if constexpr (!std::is_void_v<T>) {
                                    successful_results.push_back(std::move(try_result.value()));
                                }
                            } else {
                                // If any future failed, propagate the first exception
                                std::rethrow_exception(try_result.exception());
                            }
                        }
                        return successful_results;
                    });
                    
            case collection_strategy::majority:
                return collect_majority(std::move(futures), timeout);
                
            case collection_strategy::any:
                return collect_any_with_timeout(std::move(futures), timeout)
                    .thenValue([](std::tuple<std::size_t, T> result) {
                        std::vector<T> results;
                        if constexpr (!std::is_void_v<T>) {
                            results.push_back(std::get<1>(result));
                        }
                        return results;
                    });
                    
            case collection_strategy::count: {
                if (count == 0 || count > futures.size()) {
                    return kythira::FutureFactory::makeExceptionalFuture<std::vector<T>>(
                        kythira::future_collection_exception("collect_with_strategy", 0));
                }
                
                // Add timeout to all futures
                std::vector<kythira::Future<T>> timed_futures;
                timed_futures.reserve(futures.size());
                for (auto& future : futures) {
                    timed_futures.push_back(std::move(future).within(timeout));
                }
                
                return kythira::FutureCollector::collectN(std::move(timed_futures), count)
                    .thenValue([](std::vector<std::tuple<std::size_t, kythira::Try<T>>> results) {
                        std::vector<T> successful_results;
                        for (auto& [index, try_result] : results) {
                            if (try_result.hasValue()) {
                                if constexpr (!std::is_void_v<T>) {
                                    successful_results.push_back(std::move(try_result.value()));
                                }
                            }
                        }
                        return successful_results;
                    });
            }
                    
            default:
                return kythira::FutureFactory::makeExceptionalFuture<std::vector<T>>(
                    kythira::future_collection_exception("collect_with_strategy", 0));
        }
    }
};

} // namespace kythira