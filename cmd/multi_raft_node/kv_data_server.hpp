// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file kv_data_server.hpp
/// @brief The host side of the client-facing data path.
///        `.kiro/specs/multi-raft-host-binary/` Requirement 2.
///
/// One `httplib::Server` on its own port, serving three routes and nothing
/// else. The control surface — health, status, the shard map — is a **second**
/// server on a second port (`control_server.hpp`), because Requirement 2.4
/// asks the two be kept apart and a path prefix is not apart enough: anything
/// counting bytes or connections on the data port would still see control
/// traffic. Two sockets cannot be confused.
///
/// The host never parses a command. The body of a put or a delete is forwarded
/// to `submit_command` exactly as it arrived, which is what makes a Tier C row
/// carry the same workload as a Tier B one rather than a re-encoded lookalike.

#include "../multi_raft_bench_common/kv_data_path.hpp"

#include <raft/shard_exceptions.hpp>
#include <raft/shard_types.hpp>

#include <httplib.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace kythira::bench {

/// @brief Serves put / get / delete against one `multi_raft` host.
///
/// @tparam Host `kythira::multi_raft<Types, std::string, std::uint64_t>`.
template<typename Host> class kv_data_server {
public:
    using host_type = Host;

    kv_data_server(host_type& host, std::string bind_address, std::uint16_t port,
                   std::string media_type, std::chrono::milliseconds op_timeout,
                   std::size_t threads = 0)
        : _host(host),
          _bind(std::move(bind_address)),
          _port(port),
          _media_type(std::move(media_type)),
          _op_timeout(op_timeout),
          _threads(threads) {}

    ~kv_data_server() { stop(); }

    kv_data_server(const kv_data_server&) = delete;
    auto operator=(const kv_data_server&) -> kv_data_server& = delete;

    auto start() -> void {
        // See `kv_data_client`'s constructor: without this, Nagle plus the
        // peer's delayed-ACK timer adds 40 ms to a small response.
        _server.set_tcp_nodelay(true);
        // A handler blocks for the whole commit, so this is the ceiling on
        // client operations in flight against this host. See
        // `node_options::_data_threads` for the measurement that made it a knob.
        if (_threads > 0) {
            _server.new_task_queue = [n = _threads] { return new httplib::ThreadPool(n); };
        }
        _server.Post(std::string(data_path_routes::k_put),
                     [this](const httplib::Request& req, httplib::Response& res) {
                         handle_write(req, res);
                     });
        _server.Post(std::string(data_path_routes::k_delete),
                     [this](const httplib::Request& req, httplib::Response& res) {
                         handle_write(req, res);
                     });
        _server.Post(
            std::string(data_path_routes::k_get),
            [this](const httplib::Request& req, httplib::Response& res) { handle_read(req, res); });
        _thread = std::thread([this] { _server.listen(_bind.c_str(), _port); });
        // `listen` binds on the new thread, so a client that connects the
        // instant this returns would otherwise race the bind. Waiting on the
        // library's own readiness flag is cheaper and more honest than a sleep.
        _server.wait_until_ready();
    }

    auto stop() -> void {
        if (_stopped.exchange(true)) {
            return;
        }
        _server.stop();
        if (_thread.joinable()) {
            _thread.join();
        }
    }

    [[nodiscard]] auto port() const -> std::uint16_t { return _port; }

private:
    /// Put and delete are the same shape: forward the body, wait for the
    /// commit, answer. They differ only in the command the client built, which
    /// this host does not look at.
    auto handle_write(const httplib::Request& req, httplib::Response& res) -> void {
        const auto key = req.get_param_value("key");
        if (key.empty()) {
            res.status = data_path_status::k_bad_request;
            return;
        }
        annotate_routing(key, res);

        try {
            auto future = _host.submit_command(key, to_bytes(req.body), _op_timeout);
            if (!future.wait(_op_timeout)) {
                res.status = data_path_status::k_unavailable;
                return;
            }
            auto value = std::move(future).get();
            res.status = data_path_status::k_ok;
            res.set_content(to_string_body(value), _media_type);
        } catch (const kythira::shard_not_leader_exception<std::uint64_t, std::uint64_t>& e) {
            misdirect(res, e.leader_hint());
        } catch (const std::exception&) {
            // Anything else — an epoch mismatch, a merging shard, a transport
            // failure inside the cluster — is reported as unavailable rather
            // than as a leader hint the client would act on. Guessing a leader
            // here would send the driver somewhere on no evidence.
            res.status = data_path_status::k_unavailable;
        }
    }

    auto handle_read(const httplib::Request& req, httplib::Response& res) -> void {
        const auto key = req.get_param_value("key");
        const auto kind = parse_read_kind(req.get_param_value("read"));
        if (!kind.has_value()) {
            res.status = data_path_status::k_bad_request;
            return;
        }
        if (key.empty()) {
            res.status = data_path_status::k_bad_request;
            return;
        }
        annotate_routing(key, res);

        switch (*kind) {
            case wire_read_kind::log:
                handle_write(req, res);
                return;
            case wire_read_kind::state:
                handle_read_state(key, res);
                return;
            case wire_read_kind::local:
                handle_local_read(key, res);
                return;
        }
    }

    auto handle_read_state(const std::string& key, httplib::Response& res) -> void {
        try {
            auto future = _host.read_state(key, _op_timeout);
            if (!future.wait(_op_timeout)) {
                res.status = data_path_status::k_unavailable;
                return;
            }
            auto value = std::move(future).get();
            res.status = data_path_status::k_ok;
            res.set_content(to_string_body(value), _media_type);
        } catch (const kythira::shard_not_leader_exception<std::uint64_t, std::uint64_t>& e) {
            misdirect(res, e.leader_hint());
        } catch (const std::exception&) {
            res.status = data_path_status::k_unavailable;
        }
    }

    /// The non-consensus read, answered wherever the request landed.
    ///
    /// **No leadership check, deliberately.** A local read served only by the
    /// leader would be stale only in theory; served by whichever replica the
    /// client addressed, it is stale the way a deployment would experience it.
    /// Note what it still pays: `with_state_machine` takes the group node's own
    /// mutex, so skipping consensus does not skip the per-group serialisation.
    auto handle_local_read(const std::string& key, httplib::Response& res) -> void {
        auto descriptor = _host.resolve(key);
        if (!descriptor.has_value()) {
            res.status = data_path_status::k_unavailable;
            return;
        }
        auto* node = _host.group_node(descriptor->_group_id);
        if (node == nullptr) {
            // The routing table knows the shard but this host holds no replica
            // of it. Not a miss — a misdirection, and the client should ask
            // somebody else.
            misdirect(res, std::nullopt);
            return;
        }
        try {
            auto value = node->with_state_machine([&key](auto& sm) { return sm.get_value(key); });
            if (!value.has_value()) {
                // A miss is a completed read, not a failure: the store is
                // preloaded before the window, so a miss means this replica has
                // not applied that entry yet, which is exactly the staleness
                // this kind exists to expose.
                res.status = data_path_status::k_no_such_key;
                return;
            }
            res.status = data_path_status::k_ok;
            res.set_content(*value, _media_type);
        } catch (const std::exception&) {
            res.status = data_path_status::k_unavailable;
        }
    }

    /// Put the group and, when known, the leader on every answer — including
    /// the successful ones, so a client can keep its cache warm without a
    /// second request.
    auto annotate_routing(const std::string& key, httplib::Response& res) -> void {
        auto descriptor = _host.resolve(key);
        if (!descriptor.has_value()) {
            return;
        }
        res.set_header(std::string(k_group_header), std::to_string(descriptor->_group_id));
        if (auto* node = _host.group_node(descriptor->_group_id); node != nullptr) {
            // A leader's `known_leader()` records who last sent it an
            // AppendEntries, which is somebody else or nobody. Asking the
            // replica whether it leads is the only way to get "me" out of it.
            const auto leader =
                node->is_leader() ? std::optional{_host.node_id()} : node->known_leader();
            if (leader.has_value()) {
                res.set_header(std::string(k_leader_header), std::to_string(*leader));
            }
        }
    }

    /// @brief Answer 421 and name the leader if one is known.
    ///
    /// **Returned, never forwarded** (Requirement 2.2). The host has the
    /// leader's identity and could proxy the request in one hop; doing so would
    /// move the routing cost inside the cluster where no client-side
    /// measurement can see it, and this project prices routing deliberately.
    static auto misdirect(httplib::Response& res, std::optional<std::uint64_t> leader) -> void {
        res.status = data_path_status::k_misdirected;
        if (leader.has_value()) {
            res.set_header(std::string(k_leader_header), std::to_string(*leader));
        }
    }

    host_type& _host;
    std::string _bind;
    std::uint16_t _port;
    std::string _media_type;
    std::chrono::milliseconds _op_timeout;
    std::size_t _threads{0};
    httplib::Server _server;
    std::thread _thread;
    std::atomic<bool> _stopped{false};
};

}  // namespace kythira::bench
