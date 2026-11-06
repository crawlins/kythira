// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Rate limiting for failed SigV4 verifications
// (`.kiro/specs/iam-compatible-auth/` Requirement 3.8), on the
// `redis_auth_limiter` design (services/redis/.../redis_auth_limiter.hpp):
//
//  - Failures are counted per fixed window, per key, and a key at its limit
//    is refused until its window ends.
//  - A source address is the IPv4 address, or the IPv6 /64: one host is
//    routinely given a whole /64, so a full IPv6 address would hand an
//    attacker 2^64 fresh budgets. IPv4-mapped IPv6 counts as the IPv4
//    address.
//  - The table is bounded. When full, expired windows go first and then the
//    single oldest window, never the key being charged: clearing the table
//    would let an attacker reset every counter, its own included.
//
// Two budgets are kept, one per source address and one per access key id,
// and a request is refused when either is spent (Requirement 3.8 names
// both). Unlike AUTH's KDF, a SigV4 check costs a few HMACs, so there is no
// concurrency cap and an attempt is charged only when it fails.
//
// The per-key budget is the double-edged one: access key ids are not
// secret, so anyone can spend a key's budget and lock its owner out for a
// window. Its default is therefore five times the per-source one, and a
// limit of 0 turns either budget off.

#include <raft/sigv4_verifier.hpp>

#include <arpa/inet.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace kythira::sigv4 {

struct failure_limits {
    std::size_t per_source = 20;  // failures per window per address (or /64); 0 = off
    std::size_t per_key = 100;    // failures per window per access key id; 0 = off
    std::chrono::seconds window{60};
    std::size_t max_tracked = 65536;  // entries per table
};

class failure_limiter {
public:
    using clock = std::chrono::steady_clock;

    enum class verdict : std::uint8_t {
        allowed,
        source_limited,  // this address (or /64) spent its budget
        key_limited,     // this access key id spent its budget
    };

    explicit failure_limiter(failure_limits limits = {}) : _limits(limits) {}

    /// The key a source address is charged to: the IPv4 address, or the
    /// IPv6 /64 as "<prefix>::/64". An unparsable address is used as given.
    [[nodiscard]] static auto source_key(std::string_view address) -> std::string {
        std::string a(address);
        if (a.size() > 2 && a.front() == '[' && a.back() == ']') {
            a = a.substr(1, a.size() - 2);
        }
        std::array<unsigned char, 16> v6{};
        std::array<unsigned char, 4> v4{};
        std::array<char, INET6_ADDRSTRLEN> text{};
        if (inet_pton(AF_INET, a.c_str(), v4.data()) == 1) {
            inet_ntop(AF_INET, v4.data(), text.data(), text.size());
            return text.data();
        }
        if (inet_pton(AF_INET6, a.c_str(), v6.data()) != 1) {
            return a;
        }
        static constexpr std::array<unsigned char, 12> mapped{0, 0, 0, 0, 0,    0,
                                                              0, 0, 0, 0, 0xFF, 0xFF};
        if (std::memcmp(v6.data(), mapped.data(), mapped.size()) == 0) {
            inet_ntop(AF_INET, v6.data() + 12, text.data(), text.size());
            return text.data();
        }
        for (std::size_t i = 8; i < v6.size(); ++i) {
            v6[i] = 0;
        }
        inet_ntop(AF_INET6, v6.data(), text.data(), text.size());
        return std::string(text.data()) + "/64";
    }

    /// Whether to verify a request from `source` (a `source_key()`) for
    /// `access_key_id` at all. Call before verifying; a refusal answers
    /// with a throttling error and is not itself charged.
    [[nodiscard]] auto check(const std::string& source, std::string_view access_key_id,
                             clock::time_point now = clock::now()) -> verdict {
        const std::lock_guard lock(_mu);
        if (spent(_sources, source, _limits.per_source, now)) {
            return verdict::source_limited;
        }
        if (spent(_keys, std::string(access_key_id), _limits.per_key, now)) {
            return verdict::key_limited;
        }
        return verdict::allowed;
    }

    /// Charges a failed verification to both budgets.
    void record_failure(const std::string& source, std::string_view access_key_id,
                        clock::time_point now = clock::now()) {
        const std::lock_guard lock(_mu);
        if (_limits.per_source != 0) {
            charge(_sources, source, now);
        }
        if (_limits.per_key != 0 && !access_key_id.empty()) {
            charge(_keys, std::string(access_key_id), now);
        }
    }

    [[nodiscard]] auto tracked_sources() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _sources.size();
    }
    [[nodiscard]] auto tracked_keys() const -> std::size_t {
        const std::lock_guard lock(_mu);
        return _keys.size();
    }

private:
    struct window {
        std::size_t count = 0;
        clock::time_point start{};
    };
    using table = std::unordered_map<std::string, window>;

    [[nodiscard]] auto spent(table& t, const std::string& key, std::size_t limit,
                             clock::time_point now) const -> bool {
        if (limit == 0) {
            return false;
        }
        const auto it = t.find(key);
        if (it == t.end()) {
            return false;
        }
        if (now - it->second.start >= _limits.window) {
            t.erase(it);
            return false;
        }
        return it->second.count >= limit;
    }

    void charge(table& t, const std::string& key, clock::time_point now) {
        auto& w = t[key];
        if (w.count == 0 || now - w.start >= _limits.window) {
            w = window{0, now};
        }
        ++w.count;
        evict(t, key, now);
    }

    void evict(table& t, const std::string& keep, clock::time_point now) const {
        if (t.size() <= _limits.max_tracked) {
            return;
        }
        std::erase_if(t, [&](const auto& kv) { return now - kv.second.start >= _limits.window; });
        while (t.size() > _limits.max_tracked) {
            auto oldest = t.end();
            for (auto it = t.begin(); it != t.end(); ++it) {
                if (it->first != keep &&
                    (oldest == t.end() || it->second.start < oldest->second.start)) {
                    oldest = it;
                }
            }
            if (oldest == t.end()) {
                return;
            }
            t.erase(oldest);
        }
    }

    failure_limits _limits;
    mutable std::mutex _mu;
    table _sources;
    table _keys;
};

/// The throttling error to answer a refused request with, as each protocol
/// spells it.
[[nodiscard]] inline auto throttled(error_protocol p) -> aws_error {
    switch (p) {
        case error_protocol::query:
            return {"Throttling", "Rate exceeded", 400};
        case error_protocol::json:
            return {"ThrottlingException", "Rate exceeded", 400};
        case error_protocol::s3:
            return {"SlowDown", "Please reduce your request rate.", 503};
    }
    return {"Throttling", "Rate exceeded", 400};
}

}  // namespace kythira::sigv4
