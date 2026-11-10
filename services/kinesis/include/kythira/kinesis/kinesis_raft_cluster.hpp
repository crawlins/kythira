// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kinesis_raft_cluster.hpp
/// @brief `stream_cluster` over a `multi_raft` host (`.kiro/specs/
///        kinesis-compatible-streams/` task 6).
///
/// - `propose` submits to the group owning the key. When this replica is not
///   the leader, the command goes to the leader's gateway through the
///   forwarder (`kinesis_forward.hpp`); without a leader hint or a
///   forwarder it fails as unavailable, and the client retries.
/// - `propose_local` is what the forwarding endpoint calls: it never
///   forwards again, so a leadership change mid-flight cannot bounce a
///   command around the cluster.
/// - `read` runs on this node's replica of the owning group (a replica
///   read: possibly behind the leader by the entries in flight).
///
/// The host must outlive this object. `Host` is a `kythira::multi_raft`
/// whose state machine is (or wraps, through `with_group_state_machine`) a
/// `stream_state_machine`.

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>

#include <raft/shard_exceptions.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace kythira::kinesis {

template<typename Host> class raft_stream_cluster {
public:
    using node_id_type = typename Host::node_id_type;
    using group_id_type = typename Host::group_id_type;
    /// Sends `command` for `key` to `leader`'s gateway; the decoded result,
    /// or nullopt when the leader could not be reached or did not commit.
    using forwarder = std::function<std::optional<apply_result>(
        const node_id_type& leader, const std::string& key, const std::vector<std::byte>& command)>;

    raft_stream_cluster(Host& host, forwarder forward,
                        std::chrono::milliseconds timeout = std::chrono::seconds{5})
        : _host(host), _forward(std::move(forward)), _timeout(timeout) {}

    auto propose(const std::string& key, std::vector<std::byte> command)
        -> std::optional<apply_result> {
        return submit(key, command, true);
    }

    auto propose_local(const std::string& key, const std::vector<std::byte>& command)
        -> std::optional<apply_result> {
        return submit(key, command, false);
    }

    auto read(const std::string& key, const stream_reader& f) -> bool {
        const auto d = _host.resolve(key);
        if (!d) {
            return false;
        }
        const auto done = _host.with_group_state_machine(
            d->_group_id, [&f](auto& sm, const auto& /*descriptor*/) {
                f(sm);
                return true;
            });
        return done.value_or(false);
    }

    /// The group this node routes `key` to now. Batching on it is safe even
    /// when it is stale: a part whose shard has left the group answers
    /// `not_found` and is retried alone.
    [[nodiscard]] auto group_of(const std::string& key) const -> std::optional<std::uint64_t> {
        const auto d = _host.resolve(key);
        if (!d) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(d->_group_id);
    }

    [[nodiscard]] auto now_ms() const -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] auto forwarded_count() const -> std::uint64_t { return _forwarded; }

private:
    Host& _host;
    forwarder _forward;
    std::chrono::milliseconds _timeout;
    std::atomic<std::uint64_t> _forwarded{0};

    auto submit(const std::string& key, const std::vector<std::byte>& command, bool may_forward)
        -> std::optional<apply_result> {
        try {
            auto bytes = _host.submit_command(key, command, _timeout).get();
            return decode_result(bytes);
        } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
            if (!may_forward || !_forward || !e.leader_hint() ||
                *e.leader_hint() == _host.node_id()) {
                return std::nullopt;
            }
            ++_forwarded;
            return _forward(*e.leader_hint(), key, command);
        } catch (const std::exception&) {
            // Unrouted key, no local replica, timeout, frozen shard: all
            // "not now" to the client, which retries.
            return std::nullopt;
        }
    }
};

}  // namespace kythira::kinesis
