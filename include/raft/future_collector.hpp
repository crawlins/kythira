#pragma once

#include "future.hpp"
#include "../raft/completion_exceptions.hpp"
#include <vector>
#include <chrono>
#include <stdexcept>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

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
template<typename T> class raft_future_collector {
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
    static auto collect_majority(std::vector<kythira::Future<T>> futures,
                                 std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<T>> {
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
                    // Note: We ignore futures that failed with exceptions (timeouts, network
                    // errors) These are different from "failed responses" which are valid responses
                    // with success=false
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
                        return std::vector<T>{};  // Empty vector indicates success for void
                    }
                    throw kythira::future_collection_exception("collect_majority", failed_count);
                } else {
                    if (completed_results.size() >= majority_count) {
                        return completed_results;
                    }
                    throw kythira::future_collection_exception("collect_majority", failed_count);
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
    static auto collect_all_with_timeout(std::vector<kythira::Future<T>> futures,
                                         std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<kythira::Try<T>>> {
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
     * @brief Collect futures, resolving as soon as `required_successes` have
     * succeeded — unlike collect_all_with_timeout/collect_majority (both of
     * which internally wait for every future to individually settle before
     * checking whether enough succeeded, via collectAll), this resolves the
     * instant enough successes are in, without waiting on the rest.
     *
     * This matters when one or more of the input futures can legitimately
     * take a very long time to settle (e.g. a TCP connect attempt against a
     * network-partitioned peer, which can take up to its own per-future
     * timeout to fail even after a network drop): collect_all_with_timeout
     * would make every caller wait for that slow future's full timeout even
     * though a quorum of the OTHER futures already succeeded. This is
     * exactly what made node<Types>::read_state() (raft.hpp) take up to its
     * full command timeout for every linearizable read while any single
     * follower was partitioned, even though only a majority of followers
     * were ever required to ack.
     *
     * Resolves early with the successful results once `required_successes`
     * is reached; fails early once reaching `required_successes` becomes
     * mathematically impossible (too many of the remaining futures have
     * already failed); otherwise resolves once every future has settled
     * within `timeout`, whichever of the three happens first. The returned
     * vector's order reflects completion order, not input order (unlike
     * collect_all_with_timeout) — callers that need a specific successful
     * response should identify it from the value itself (e.g. a response's
     * own follower_id field), not by index.
     *
     * @param futures Vector of futures to collect from
     * @param required_successes Number of successful results needed
     * @param timeout Per-future timeout (via Future::within)
     * @return Future containing the first `required_successes` successful
     * results, or a future_collection_exception if that count becomes
     * unreachable
     */
    static auto collect_n_successes_with_timeout(std::vector<kythira::Future<T>> futures,
                                                 std::size_t required_successes,
                                                 std::chrono::milliseconds timeout)
        -> kythira::Future<std::vector<T>> {
        if (required_successes == 0) {
            return kythira::FutureFactory::makeFuture(std::vector<T>{});
        }
        if (futures.empty() || required_successes > futures.size()) {
            return kythira::FutureFactory::makeExceptionalFuture<std::vector<T>>(
                kythira::future_collection_exception("collect_n_successes_with_timeout", 0));
        }

        struct shared_state {
            std::mutex mu;
            std::vector<T> successes;
            std::size_t remaining;
            bool settled{false};
            kythira::Promise<std::vector<T>> promise;
        };
        auto state = std::make_shared<shared_state>();
        state->remaining = futures.size();
        auto result_future = state->promise.getFuture();

        for (auto& f : futures) {
            std::move(f).within(timeout).thenTry([state,
                                                  required_successes](kythira::Try<T> result) {
                bool fulfill_success = false;
                bool fulfill_failure = false;
                std::vector<T> successes_copy;
                {
                    std::lock_guard<std::mutex> lock(state->mu);
                    if (state->settled) {
                        return;
                    }
                    state->remaining--;
                    if (result.hasValue()) {
                        state->successes.push_back(std::move(result).value());
                    }
                    if (state->successes.size() >= required_successes) {
                        state->settled = true;
                        fulfill_success = true;
                        successes_copy = state->successes;
                    } else if (state->successes.size() + state->remaining < required_successes) {
                        state->settled = true;
                        fulfill_failure = true;
                    }
                }
                // setValue/setException run outside the lock - state->mu
                // only needs to protect the shared counters/vector above,
                // and `settled` (checked-then-set under the lock before
                // either branch runs) guarantees exactly one of these
                // fires across every future's callback.
                if (fulfill_success) {
                    state->promise.setValue(std::move(successes_copy));
                } else if (fulfill_failure) {
                    state->promise.setException(
                        std::make_exception_ptr(kythira::future_collection_exception(
                            "collect_n_successes_with_timeout", 0)));
                }
            });
        }
        return result_future;
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
    static auto collect_any_with_timeout(std::vector<kythira::Future<T>> futures,
                                         std::chrono::milliseconds timeout)
        -> kythira::Future<std::tuple<std::size_t, T>> {
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
                .thenValue([](std::size_t index) { return std::make_tuple(index, T{}); });
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
    enum class collection_strategy : std::uint8_t {
        all,       // Wait for all futures
        majority,  // Wait for majority of futures
        any,       // Wait for any single future
        count      // Wait for specific count of futures
    };

    static auto collect_with_strategy(std::vector<kythira::Future<T>> futures,
                                      collection_strategy strategy,
                                      std::chrono::milliseconds timeout, std::size_t count = 0)
        -> kythira::Future<std::vector<T>> {
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

}  // namespace kythira