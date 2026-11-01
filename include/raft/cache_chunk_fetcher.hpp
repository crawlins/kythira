// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file cache_chunk_fetcher.hpp
/// @brief Brings chunks a node references but does not hold from its peers
///        (.kiro/specs/disk-backed-build-cache/ design Component 5, task 5).
///
/// A replica learns of chunks it lacks from a snapshot restore, a merge, or a
/// `commit` whose pending chunks arrived in a snapshot without their bytes
/// (`cache_kv_options::_on_missing_chunks`). Until every chunk of a manifest
/// is here, the gateway treats that key as unservable on this node: a
/// follower forwards reads to the leader, and a leader answers a miss
/// (Requirement 4.4). The fetcher closes that gap:
///
/// - One queue per node, keyed by chunk, with every shard that wants each
///   chunk. A chunk two shards want is fetched once.
/// - For each chunk, the peers of the first shard that wants it are asked in
///   the order `peers` gives (the daemon puts the shard's leader first, then
///   its other members). The transport is a callback: in the daemon it is
///   `GET /_chunk/<sha>` on the HTTP front end with the internal secret
///   (task 6); tests read another node's store directly.
/// - Every fetched chunk is hashed before it is stored. A peer that sends the
///   wrong bytes is skipped like one that is down.
/// - It stores with `chunk_refcounts::put_if_referenced`, so a key deleted
///   while its chunk was in flight leaves no orphan behind.
/// - A token bucket caps the fetch rate (`KYTHIRA_CACHE_FETCH_BYTES_PER_SEC`,
///   default 50 MB/s), so a fresh replica catching up does not starve the
///   node's own traffic (Requirement 4.6).
/// - When a shard's last wanted chunk is done, `refresh` is called for that
///   shard so its manifests' `has_all` bits are recomputed, and while a long
///   fetch is running, at most once per `_refresh_interval`, so keys become
///   servable as their chunks land rather than all at the end.
/// - A chunk no peer could supply is retried with a doubling delay, capped
///   at `_max_retry_delay`. It is dropped once nothing references it.
///
/// Lock order: the state machine calls `want` under its node lock, so `want`
/// takes only the fetcher's own mutex, and `refresh` (which takes the node
/// lock) is never called with that mutex held.

#include <raft/cache_chunk_store.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace kythira {

struct cache_chunk_fetcher_options {
    /// Sustained fetch rate in bytes per second; 0 disables the limit.
    std::uint64_t _bytes_per_sec = 50'000'000;
    /// Bytes the bucket may hold when idle. A chunk larger than this is
    /// still fetched; the bucket goes into debt and the next fetch waits.
    std::uint64_t _burst_bytes = std::uint64_t{16} * 1024 * 1024;
    /// First delay before retrying a chunk no peer supplied; doubles per
    /// attempt up to `_max_retry_delay`.
    std::chrono::milliseconds _retry_delay{250};
    std::chrono::milliseconds _max_retry_delay{30'000};
    /// While a shard still has chunks outstanding, refresh its `has_all`
    /// bits at most this often.
    std::chrono::milliseconds _refresh_interval{1000};
    std::size_t _workers = 2;
};

struct cache_chunk_fetcher_stats {
    std::atomic<std::uint64_t> _fetched{0};
    std::atomic<std::uint64_t> _fetched_bytes{0};
    /// Wanted chunks that were already here when their turn came.
    std::atomic<std::uint64_t> _already_present{0};
    /// Wanted chunks dropped because nothing referenced them any more.
    std::atomic<std::uint64_t> _dropped_unreferenced{0};
    /// Peers that failed to answer, or had no copy.
    std::atomic<std::uint64_t> _peer_misses{0};
    /// Peers that answered with bytes that did not hash to the name.
    std::atomic<std::uint64_t> _verify_failures{0};
    /// Chunks put back on the queue because no peer supplied them.
    std::atomic<std::uint64_t> _retries{0};
    std::atomic<std::uint64_t> _refreshes{0};
};

/// A token bucket over bytes. Charged after the fact, because a chunk's size
/// is known only once it arrives; a fetch waits while the bucket is in debt.
class cache_token_bucket {
public:
    using clock = std::chrono::steady_clock;

    cache_token_bucket(std::uint64_t bytes_per_sec, std::uint64_t burst_bytes)
        : _rate(bytes_per_sec),
          _burst(static_cast<double>(burst_bytes)),
          _tokens(static_cast<double>(burst_bytes)),
          _last(clock::now()) {}

    /// How long to wait before the next fetch may start; zero if now.
    [[nodiscard]] auto wait_time() -> std::chrono::nanoseconds {
        if (_rate == 0) {
            return std::chrono::nanoseconds{0};
        }
        refill();
        if (_tokens >= 0) {
            return std::chrono::nanoseconds{0};
        }
        const double seconds = -_tokens / static_cast<double>(_rate);
        return std::chrono::nanoseconds{static_cast<std::int64_t>(seconds * 1e9) + 1};
    }

