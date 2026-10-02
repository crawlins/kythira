// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file redis_auth_limiter.hpp
/// @brief Admission control for the Redis gateway's AUTH, which costs one
///        PBKDF2 derivation (600k HMACs by default) on the shared worker pool.
///
/// Three rules, all decided before the KDF runs:
///
/// 1. Each attempt is charged to its source *before* the derivation and
///    refunded only if it succeeds. Charging after the fact (the first
///    version) let one source run any number of derivations concurrently:
///    none of them had failed yet when the next one was admitted.
/// 2. The source is the IPv4 address, or the IPv6 /64. A single host is
///    routinely handed a whole /64, so keying on the full IPv6 address gave
///    an attacker 2^64 fresh budgets.
/// 3. At most `max_concurrent` derivations run at once gateway-wide, so
///    AUTH traffic from many sources cannot occupy every worker and stall
///    authenticated commands. Past it the attempt is refused (not queued:
///    queueing would still hold a worker) and is not charged as a failure.

#include <boost/asio/ip/address.hpp>

#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kythira {

class redis_auth_limiter {
public:
    using clock = std::chrono::steady_clock;

    enum class verdict {
        /// Run the KDF, then call `finish()`.
        allowed,
        /// This source spent its failure budget for the current window.
        rate_limited,
        /// Too many derivations in flight gateway-wide; retry shortly.
        busy,
    };

    redis_auth_limiter(std::size_t failure_limit, std::chrono::seconds window,
                       std::size_t max_concurrent)
        : _failure_limit(failure_limit),
          _window(window),
          _max_concurrent(max_concurrent == 0 ? 1 : max_concurrent) {}

    /// The key AUTH attempts are charged to.
    [[nodiscard]] static auto rate_key(const boost::asio::ip::address& a) -> std::string {
        if (a.is_v4()) {
            return a.to_v4().to_string();
        }
        auto v6 = a.to_v6();
        if (v6.is_v4_mapped()) {
            return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, v6).to_string();
        }
        auto bytes = v6.to_bytes();
        for (std::size_t i = 8; i < bytes.size(); ++i) {
            bytes[i] = 0;
        }
        return boost::asio::ip::address_v6(bytes).to_string() + "/64";
    }

    /// Admit one attempt from `key`. On `allowed` the attempt is already
    /// counted as a failure and holds a concurrency slot; `finish()` must
    /// follow.
    auto begin(const std::string& key, clock::time_point now = clock::now()) -> verdict {
        std::lock_guard<std::mutex> lock(_mutex);
        auto it = _sources.find(key);
        if (it != _sources.end() && now - it->second._window_start > _window) {
            it->second = {};
        }
        if (it != _sources.end() && it->second._count >= _failure_limit) {
            return verdict::rate_limited;
        }
        if (_in_flight >= _max_concurrent) {
            return verdict::busy;
        }
        auto& f = _sources[key];
        if (f._count == 0) {
            f._window_start = now;
        }
        ++f._count;
        ++_in_flight;
        evict(key, now);
        return verdict::allowed;
    }

    /// Release the slot taken by an `allowed` `begin()`; a success refunds
    /// the failure it was provisionally charged.
    auto finish(const std::string& key, bool success) -> void {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_in_flight > 0) {
            --_in_flight;
        }
        if (!success) {
            return;
        }
        auto it = _sources.find(key);
        if (it != _sources.end() && it->second._count > 0) {
            --it->second._count;
        }
    }

    [[nodiscard]] auto in_flight() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _in_flight;
    }

    [[nodiscard]] auto tracked_sources() const -> std::size_t {
        std::lock_guard<std::mutex> lock(_mutex);
        return _sources.size();
    }

    /// Upper bound on tracked sources; an attacker cycling addresses must not
    /// grow the table without limit.
    static constexpr std::size_t k_max_tracked_sources = 65536;

private:
    struct failures {
        std::size_t _count = 0;
        clock::time_point _window_start{};
    };

    // Drop expired windows first; if still full, evict only the oldest window
    // (never `keep`). Clearing the whole table instead would let an attacker
    // reset every source's counter, its own included, on demand.
    auto evict(const std::string& keep, clock::time_point now) -> void {
        if (_sources.size() <= k_max_tracked_sources) {
            return;
        }
        std::erase_if(_sources,
                      [&](const auto& kv) { return now - kv.second._window_start > _window; });
        if (_sources.size() <= k_max_tracked_sources) {
            return;
        }
        auto oldest = _sources.end();
        for (auto it = _sources.begin(); it != _sources.end(); ++it) {
            if (it->first != keep && (oldest == _sources.end() ||
                                      it->second._window_start < oldest->second._window_start)) {
                oldest = it;
            }
        }
        if (oldest != _sources.end()) {
            _sources.erase(oldest);
        }
    }

    std::size_t _failure_limit;
    std::chrono::seconds _window;
    std::size_t _max_concurrent;
    mutable std::mutex _mutex;
    std::unordered_map<std::string, failures> _sources;
    std::size_t _in_flight = 0;
};

}  // namespace kythira
