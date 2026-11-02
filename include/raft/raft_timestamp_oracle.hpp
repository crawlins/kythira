// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file raft_timestamp_oracle.hpp
/// @brief The timestamp oracle of `.kiro/specs/multi-raft-transactions/`
///        tier 2 (task 10, Requirement 7).
///
/// Snapshot isolation needs timestamps every shard can compare. A shard's
/// log index cannot serve, because each group's log is independent and a
/// split or merge starts fresh logs. The oracle is a counter whose
/// high-water mark is replicated by one dedicated Raft group: the shape of
/// TiKV's PD timestamp oracle, with no dependence on wall clocks.
///
/// That group is an ordinary group of the host, wrapping the same
/// `transactional_state_machine` every shard does; only its
/// `oracle_reserve` entries and `_oracle_hwm` field mean anything there. The
/// host keeps it out of routing, split, merge and placement
/// (`multi_raft_config::oracle_group`).
///
/// **Serving.** The oracle group's leader keeps a window `[next, end)` in
/// memory. Every value below the replicated high-water mark may already have
/// been served; nothing at or above it has. A batch of allocations:
///
/// 1. confirms leadership with one read-index round (a heartbeat to a
///    quorum), so a deposed leader serving from a stale window after its
///    successor has served larger values fails instead (Requirement 7.5);
/// 2. if the window is short, proposes `oracle_reserve(end + max(window,
///    total))` and waits for it to apply (Requirement 7.3);
/// 3. hands out consecutive ranges.
///
/// A leader that is new to its term starts its window at the applied
/// high-water mark, read after step 1, which has applied every entry an
/// earlier leader committed. Every value an earlier leader served is below
/// that mark, so no value is ever served twice, across leader changes and
/// restarts alike (Requirement 7.4).
///
/// **Batching.** One batch runs at a time. Calls that arrive while it runs
/// queue, and the next caller to find the oracle idle runs all of them as
/// the next batch, so concurrent callers share one heartbeat round and at
/// most one reserve.
///
/// **Blocking.** `allocate()` blocks the calling thread for its batch and
/// returns a ready future, as the Redis gateway's transaction client does.
/// The coordinator calls it once per transaction, at `begin()`.

