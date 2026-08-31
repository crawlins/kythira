// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file data_path_target.hpp
/// @brief The out-of-process half of the workload seam.
///        `.kiro/specs/multi-raft-host-binary/` Requirements 3 and 4.1.
///
/// **This file is the entire difference between a Tier B row and a Tier C
/// one.** The key sampler, value construction, the command mix, the read-kind
/// taxonomy, the open- and closed-loop schedules, `latency_sample_set`,
/// `repeated_result`, the spread rule and `verdict()` are all above the seam
/// and shared with `tests/multi_raft_transport_harness.hpp`. What differs is
/// the submit step, and it differs here.
///
/// If a future reader finds this file sampling a key, building a value or
/// computing a percentile, the design has failed and every cross-tier delta it
/// produced is a comparison of two workloads rather than of two tiers.

#include "kv_data_path.hpp"

// Shared, never reimplemented. See above.
#include "multi_raft_kv_workload.hpp"

#include <httplib.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace kythira::bench {

/// @brief One host of the cluster under test, as the driver addresses it.
struct host_endpoint {
    std::uint64_t _node_id{0};
    std::string _address;
    std::uint16_t _data_port{0};
    std::uint16_t _control_port{0};
};

/// @brief What the driver needs to know before it can offer load.
struct target_options {
    std::vector<host_endpoint> _hosts;
    /// The tiling the hosts were pre-split into. Computed with the SAME
    /// `kv_shard_ranges` the hosts used, so the driver's idea of which shard
    /// holds a key is the hosts' idea by construction rather than by agreement.
    std::size_t _groups{4};
    std::uint64_t _key_count{100000};
    std::string _media_type{"application/json"};
    std::chrono::milliseconds _connect_timeout{2000};
    std::chrono::milliseconds _request_timeout{5000};
    /// What the row should say it is. Tier C when the hosts are processes on
    /// one machine, Tier E when they are on several — the driver cannot tell
    /// the difference and must be told, which is why Requirement 3.5 asks for
    /// the placement to be recorded rather than inferred.
    kythira::testing::deployment_tier _tier{kythira::testing::deployment_tier::c_process};
    std::string _transport_name{"cpp-httplib"};
    /// Where the hosts and this driver actually are, recorded verbatim on the
    /// row. Requirement 3.5: the driver cannot tell one machine from several,
    /// so this is an input rather than an inference, and it says "not stated"
    /// rather than guessing when nobody supplied one.
    std::string _placement{"not stated"};
    std::string _node_serializer{"application/json"};
    std::chrono::milliseconds _tick_interval{2};
    kythira::testing::durability_mode _durability{kythira::testing::durability_mode::memory};
};

/// @brief Offers the shared workload's operations over the data path.
///
/// **Leader routing is resolved out of band and never inside a measured
/// window's first request.** The driver asks each host's *control* port which
/// groups it leads before the window opens, and addresses the right host
/// directly. A not-leader answer is still possible — leadership moves — and
/// when one arrives it is counted in `operation_tally::_not_leader` and the
/// cache is refreshed. That keeps the routing cost visible, which is the whole
/// reason Requirement 2.2 forbids the host from forwarding.
class data_path_target {
public:
    using key_type = std::string;

    explicit data_path_target(target_options options)
        : _options(std::move(options)),
          _ranges(kythira::testing::kv_shard_ranges(_options._groups, _options._key_count)) {}

