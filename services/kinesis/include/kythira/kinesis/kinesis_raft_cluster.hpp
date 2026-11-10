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
/// - Write admission (`admission::shard_limits`) happens only where the
///   command is proposed: tokens a non-leader took are refunded before it
///   forwards, and the leader admits the forwarded command itself.
/// - `read` runs on this node's replica of the owning group (a replica
///   read: possibly behind the leader by the entries in flight).
///
/// The host must outlive this object. `Host` is a `kythira::multi_raft`
/// whose state machine is (or wraps, through `with_group_state_machine`) a
/// `stream_state_machine`.

#include <kythira/kinesis/kinesis_cluster.hpp>
#include <kythira/kinesis/kinesis_stream_state_machine.hpp>
#include <kythira/kinesis/kinesis_throttle.hpp>

#include <raft/future_default.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_envelope.hpp>
#include <raft/txn_state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
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
        const node_id_type& leader, const std::string& key, const std::vector<std::byte>& command,
        admission mode)>;

    raft_stream_cluster(Host& host, forwarder forward,
                        std::chrono::milliseconds timeout = std::chrono::seconds{5},
                        write_throttle* throttle = nullptr, disk_guard* guard = nullptr)
        : _host(host),
          _forward(std::move(forward)),
          _timeout(timeout),
          _throttle(throttle),
          _guard(guard) {}

    auto propose(const std::string& key, std::vector<std::byte> command,
                 admission mode = admission::none) -> std::optional<apply_result> {
        return submit(key, command, mode, true);
    }

    auto propose_local(const std::string& key, const std::vector<std::byte>& command,
                       admission mode = admission::none) -> std::optional<apply_result> {
        return submit(key, command, mode, false);
    }

    auto read(const std::string& key, const stream_reader& f) -> bool {
        const auto d = _host.resolve(key);
        if (!d) {
            return false;
        }
        const auto done = _host.with_group_state_machine(
            d->_group_id, [&f](auto& sm, const auto& /*descriptor*/) {
                if constexpr (k_transactional) {
                    f(sm.inner());
                } else {
                    f(sm);
                }
                return true;
            });
        return done.value_or(false);
    }

    // ---- resharding transactions (task 11) ---------------------------------

    /// Whether the host's state machines take part in tier-1 transactions:
    /// the daemon wraps them (`transactional_state_machine`), which every
    /// node of a cluster must agree on.
    static constexpr bool k_transactional =
        is_transactional_state_machine_v<typename Host::state_machine_type>;

    /// Sends a transaction op to `leader`'s gateway; the encoded
    /// `txn_result`, or nullopt.
    using txn_forwarder = std::function<std::optional<std::vector<std::byte>>(
        const node_id_type& leader, const std::vector<std::string>& keys,
        const std::vector<std::byte>& envelope)>;

    /// Set before the first transaction.
    void set_txn_forwarder(txn_forwarder f) { _txn_forward = std::move(f); }

    /// Submit a transaction op here when this node leads the keys' group,
    /// otherwise at the leader's gateway, until `timeout`. The encoded
    /// `txn_result`; throws when no leader took it. What the coordinator and
    /// the host's lock resolver send through.
    auto txn_send(const std::vector<std::string>& keys, const std::vector<std::byte>& envelope,
                  std::chrono::milliseconds timeout) -> std::vector<std::byte>
    requires k_transactional
    {
        if (keys.empty()) {
            throw std::invalid_argument("kinesis: a transaction op names no key");
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const auto left = [&deadline] {
            return std::max(std::chrono::duration_cast<std::chrono::milliseconds>(
                                deadline - std::chrono::steady_clock::now()),
                            std::chrono::milliseconds{1});
        };
        for (;;) {
            std::optional<node_id_type> hint;
            try {
                auto f = _host.submit_txn(keys, envelope, left());
                return std::move(f).get();
            } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
                hint = e.leader_hint();
            } catch (const unknown_shard_exception<group_id_type>&) {
                // No local replica: nobody here to ask.
            }
            if (hint && *hint != _host.node_id() && _txn_forward) {
                if (auto r = _txn_forward(*hint, keys, envelope)) {
                    return std::move(*r);
                }
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                throw std::runtime_error("kinesis: no leader took the transaction op");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
    }

    /// A transaction op another gateway forwarded: submitted here only.
    auto txn_submit_local(const std::vector<std::string>& keys,
                          const std::vector<std::byte>& envelope)
        -> std::optional<std::vector<std::byte>>
    requires k_transactional
    {
        try {
            auto f = _host.submit_txn(keys, envelope, _timeout);
            return std::move(f).get();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    /// Every write or none, as one tier-1 transaction (read committed, no-wait
    /// locks): what a reshard commits its catalog, parent and child writes
    /// with.
    auto transact(std::vector<txn_write> writes) -> transact_outcome
    requires k_transactional
    {
        coordinator_client client(*this);
        txn_options options;
        options.timeout = _timeout;
        transaction_coordinator<coordinator_client, std::string> coordinator(client, options);
        try {
            coordinator.begin();
            for (auto& w : writes) {
                coordinator.put(w._key, std::move(w._command));
            }
            const auto out = coordinator.commit();
            switch (out._kind) {
                case txn_outcome_kind::committed:
                    return transact_outcome::committed;
                case txn_outcome_kind::aborted:
                    return transact_outcome::aborted;
                case txn_outcome_kind::unknown:
                    return transact_outcome::unknown;
            }
        } catch (const std::exception&) {
            return transact_outcome::unknown;
        }
        return transact_outcome::unknown;
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

    /// Whether this node leads the group owning `key` now.
    [[nodiscard]] auto leads(const std::string& key) const -> bool {
        const auto d = _host.resolve(key);
        if (!d) {
            return false;
        }
        auto* node = _host.group_node(d->_group_id);
        return node != nullptr && node->is_leader();
    }

    [[nodiscard]] auto now_ms() const -> std::int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    [[nodiscard]] auto forwarded_count() const -> std::uint64_t { return _forwarded; }

private:
    /// The coordinator's view of the participant shards
    /// (`txn_participant_client`), over this cluster.
    class coordinator_client {
    public:
        using group_id_type = typename Host::group_id_type;
        using send_future = future_default<std::vector<std::byte>>;

        explicit coordinator_client(raft_stream_cluster& cluster) : _cluster(cluster) {}

        [[nodiscard]] auto resolve(const std::string& key) -> std::optional<group_id_type> {
            if (auto d = _cluster._host.resolve(key)) {
                return d->_group_id;
            }
            return std::nullopt;
        }

        [[nodiscard]] auto send(const std::vector<std::string>& keys,
                                const std::vector<std::byte>& envelope,
                                std::chrono::milliseconds timeout) -> send_future {
            promise_default<std::vector<std::byte>> promise;
            auto result = promise.getFuture();
            try {
                promise.setValue(_cluster.txn_send(keys, envelope, timeout));
            } catch (...) {
                promise.setException(std::current_exception());
            }
            return result;
        }

        [[nodiscard]] auto refresh(std::exception_ptr error) -> bool {
            try {
                std::rethrow_exception(std::move(error));
            } catch (
                const shard_epoch_mismatch_exception<group_id_type, std::string, node_id_type>& e) {
                std::ignore = _cluster._host.learn_descriptors(e.current_descriptors());
                return true;
            } catch (...) {
                return false;
            }
        }

        /// A reshard reads nothing inside its transaction (the front end
        /// read the catalog under the stream's UPDATING status).
        [[nodiscard]] auto read(const std::string& /*key*/, std::chrono::milliseconds /*t*/)
            -> txn_read_result<std::string> {
            return {};
        }

        [[nodiscard]] auto now_ms() const -> std::uint64_t { return _cluster._host.txn_now_ms(); }

    private:
        raft_stream_cluster& _cluster;
    };

    Host& _host;
    forwarder _forward;
    std::chrono::milliseconds _timeout;
    write_throttle* _throttle;
    disk_guard* _guard;
    txn_forwarder _txn_forward;
    std::atomic<std::uint64_t> _forwarded{0};

    /// `submit_command`, waiting out a reshard transaction that holds the
    /// key: its locks last one commit round trip, and the host's resolver
    /// clears an abandoned one after its TTL.
    auto submit_unlocked(const std::string& key, const std::vector<std::byte>& bytes)
        -> std::vector<std::byte> {
        const auto deadline = std::chrono::steady_clock::now() + _timeout;
        auto backoff = std::chrono::milliseconds{2};
        for (;;) {
            if constexpr (k_transactional) {
                try {
                    return _host.submit_command(key, bytes, _timeout).get();
                } catch (const key_locked_exception<std::string>&) {
                    if (std::chrono::steady_clock::now() + backoff >= deadline) {
                        throw;
                    }
                }
                std::this_thread::sleep_for(backoff);
                backoff = std::min(backoff * 2, std::chrono::milliseconds{100});
            } else {
                return _host.submit_command(key, bytes, _timeout).get();
            }
        }
    }

    auto submit(const std::string& key, const std::vector<std::byte>& command, admission mode,
                bool may_forward) -> std::optional<apply_result> {
        // Here, and again on the leader a forwarded append reaches.
        if (auto refused = refuse_when_full(_guard, command)) {
            return refused;
        }
        try {
            return propose_admitted(_throttle, mode, command, [&](const auto& bytes) {
                return std::optional{decode_result(submit_unlocked(key, bytes))};
            });
        } catch (const shard_not_leader_exception<group_id_type, node_id_type>& e) {
            if (!may_forward || !_forward || !e.leader_hint() ||
                *e.leader_hint() == _host.node_id()) {
                return std::nullopt;
            }
            ++_forwarded;
            return _forward(*e.leader_hint(), key, command, mode);
        } catch (const std::exception&) {
            // Unrouted key, no local replica, timeout, frozen shard: all
            // "not now" to the client, which retries.
            return std::nullopt;
        }
    }
};

}  // namespace kythira::kinesis