#include <raft/future_default.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/txn_envelope.hpp>

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace kythira {

/// @brief `_count` consecutive timestamps starting at `_first`.
struct timestamp_range {
    std::uint64_t _first{0};
    std::uint64_t _count{0};

    /// One past the last timestamp of the range.
    [[nodiscard]] auto end() const -> std::uint64_t { return _first + _count; }

    [[nodiscard]] auto operator==(const timestamp_range&) const -> bool = default;
};

/// @brief A source of cluster-wide, strictly increasing timestamps
///        (Requirement 7.1).
///
/// `allocate(n)` yields `n` values no other call has been or will be given,
/// each larger than every value a call that completed before this one
/// started was given. Zero is never a timestamp.
template<typename O>
concept timestamp_oracle = requires(O& o, std::size_t n) {
    { o.allocate(n).get() } -> std::convertible_to<timestamp_range>;
};

/// @brief The oracle could not serve: it is unconfigured, its group is not
///        hosted here, or a reserve did not apply.
class timestamp_oracle_unavailable_exception : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// @brief `raft_timestamp_oracle`'s counters, for tests and operators.
struct timestamp_oracle_counts {
    /// Batches run (each one heartbeat round).
    std::uint64_t _batches{0};
    /// Allocations served.
    std::uint64_t _allocations{0};
    /// `oracle_reserve` entries this oracle proposed and saw apply.
    std::uint64_t _reserves{0};
};

/// @brief A deployment's own oracle, behind the
///        `multi_raft_config::allocate_timestamps` hook (Requirement 7.6).
///
/// The hook is synchronous and throws on failure, as `allocate_shard_ids`
/// is; this adapts it to the concept.
class hook_timestamp_oracle {
public:
    using hook_type = std::function<timestamp_range(std::size_t)>;

    explicit hook_timestamp_oracle(hook_type hook) : _hook(std::move(hook)) {}

    auto allocate(std::size_t n) -> future_default<timestamp_range> {
        promise_default<timestamp_range> p;
        auto f = p.getFuture();
        try {
            if (n == 0) {
                throw std::invalid_argument("timestamp oracle: allocate(0)");
            }
            if (!_hook) {
                throw timestamp_oracle_unavailable_exception(
                    "timestamp oracle: no allocate_timestamps hook");
            }
            auto r = _hook(n);
            if (r._count < n || r._first == 0) {
                throw timestamp_oracle_unavailable_exception(
                    "timestamp oracle: the hook returned " + std::to_string(r._count) +
                    " values from " + std::to_string(r._first) + " for a request of " +
                    std::to_string(n));
            }
            r._count = n;
            p.setValue(r);
        } catch (...) {
            p.setException(std::current_exception());
        }
        return f;
    }

private:
    hook_type _hook;
};

/// @brief The in-host oracle: a counter replicated by `Host`'s oracle group.
///
/// @tparam Host A `multi_raft` whose state machine is a
///         `transactional_state_machine`. Only `group_node()` and the
///         node's `is_leader`, `get_current_term`, `read_state`,
///         `submit_command` and `with_state_machine` are used.
///
/// Serves only on the host whose replica leads the oracle group; elsewhere
/// `allocate` fails with `shard_not_leader_exception` carrying the leader
/// hint, as a shard does.
template<typename Host> class raft_timestamp_oracle {
public:
    using group_id_type = typename Host::group_id_type;
    using node_id_type = typename Host::node_id_type;
    using key_type = typename Host::key_type;
    using counts = timestamp_oracle_counts;

    /// The window a leader reserves at a time (Requirement 7.3).
    static constexpr std::uint64_t k_default_window = 10'000;

    raft_timestamp_oracle(Host& host, group_id_type group, std::uint64_t window = k_default_window,
                          std::chrono::milliseconds timeout = std::chrono::seconds{5})
        : _host(host),
          _group(std::move(group)),
          _window(std::max<std::uint64_t>(window, 1)),
          _timeout(timeout) {}

    raft_timestamp_oracle(const raft_timestamp_oracle&) = delete;
    auto operator=(const raft_timestamp_oracle&) -> raft_timestamp_oracle& = delete;

    [[nodiscard]] auto group() const -> const group_id_type& { return _group; }

    /// @brief `n` consecutive timestamps, or the reason there are none.
    auto allocate(std::size_t n) -> future_default<timestamp_range> {
        return allocate(n, _timeout);
    }

    auto allocate(std::size_t n, std::chrono::milliseconds timeout)
        -> future_default<timestamp_range> {
        promise_default<timestamp_range> p;
        auto f = p.getFuture();
        try {
            p.setValue(allocate_blocking(n, timeout));
        } catch (...) {
            p.setException(std::current_exception());
        }
        return f;
    }

    [[nodiscard]] auto stats() const -> counts {
        std::lock_guard lock(_mutex);
        return _stats;
    }

private:
    struct request {
        std::uint64_t _n{0};
        bool _done{false};
        timestamp_range _range{};
        std::exception_ptr _error;
    };

    auto allocate_blocking(std::size_t n, std::chrono::milliseconds timeout) -> timestamp_range {
        if (n == 0) {
            throw std::invalid_argument("timestamp oracle: allocate(0)");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        auto req = std::make_shared<request>();
        req->_n = n;

        std::unique_lock lock(_mutex);
        _queue.push_back(req);
        while (!req->_done) {
            if (!_busy) {
                // Run everything queued so far as one batch, ours included.
                _busy = true;
                std::vector<std::shared_ptr<request>> batch(_queue.begin(), _queue.end());
                _queue.clear();
                lock.unlock();
                run_batch(batch, timeout);
                lock.lock();
                _busy = false;
                _cv.notify_all();
                continue;
            }
            auto it = std::find(_queue.begin(), _queue.end(), req);
            if (it == _queue.end()) {
                // Taken into the running batch, which is bounded by its own
                // timeout: wait for it to finish.
                _cv.wait(lock);
                continue;
            }
            if (_cv.wait_until(lock, deadline) == std::cv_status::timeout && !req->_done) {
                // Still queued behind a batch that outlived our deadline.
                it = std::find(_queue.begin(), _queue.end(), req);
                if (it != _queue.end()) {
                    _queue.erase(it);
                    throw timestamp_oracle_unavailable_exception(
                        "timestamp oracle: timed out waiting for a batch");
                }
            }
        }
        if (req->_error) {
            std::rethrow_exception(req->_error);
        }
        return req->_range;
    }

    /// Runs one batch. Called by exactly one thread at a time (`_busy`), so
    /// the window and term need no lock of their own.
    auto run_batch(const std::vector<std::shared_ptr<request>>& batch,
                   std::chrono::milliseconds timeout) -> void {
        std::uint64_t total = 0;
        for (const auto& r : batch) {
            total += r->_n;
        }
        std::exception_ptr error;
        std::vector<timestamp_range> ranges;
        bool reserved = false;
        try {
            reserved = serve(total, timeout);
            ranges.reserve(batch.size());
            for (const auto& r : batch) {
                ranges.push_back(timestamp_range{._first = _next, ._count = r->_n});
                _next += r->_n;
            }
        } catch (...) {
            error = std::current_exception();
        }

        std::lock_guard lock(_mutex);
        ++_stats._batches;
        if (reserved) {
            ++_stats._reserves;
        }
        for (std::size_t i = 0; i < batch.size(); ++i) {
            batch[i]->_done = true;
            if (error) {
                batch[i]->_error = error;
            } else {
                batch[i]->_range = ranges[i];
                ++_stats._allocations;
            }
        }
    }

    /// Steps 1 and 2: on return, `[_next, _next + total)` is inside the
    /// window and this replica led the oracle group in `_term` throughout.
    /// Returns whether it reserved.
    auto serve(std::uint64_t total, std::chrono::milliseconds timeout) -> bool {
        auto* node = _host.group_node(_group);
        if (node == nullptr) {
            throw timestamp_oracle_unavailable_exception(
                "timestamp oracle: this host has no replica of the oracle group");
        }
        auto not_leader = [&] {
            return shard_not_leader_exception<group_id_type, node_id_type>(_group,
                                                                           node->known_leader());
        };
        if (!node->is_leader()) {
            throw not_leader();
        }
        const auto term = node->get_current_term();
        if (!_term.has_value() || *_term != term) {
            // New to this term: the old window may overlap what a leader of
            // a term in between served.
            _term = term;
            _next = 0;
            _end = 0;
        }

        // 1. One heartbeat round. Its read index includes this term's first
        //    entry, so on success every entry an earlier leader committed,
        //    every reserve included, has been applied here.
        (void)node->read_state(timeout).get();
        if (!node->is_leader() || node->get_current_term() != term) {
            throw not_leader();
        }
        if (_end == 0) {
            const std::uint64_t hwm =
                node->with_state_machine([](auto& sm) { return sm.oracle_high_water_mark(); });
            _next = std::max<std::uint64_t>(hwm, 1);
            _end = _next;
        }
        if (total > std::numeric_limits<std::uint64_t>::max() - _next) {
            throw timestamp_oracle_unavailable_exception("timestamp oracle: exhausted");
        }
        if (_next + total <= _end) {
            return false;
        }

        // 2. Reserve. Committing the entry in this term is itself proof of
        //    leadership when it commits.
        // Looked up here, not as a class alias: a host whose state machine
        // is not transactional names this class without instantiating it.
        using codec_type = typename Host::state_machine_type::key_codec_type;
        const auto new_hwm = _end + std::max(_window, total);
        const auto envelope = encode_txn_envelope<key_type, codec_type>(
            txn_envelope<key_type>{txn_oracle_reserve{new_hwm}});
        auto bytes = node->submit_command(envelope, timeout).get();
        auto res = decode_txn_result<key_type, codec_type>(bytes);
        if (res._status != txn_result_status::ok) {
            throw timestamp_oracle_unavailable_exception(
                "timestamp oracle: oracle_reserve did not apply");
        }
        if (!node->is_leader() || node->get_current_term() != term) {
            // It applied, so the mark is raised and nothing is lost, but
            // a successor may already be serving above it.
            _term.reset();
            throw not_leader();
        }
        _end = new_hwm;
        return true;
    }

    Host& _host;
    group_id_type _group;
    std::uint64_t _window;
    std::chrono::milliseconds _timeout;

    mutable std::mutex _mutex;
    std::condition_variable _cv;
    std::deque<std::shared_ptr<request>> _queue;
    bool _busy{false};
    counts _stats;

    // Owned by the thread running the batch.
    std::optional<std::uint64_t> _term;
    std::uint64_t _next{0};
    std::uint64_t _end{0};
};

}  // namespace kythira
