// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file cache_disk_guard.hpp
/// @brief The disk budget and the free-space floors of a disk-backed cache
///        node, for itself and for the voters it hears from
///        (.kiro/specs/disk-backed-build-cache/ design Component 2
///        "advisories", Requirement 6).
///
/// A leader proposes every write, but the disk that fills may be a
/// follower's: a follower that runs out of space or inodes while applying
/// stops rather than diverge (Requirement 2.4). So each node tells the
/// leaders of the shards it follows how much room it has, once per policy
/// tick, on the gateway's forwarding link (`KYTHIRA.ADVISE`). A leader
/// refuses new writes while it, or any voter it has a fresh report from, is
/// below either floor (Requirement 6.6), and evicts while any of them holds
/// more chunk bytes than the budget (Requirement 6.1).
///
/// Floors and budgets are each node's own setting, so a report carries the
/// sender's along with its figures: the leader holds a voter to that
/// voter's floor and budget, not its own (Requirement 6.6 says "below its
/// floor"). A cluster configured alike sees no difference.
///
/// A report goes stale after `_report_ttl`. A stale or missing report is
/// not counted against the shard: a voter that is down is not applying, and
/// refusing writes because a node stopped talking would turn one failure
/// into an outage.
///
/// Node ids are kept as text, the form they travel in, so this header
/// depends on nothing of the host's.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kythira {

struct cache_disk_guard_options {
    /// `KYTHIRA_CACHE_MAX_DISK_BYTES`: chunk-store bytes per node before
    /// eviction. 0 means no budget.
    std::uint64_t _max_disk_bytes = 0;
    /// `KYTHIRA_CACHE_MIN_FREE_BYTES` and `KYTHIRA_CACHE_MIN_FREE_INODES`.
    std::uint64_t _min_free_bytes = std::uint64_t{1} * 1024 * 1024 * 1024;
    std::uint64_t _min_free_inodes = 10000;
    /// A report older than this is ignored.
    std::chrono::milliseconds _report_ttl{30000};
};

/// What one node says about its data directory.
struct cache_node_report {
    std::uint64_t _free_bytes = 0;
    std::uint64_t _free_inodes = 0;
    /// The chunk store's bytes on disk, packs included.
    std::uint64_t _store_bytes = 0;
    /// Of those, bytes of chunks still inside the orphan grace: no eviction
    /// frees them until it runs out.
    std::uint64_t _pinned_bytes = 0;
    /// The sender's own floors and budget (`cache_disk_guard_options`).
    std::uint64_t _min_free_bytes = 0;
    std::uint64_t _min_free_inodes = 0;
    std::uint64_t _max_disk_bytes = 0;

    static constexpr std::size_t field_count = 7;

    /// `free_bytes,free_inodes,store_bytes,pinned_bytes,min_free_bytes,
    /// min_free_inodes,max_disk_bytes`, the advisory's wire form.
    [[nodiscard]] auto encode() const -> std::string {
        return std::to_string(_free_bytes) + "," + std::to_string(_free_inodes) + "," +
               std::to_string(_store_bytes) + "," + std::to_string(_pinned_bytes) + "," +
               std::to_string(_min_free_bytes) + "," + std::to_string(_min_free_inodes) + "," +
               std::to_string(_max_disk_bytes);
    }
    [[nodiscard]] static auto decode(std::string_view text) -> std::optional<cache_node_report> {
        cache_node_report out;
        std::uint64_t* fields[field_count] = {
            &out._free_bytes,     &out._free_inodes,     &out._store_bytes,   &out._pinned_bytes,
            &out._min_free_bytes, &out._min_free_inodes, &out._max_disk_bytes};
        std::size_t i = 0;
        std::uint64_t v = 0;
        bool digits = false;
        for (std::size_t pos = 0; pos <= text.size(); ++pos) {
            if (pos == text.size() || text[pos] == ',') {
                if (!digits || i >= field_count) {
                    return std::nullopt;
                }
                *fields[i++] = v;
                v = 0;
                digits = false;
                continue;
            }
            const char c = text[pos];
            if (c < '0' || c > '9' || v > (std::numeric_limits<std::uint64_t>::max() - 9) / 10) {
                return std::nullopt;
            }
            v = (v * 10) + static_cast<std::uint64_t>(c - '0');
            digits = true;
        }
        if (i != field_count) {
            return std::nullopt;
        }
        return out;
    }
};

class cache_disk_guard {
public:
    using clock = std::chrono::steady_clock;

    cache_disk_guard(std::string self, cache_disk_guard_options options)
        : _self(std::move(self)), _options(options) {}

    [[nodiscard]] auto self() const -> const std::string& { return _self; }
    [[nodiscard]] auto options() const noexcept -> const cache_disk_guard_options& {
        return _options;
    }

