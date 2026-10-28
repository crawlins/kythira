// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_txn_client.hpp
/// @brief A `txn_participant_client` over in-process `multi_raft` hosts
///        (task 6 of `.kiro/specs/multi-raft-transactions/`).
///
/// For an application that embeds its hosts, and for the tests. Each send
/// goes to whichever host's replica of the owning group leads, by calling
/// `multi_raft::submit_txn` on it; a host that turns out not to lead, or not
/// to hold a replica, is skipped for the next. A routing refusal is the
/// coordinator's to see: `refresh()` teaches every host the descriptors it
/// carried, so the coordinator's re-partition resolves against them.
///
/// Each send runs on a thread of its own, which tries the hosts in turn and
/// fulfils a `future_default`, so a coordinator can wait on several with a
/// deadline whatever future backend the hosts were built with. The client
/// joins every send thread before it is destroyed.

#include <raft/future_default.hpp>
#include <raft/multi_raft.hpp>
#include <raft/shard_exceptions.hpp>
#include <raft/txn_coordinator.hpp>
#include <raft/txn_state_machine.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace kythira {

template<raft_types Types, shard_key Key, raft_group_id GroupId>
requires is_transactional_state_machine_v<typename Types::state_machine_type>
class multi_raft_txn_client {
public:
    using host_type = multi_raft<Types, Key, GroupId>;
    using group_id_type = GroupId;
    using state_machine_type = typename Types::state_machine_type;
    using inner_type = typename state_machine_type::inner_type;
    /// Reads a key's committed value out of the wrapped state machine. The
    /// value encoding is the application's, so the application supplies it.
    using value_reader =
        std::function<std::optional<std::vector<std::byte>>(const inner_type&, const Key&)>;

    using send_future = future_default<std::vector<std::byte>>;

    multi_raft_txn_client(std::vector<host_type*> hosts, value_reader value_of)
        : _hosts(std::move(hosts)), _value_of(std::move(value_of)) {
        if (_hosts.empty()) {
            throw std::invalid_argument("multi_raft_txn_client: no hosts");
        }
    }

    ~multi_raft_txn_client() {
        const std::lock_guard lock(_senders_mutex);
        for (auto& s : _senders) {
            s._thread.join();
        }
    }

    multi_raft_txn_client(const multi_raft_txn_client&) = delete;
    auto operator=(const multi_raft_txn_client&) -> multi_raft_txn_client& = delete;
    multi_raft_txn_client(multi_raft_txn_client&&) = delete;
    auto operator=(multi_raft_txn_client&&) -> multi_raft_txn_client& = delete;

    [[nodiscard]] auto resolve(const Key& key) -> std::optional<GroupId> {
        for (auto* h : _hosts) {
            if (auto desc = h->resolve(key); desc.has_value()) {
                return desc->_group_id;
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto send(const std::vector<Key>& keys, const std::vector<std::byte>& envelope,
                            std::chrono::milliseconds timeout) -> send_future {
        promise_default<std::vector<std::byte>> promise;
        auto result = promise.getFuture();
        const std::lock_guard lock(_senders_mutex);
        reap_finished();
        auto& s = _senders.emplace_back();
        s._done = std::make_shared<std::atomic<bool>>(false);
        s._thread = std::thread([this, keys, envelope, timeout, promise = std::move(promise),
                                 done = s._done]() mutable {
            try {
                promise.setValue(send_now(keys, envelope, timeout));
            } catch (...) {
                promise.setException(std::current_exception());
            }
            done->store(true);
        });
        return result;
    }

    [[nodiscard]] auto refresh(std::exception_ptr error) -> bool {
        try {
            std::rethrow_exception(std::move(error));
        } catch (const shard_epoch_mismatch_exception<GroupId, Key, node_id_type>& e) {
            for (auto* h : _hosts) {
                std::ignore = h->learn_descriptors(e.current_descriptors());
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] auto read(const Key& key, std::chrono::milliseconds timeout)
        -> txn_read_result<Key> {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        do {
            if (auto* node = leader_node_for(key); node != nullptr) {
                return node->with_state_machine([&](state_machine_type& sm) {
                    return txn_read_result<Key>{_value_of(sm.inner(), key), sm.intent_on(key)};
                });
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } while (std::chrono::steady_clock::now() < deadline);
        throw std::runtime_error("multi_raft_txn_client: no leader for the key's shard");
    }

    [[nodiscard]] auto now_ms() const -> std::uint64_t {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::steady_clock::now().time_since_epoch())
                                              .count());
    }

private:
    using node_id_type = typename Types::node_id_type;
    using not_leader = shard_not_leader_exception<GroupId, node_id_type>;
    using no_replica = unknown_shard_exception<GroupId>;

    auto leader_node_for(const Key& key) -> typename host_type::group_node_type* {
        for (auto* h : _hosts) {
            const auto desc = h->resolve(key);
            if (!desc.has_value()) {
                continue;
            }
            auto* node = h->group_node(desc->_group_id);
            if (node != nullptr && node->is_leader()) {
                return node;
            }
        }
        return nullptr;
    }

    /// Try each host in turn until one leads the group, then wait for its
    /// answer. A not-leader or no-replica refusal moves on; anything else,
    /// a routing refusal included, is the caller's.
    auto send_now(const std::vector<Key>& keys, const std::vector<std::byte>& envelope,
                  std::chrono::milliseconds timeout) -> std::vector<std::byte> {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::exception_ptr last;
        do {
            for (auto* h : _hosts) {
                auto f = h->submit_txn(keys, envelope, timeout);
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now());
                if (!f.wait(std::max(left, std::chrono::milliseconds{1}))) {
                    throw std::runtime_error("multi_raft_txn_client: send timed out");
                }
                try {
                    return std::move(f).get();
                } catch (const not_leader&) {
                    last = std::current_exception();
                } catch (const no_replica&) {
                    last = std::current_exception();
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        } while (std::chrono::steady_clock::now() < deadline);
        std::rethrow_exception(last);
    }

    struct sender {
        std::thread _thread;
        std::shared_ptr<std::atomic<bool>> _done;
    };

    /// Join the send threads that have finished. Caller holds the mutex.
    auto reap_finished() -> void {
        for (auto it = _senders.begin(); it != _senders.end();) {
            if (it->_done->load()) {
                it->_thread.join();
                it = _senders.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::vector<host_type*> _hosts;
    value_reader _value_of;
    std::mutex _senders_mutex;
    std::list<sender> _senders;
};

}  // namespace kythira