    void charge(std::uint64_t bytes) {
        if (_rate == 0) {
            return;
        }
        refill();
        _tokens -= static_cast<double>(bytes);
    }

private:
    void refill() {
        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - _last).count();
        _last = now;
        _tokens = std::min(_burst, _tokens + (elapsed * static_cast<double>(_rate)));
    }

    std::uint64_t _rate;
    double _burst;
    double _tokens;
    clock::time_point _last;
};

template<typename NodeId, typename GroupId = std::uint64_t> class cache_chunk_fetcher {
public:
    using clock = std::chrono::steady_clock;
    /// Members of `group` to ask, best first (its leader, then the rest).
    /// Never this node.
    using peers_fn = std::function<std::vector<NodeId>(const GroupId&)>;
    /// One peer's copy of a chunk, or `nullopt` if it has none or is down.
    using fetch_fn =
        std::function<std::optional<std::vector<std::byte>>(const NodeId&, const chunk_sha&)>;
    /// Recompute `has_all` for every manifest of `group` on this node.
    using refresh_fn = std::function<void(const GroupId&)>;

    cache_chunk_fetcher(std::shared_ptr<chunk_refcounts> refs, peers_fn peers, fetch_fn fetch,
                        refresh_fn refresh, cache_chunk_fetcher_options options = {})
        : _refs(std::move(refs)),
          _peers(std::move(peers)),
          _fetch(std::move(fetch)),
          _refresh(std::move(refresh)),
          _options(options),
          _bucket(options._bytes_per_sec, options._burst_bytes) {
        if (!_refs || !_peers || !_fetch || !_refresh) {
            throw std::invalid_argument("cache_chunk_fetcher: missing a collaborator");
        }
    }

    cache_chunk_fetcher(const cache_chunk_fetcher&) = delete;
    auto operator=(const cache_chunk_fetcher&) -> cache_chunk_fetcher& = delete;

    ~cache_chunk_fetcher() { stop(); }

    /// Queue `shas` for `group`. Cheap and non-blocking: safe to call under
    /// a node lock, which is where the state machine calls it from.
    void want(const GroupId& group, const std::vector<chunk_sha>& shas) {
        if (shas.empty()) {
            return;
        }
        {
            const std::lock_guard lock(_mu);
            const auto now = clock::now();
            for (const auto& sha : shas) {
                auto [it, inserted] = _wanted.try_emplace(sha);
                auto& groups = it->second._groups;
                if (std::find(groups.begin(), groups.end(), group) == groups.end()) {
                    groups.push_back(group);
                    ++_outstanding[group];
                }
                if (inserted) {
                    _queue.emplace(now, sha);
                }
            }
        }
        _cv.notify_all();
    }

    /// Replace the transport, for a daemon whose transport (the HTTP front
    /// end) is built after the state machines that queue chunks. Only
    /// before `start()`, or while stopped.
    void set_fetch(fetch_fn fetch) {
        const std::lock_guard lock(_mu);
        if (!_threads.empty()) {
            throw std::logic_error("cache_chunk_fetcher: set_fetch while running");
        }
        if (!fetch) {
            throw std::invalid_argument("cache_chunk_fetcher: missing a collaborator");
        }
        _fetch = std::move(fetch);
    }

    void start() {
        const std::lock_guard lock(_mu);
        if (!_threads.empty()) {
            return;
        }
        _stopping = false;
        const std::size_t n = std::max<std::size_t>(1, _options._workers);
        for (std::size_t i = 0; i < n; ++i) {
            _threads.emplace_back([this] { worker(); });
        }
    }

    /// Joins the workers. Chunks still queued stay queued; a later `start`
    /// resumes them.
    void stop() {
        {
            const std::lock_guard lock(_mu);
            _stopping = true;
        }
        _cv.notify_all();
        for (auto& t : _threads) {
            if (t.joinable()) {
                t.join();
            }
        }
        _threads.clear();
    }

    /// Process one chunk whose turn has come, on the calling thread. Returns
    /// false if none was ready. For tests that drive the fetcher by hand;
    /// ignores the token bucket.
    auto run_once() -> bool {
        std::unique_lock lock(_mu);
        auto it = _queue.begin();
        if (it == _queue.end() || it->first > clock::now() || _in_flight.contains(it->second)) {
            return false;
        }
        auto sha = it->second;
        _queue.erase(it);
        _in_flight.insert(sha);
        lock.unlock();
        process(sha);
        return true;
    }

    /// Block until nothing is queued or in flight, or `timeout` passes.
    auto wait_idle(std::chrono::milliseconds timeout) -> bool {
        std::unique_lock lock(_mu);
        return _idle_cv.wait_for(lock, timeout, [this] { return _wanted.empty(); });
    }

    [[nodiscard]] auto pending() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _wanted.size();
    }
    [[nodiscard]] auto outstanding(const GroupId& group) const -> std::size_t {
        const std::lock_guard lock(_mu);
        auto it = _outstanding.find(group);
        return it == _outstanding.end() ? 0 : it->second;
    }
    [[nodiscard]] auto stats() const noexcept -> const cache_chunk_fetcher_stats& { return _stats; }