    /// @brief Wait until every host reports every group has a leader.
    ///
    /// Before the window, never inside one. A driver that starts offering load
    /// while the cluster is still electing measures the election, and this
    /// project has already established that a window containing an election is
    /// not a window measuring a workload.
    auto await_ready(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (all_hosts_ready()) {
                refresh_leaders();
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        return false;
    }

    /// @brief Re-read every host's `/leaders`, out of band.
    auto refresh_leaders() -> void {
        std::map<std::uint64_t, std::size_t> found;
        for (std::size_t i = 0; i < _options._hosts.size(); ++i) {
            const auto& host = _options._hosts[i];
            httplib::Client client(host._address, host._control_port);
            client.set_connection_timeout(_options._connect_timeout);
            client.set_read_timeout(_options._request_timeout);
            client.set_tcp_nodelay(true);
            auto res = client.Get("/leaders");
            if (!res || res->status != 200) {
                continue;
            }
            for (auto group : parse_leads(res->body)) {
                found[group] = i;
            }
        }
        std::unique_lock lock(_leader_mu);
        for (const auto& [group, index] : found) {
            _leader_of_group[group] = index;
        }
    }

    // ── the seam ─────────────────────────────────────────────────────────────

    auto submit_write(const key_type& key, const std::vector<std::byte>& command,
                      std::chrono::milliseconds timeout, kythira::testing::operation_tally& tally)
        -> std::optional<std::chrono::nanoseconds> {
        return timed(key, tally, timeout,
                     [&](kv_data_client& client) { return client.put(key, command); });
    }

    auto submit_read(kythira::testing::read_kind kind, const key_type& key,
                     std::chrono::milliseconds timeout, kythira::testing::operation_tally& tally,
                     std::uint64_t& bytes_returned) -> std::optional<std::chrono::nanoseconds> {
        const auto wire = to_wire(kind);
        // A local stale read is deliberately NOT sent to the leader: served by
        // the leader it would be stale only in theory, and served by a replica
        // it is stale the way a deployment would experience it.
        const bool prefer_follower = wire == wire_read_kind::local;
        return timed(
            key, tally, timeout,
            [&](kv_data_client& client) {
                return client.get(key, wire, kythira::testing::kv_get(key));
            },
            &bytes_returned, prefer_follower);
    }

    /// @brief The sum of every group's term across every host.
    ///
    /// Differenced around a measured window, a change means somebody
    /// re-elected inside it — and a window containing an election is not a
    /// window measuring a workload. Read from the **control** port, before and
    /// after, so it costs the measured path nothing.
    [[nodiscard]] auto term_sum() -> std::uint64_t {
        std::uint64_t total = 0;
        for (const auto& host : _options._hosts) {
            httplib::Client client(host._address, host._control_port);
            client.set_connection_timeout(_options._connect_timeout);
            client.set_read_timeout(_options._request_timeout);
            client.set_tcp_nodelay(true);
            auto res = client.Get("/terms");
            if (!res || res->status != 200) {
                continue;
            }
            const auto at = res->body.find("\"term_sum\":");
            if (at == std::string::npos) {
                continue;
            }
            try {
                total += static_cast<std::uint64_t>(std::stoull(res->body.substr(at + 11)));
            } catch (const std::exception&) {
            }
        }
        return total;
    }

    /// @brief Replication counters, which an out-of-process driver cannot see.
    ///
    /// The host is deliberately uninstrumented — `.kiro/specs/multi-raft-
    /// performance/` Requirement 8.2 keeps counters out of the measured
    /// process, and a host that counted its own RPCs would be doing work the
    /// Tier B rows did not, inside the very window being compared with them.
    /// So these come back empty and the row records that they are **absent**
    /// rather than zero; a zero here would read as "no replication happened".
    [[nodiscard]] auto rpc_counts() const -> kythira::testing::rpc_snapshot { return {}; }
    [[nodiscard]] auto durability_counts() const -> kythira::testing::durability_snapshot {
        return {};
    }
    [[nodiscard]] static auto has_internal_counters() -> bool { return false; }

    /// @brief Fill the labels that come from the deployment rather than from
    ///        the workload.
    auto describe(kythira::testing::benchmark_result& row) const -> void {
        row._transport = _options._transport_name;
        row._serializer = _options._media_type;
        row._node_serializer = _options._node_serializer;
        row._tier = _options._tier;
        row._nodes = _options._hosts.size();
        row._groups = _options._groups;
        row._tick_interval = _options._tick_interval;
        row._durability = _options._durability;
        row._internal_counters = false;
        row._placement = _options._placement;
    }

private:
    [[nodiscard]] static auto to_wire(kythira::testing::read_kind kind) -> wire_read_kind {
        switch (kind) {
            case kythira::testing::read_kind::read_state:
                return wire_read_kind::state;
            case kythira::testing::read_kind::log_get:
                return wire_read_kind::log;
            case kythira::testing::read_kind::local_stale:
                return wire_read_kind::local;
        }
        return wire_read_kind::state;
    }

    [[nodiscard]] auto group_of_key(const key_type& key) const -> std::optional<std::uint64_t> {
        for (std::size_t i = 0; i < _ranges.size(); ++i) {
            if (_ranges[i].contains(key)) {
                return static_cast<std::uint64_t>(i + 1);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto host_for(std::uint64_t group, bool prefer_follower) -> std::size_t {
        std::shared_lock lock(_leader_mu);
        const auto it = _leader_of_group.find(group);
        const std::size_t leader = it == _leader_of_group.end() ? 0 : it->second;
        if (!prefer_follower || _options._hosts.size() < 2) {
            return leader;
        }
        // Any replica other than the leader. Every host holds a replica of
        // every shard in this deployment, which is what the in-process rows do
        // too, so "the next one along" is always a replica.
        return (leader + 1) % _options._hosts.size();
    }

    /// One `httplib::Client` per (thread, host).
    ///
    /// Per thread because `httplib::Client` serialises requests on its
    /// connection, and a shared client would turn the driver's concurrency
    /// into a queue that the row would then report as the cluster's latency —
    /// which is the exact confound this whole tier exists to remove.
    [[nodiscard]] auto client_for(std::size_t host_index) -> kv_data_client& {
        thread_local std::unordered_map<std::size_t, std::unique_ptr<kv_data_client>> clients;
        auto it = clients.find(host_index);
        if (it == clients.end()) {
            const auto& host = _options._hosts.at(host_index);
            it = clients
                     .emplace(host_index, std::make_unique<kv_data_client>(
                                              host._address, host._data_port, _options._media_type,
                                              _options._connect_timeout, _options._request_timeout))
                     .first;
        }
        return *it->second;
    }

    template<typename Call>
    auto timed(const key_type& key, kythira::testing::operation_tally& tally,
               std::chrono::milliseconds /*timeout*/, Call&& call,
               std::uint64_t* bytes_returned = nullptr, bool prefer_follower = false)
        -> std::optional<std::chrono::nanoseconds> {
        const auto started = std::chrono::steady_clock::now();
        ++tally._offered;

        const auto group = group_of_key(key);
        if (!group.has_value()) {
            // No range holds this key. A tiling failure, not a slow operation,
            // and counted where a reader will see it rather than dropped.
            ++tally._not_leader;
            return std::nullopt;
        }

        auto result = call(client_for(host_for(*group, prefer_follower)));
        switch (result._outcome) {
            case kv_outcome::ok:
                if (bytes_returned != nullptr) {
                    *bytes_returned += result._value.size();
                }
                ++tally._completed;
                return std::chrono::steady_clock::now() - started;
            case kv_outcome::no_such_key:
                // A miss is a completed read. The store is preloaded before the
                // window, so a miss means the replica has not applied that
                // entry yet — exactly the staleness a local read exists to
                // expose, and counting it as a failure would hide it.
                ++tally._completed;
                return std::chrono::steady_clock::now() - started;
            case kv_outcome::not_leader:
                ++tally._not_leader;
                note_leader(*group, result._leader);
                return std::nullopt;
            case kv_outcome::timeout:
                ++tally._timeout;
                return std::nullopt;
            case kv_outcome::other:
                ++tally._other;
                return std::nullopt;
        }
        ++tally._other;
        return std::nullopt;
    }

    auto note_leader(std::uint64_t group, std::optional<std::uint64_t> leader_id) -> void {
        if (!leader_id.has_value()) {
            return;
        }
        for (std::size_t i = 0; i < _options._hosts.size(); ++i) {
            if (_options._hosts[i]._node_id == *leader_id) {
                std::unique_lock lock(_leader_mu);
                _leader_of_group[group] = i;
                return;
            }
        }
    }

    [[nodiscard]] auto all_hosts_ready() -> bool {
        for (const auto& host : _options._hosts) {
            httplib::Client client(host._address, host._control_port);
            client.set_connection_timeout(_options._connect_timeout);
            client.set_read_timeout(_options._request_timeout);
            client.set_tcp_nodelay(true);
            auto res = client.Get("/ready");
            if (!res || res->status != 200) {
                return false;
            }
        }
        return true;
    }

    /// `{"node_id":1,"leads":[1,3]}` — small enough that a scan for the digits
    /// after `"leads":[` is clearer than pulling in a parser, and the producer
    /// is `control_server` three files away rather than an arbitrary peer.
    [[nodiscard]] static auto parse_leads(const std::string& body) -> std::vector<std::uint64_t> {
        std::vector<std::uint64_t> out;
        const auto open = body.find("\"leads\":[");
        if (open == std::string::npos) {
            return out;
        }
        const auto close = body.find(']', open);
        if (close == std::string::npos) {
            return out;
        }
        std::uint64_t value = 0;
        bool in_number = false;
        for (auto i = open + 9; i < close; ++i) {
            const char c = body[i];
            if (c >= '0' && c <= '9') {
                value = value * 10 + static_cast<std::uint64_t>(c - '0');
                in_number = true;
            } else if (in_number) {
                out.push_back(value);
                value = 0;
                in_number = false;
            }
        }
        if (in_number) {
            out.push_back(value);
        }
        return out;
    }

    target_options _options;
    std::vector<kythira::shard_range<std::string>> _ranges;
    std::shared_mutex _leader_mu;
    std::map<std::uint64_t, std::size_t> _leader_of_group;
};

}  // namespace kythira::bench