    /// This node's own figures, refreshed by the daemon once per tick. The
    /// floors and budget are stamped from this guard's options; the stamped
    /// report, the one to advise, is returned.
    auto set_local(cache_node_report report, clock::time_point now = clock::now())
        -> cache_node_report {
        report._min_free_bytes = _options._min_free_bytes;
        report._min_free_inodes = _options._min_free_inodes;
        report._max_disk_bytes = _options._max_disk_bytes;
        const std::lock_guard lock(_mu);
        _local = report;
        _local_at = now;
        return report;
    }
    [[nodiscard]] auto local() const -> std::optional<cache_node_report> {
        const std::lock_guard lock(_mu);
        return _local;
    }

    /// A report from `node`, received at `now`.
    void record(const std::string& node, const cache_node_report& report,
                clock::time_point now = clock::now()) {
        if (node == _self) {
            return;
        }
        const std::lock_guard lock(_mu);
        _peers[node] = peer{report, now};
        ++_reports_received;
    }

    /// Why a write to a shard with these voters must be refused, or
    /// `nullopt` when it may go ahead. The reason names the node.
    [[nodiscard]] auto refusal(const std::vector<std::string>& voters,
                               clock::time_point now = clock::now()) const
        -> std::optional<std::string> {
        const std::lock_guard lock(_mu);
        auto check = [](const std::string& who,
                        const cache_node_report& r) -> std::optional<std::string> {
            if (r._free_bytes < r._min_free_bytes) {
                return "node " + who + " has " + std::to_string(r._free_bytes) +
                       " bytes free, below the floor of " + std::to_string(r._min_free_bytes);
            }
            if (r._free_inodes < r._min_free_inodes) {
                return "node " + who + " has " + std::to_string(r._free_inodes) +
                       " inodes free, below the floor of " + std::to_string(r._min_free_inodes);
            }
            return std::nullopt;
        };
        std::optional<std::string> reason;
        if (_local) {
            reason = check(_self, *_local);
        }
        for (const auto& v : voters) {
            if (reason) {
                break;
            }
            if (v == _self) {
                continue;
            }
            if (const auto* p = fresh(v, now)) {
                reason = check(v, p->_report);
            }
        }
        if (reason) {
            ++_refusals;
        }
        return reason;
    }

    /// Chunk-store bytes over its own budget on the fullest of this node and
    /// the voters with fresh reports; 0 when none is over. A node with no
    /// budget (0) is never over. Nor is a node whose pinned bytes alone
    /// fill its budget: evicting other keys cannot bring it under, only the
    /// grace running out can, and asking would empty the shard for nothing.
    ///
    /// Reports taken before `not_before` are left out. A leader passes the
    /// time of its last eviction for the shard: until the voters report
    /// again they still show the bytes it just freed, and acting on those
    /// would evict the same excess twice.
    [[nodiscard]] auto excess_bytes(const std::vector<std::string>& voters,
                                    clock::time_point now = clock::now(),
                                    clock::time_point not_before = clock::time_point::min()) const
        -> std::uint64_t {
        const std::lock_guard lock(_mu);
        std::uint64_t worst = 0;
        auto consider = [&](const cache_node_report& r) {
            if (r._max_disk_bytes != 0 && r._store_bytes > r._max_disk_bytes &&
                r._pinned_bytes < r._max_disk_bytes) {
                worst = std::max(worst, r._store_bytes - r._max_disk_bytes);
            }
        };
        if (_local && _local_at >= not_before) {
            consider(*_local);
        }
        for (const auto& v : voters) {
            if (v == _self) {
                continue;
            }
            if (const auto* p = fresh(v, now); p != nullptr && p->_at >= not_before) {
                consider(p->_report);
            }
        }
        return worst;
    }

    [[nodiscard]] auto refusals() const -> std::uint64_t {
        const std::lock_guard lock(_mu);
        return _refusals;
    }
    [[nodiscard]] auto reports_received() const -> std::uint64_t {
        const std::lock_guard lock(_mu);
        return _reports_received;
    }
    /// Peers with a fresh report.
    [[nodiscard]] auto fresh_peers(clock::time_point now = clock::now()) const -> std::size_t {
        const std::lock_guard lock(_mu);
        std::size_t n = 0;
        for (const auto& [node, p] : _peers) {
            n += fresh(node, now) != nullptr ? 1 : 0;
        }
        return n;
    }

private:
    struct peer {
        cache_node_report _report;
        clock::time_point _at;
    };

    /// Called under `_mu`.
    [[nodiscard]] auto fresh(const std::string& node, clock::time_point now) const -> const peer* {
        auto it = _peers.find(node);
        if (it == _peers.end() || now - it->second._at > _options._report_ttl) {
            return nullptr;
        }
        return &it->second;
    }

    std::string _self;
    cache_disk_guard_options _options;
    mutable std::mutex _mu;
    std::optional<cache_node_report> _local;
    clock::time_point _local_at{};
    std::map<std::string, peer> _peers;
    mutable std::uint64_t _refusals = 0;
    std::uint64_t _reports_received = 0;
};

}  // namespace kythira