private:
    struct wanted {
        std::vector<GroupId> _groups;
        std::uint32_t _attempts = 0;
    };

    void worker() {
        std::unique_lock lock(_mu);
        while (!_stopping) {
            auto it = std::find_if(_queue.begin(), _queue.end(), [this](const auto& e) {
                return !_in_flight.contains(e.second);
            });
            if (it == _queue.end()) {
                _cv.wait(lock);
                continue;
            }
            // A copy: waiting releases the lock, and another worker may
            // erase this entry meanwhile.
            const auto ready_at = it->first;
            if (ready_at > clock::now()) {
                _cv.wait_until(lock, ready_at);
                continue;
            }
            if (auto wait = _bucket.wait_time(); wait > std::chrono::nanoseconds{0}) {
                _cv.wait_for(lock, wait);
                continue;
            }
            auto sha = it->second;
            _queue.erase(it);
            _in_flight.insert(sha);
            lock.unlock();
            process(sha);
            lock.lock();
        }
    }

    /// Fetch, verify and store one chunk, then settle its bookkeeping.
    /// Called with `_mu` released and `sha` in `_in_flight`.
    void process(const chunk_sha& sha) {
        enum class outcome {
            stored,
            present,
            unreferenced,
            retry
        };
        auto result = outcome::retry;
        std::uint64_t charged = 0;

        if (_refs->count(sha) == 0) {
            result = outcome::unreferenced;
        } else if (_refs->store().contains(sha)) {
            result = outcome::present;
        } else {
            GroupId group{};
            {
                const std::lock_guard lock(_mu);
                group = _wanted.at(sha)._groups.front();
            }
            for (const auto& peer : _peers(group)) {
                std::optional<std::vector<std::byte>> bytes;
                try {
                    bytes = _fetch(peer, sha);
                } catch (...) {
                    bytes.reset();
                }
                if (!bytes) {
                    ++_stats._peer_misses;
                    continue;
                }
                charged += bytes->size();
                if (sha256_of(*bytes) != sha) {
                    ++_stats._verify_failures;
                    continue;
                }
                try {
                    result = _refs->put_if_referenced(sha, *bytes, true) ? outcome::stored
                                                                         : outcome::unreferenced;
                } catch (...) {
                    // Out of space, or an I/O error: retry later rather than
                    // ask another peer for bytes we cannot write either.
                    result = outcome::retry;
                    break;
                }
                if (result == outcome::stored) {
                    ++_stats._fetched;
                    _stats._fetched_bytes += bytes->size();
                }
                break;
            }
        }

        std::vector<GroupId> finished;
        std::vector<GroupId> to_refresh;
        {
            const std::lock_guard lock(_mu);
            _bucket.charge(charged);
            _in_flight.erase(sha);
            auto it = _wanted.find(sha);
            if (result == outcome::retry) {
                ++_stats._retries;
                auto& w = it->second;
                const auto shift = std::min<std::uint32_t>(w._attempts, 20);
                auto delay =
                    std::min(_options._max_retry_delay,
                             std::chrono::milliseconds{_options._retry_delay.count() << shift});
                ++w._attempts;
                _queue.emplace(clock::now() + delay, sha);
            } else {
                if (result == outcome::present) {
                    ++_stats._already_present;
                } else if (result == outcome::unreferenced) {
                    ++_stats._dropped_unreferenced;
                }
                const auto now = clock::now();
                for (const auto& g : it->second._groups) {
                    auto o = _outstanding.find(g);
                    if (--o->second == 0) {
                        _outstanding.erase(o);
                        _last_refresh.erase(g);
                        finished.push_back(g);
                    } else if (result == outcome::stored) {
                        auto& last = _last_refresh[g];
                        if (now - last >= _options._refresh_interval) {
                            last = now;
                            to_refresh.push_back(g);
                        }
                    }
                }
                _wanted.erase(it);
            }
        }
        for (const auto& g : finished) {
            refresh(g);
        }
        for (const auto& g : to_refresh) {
            refresh(g);
        }
        {
            const std::lock_guard lock(_mu);
            if (_wanted.empty()) {
                _idle_cv.notify_all();
            }
        }
        _cv.notify_all();
    }

    void refresh(const GroupId& g) {
        try {
            _refresh(g);
            ++_stats._refreshes;
        } catch (...) {
            // A shard that went away since it asked: nothing to refresh.
        }
    }

    std::shared_ptr<chunk_refcounts> _refs;
    peers_fn _peers;
    fetch_fn _fetch;
    refresh_fn _refresh;
    cache_chunk_fetcher_options _options;
    cache_chunk_fetcher_stats _stats;

    mutable std::mutex _mu;
    std::condition_variable _cv;
    std::condition_variable _idle_cv;
    bool _stopping = false;
    std::vector<std::thread> _threads;
    cache_token_bucket _bucket;
    std::unordered_map<chunk_sha, wanted, chunk_sha_hash> _wanted;
    std::multimap<clock::time_point, chunk_sha> _queue;
    std::unordered_set<chunk_sha, chunk_sha_hash> _in_flight;
    std::map<GroupId, std::size_t> _outstanding;
    std::map<GroupId, clock::time_point> _last_refresh;
};

}  // namespace kythira
