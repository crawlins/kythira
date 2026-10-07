// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file capacity_plane.hpp
/// @brief An out-of-process control plane for elastic shard capacity, so
///        `multi_raft_node` containers can run the end-to-end scenario of
///        `.kiro/specs/elastic-shard-capacity/` task 16.
///
/// `include/raft/elastic_shard_placement_driver.hpp` leaves the RPC between a
/// host and its control plane to the application, because the hooks are
/// `std::function`. This file is that application-side RPC for this binary,
/// and only for it: a small JSON-over-HTTP protocol on its own port.
///
/// - **`controller` role.** One process owns the controller: a
///   `docker_quorum_manager`, a `threshold_capacity_policy`, a memory ledger,
///   a `single_process_capacity_lease`, and the decorator over a no-op driver
///   that also allocates shard ids. Its own host's hooks call the decorator
///   directly. Every other host's hooks reach it over `/capacity/*`.
/// - **`member` role.** The host's hooks are HTTP calls to the controller. A
///   call that fails costs that heartbeat's operators and nothing else, which
///   is the same degradation the decorator gives an in-process controller
///   that throws.
///
/// The lease is the single-process one, and that is honest here rather than a
/// shortcut: exactly one process is configured as the controller, so nothing
/// else could provision. A deployment with several candidate controllers uses
/// `raft_leadership_lease` instead.
///
/// **The plane can resize the cluster**, so it listens on `--bind` like every
/// other surface this host serves, and when a shared token is configured
/// (`node_options::_capacity_token`) every request must carry it as
/// `Authorization: Bearer <token>`. `main.cpp` refuses to start a controller
/// on a non-loopback bind without one.

#include "config.hpp"

#include <raft/capacity_lease.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/capacity_policy.hpp>
#include <raft/console_logger.hpp>
#include <raft/docker_quorum_manager.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/elastic_shard_placement_driver.hpp>
#include <raft/metrics.hpp>
#include <raft/shard_placement_driver.hpp>
#include <raft/shard_types.hpp>
#include <raft/split_merge_policy.hpp>

#include <boost/json.hpp>
#include <httplib.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::bench::capacity {

using node_id = std::uint64_t;
using group_id = std::uint64_t;
using key_type = std::string;
using descriptor = shard_descriptor<group_id, key_type, node_id>;
using report = shard_report<group_id, key_type, node_id>;
using node_rep = node_report<node_id>;
using operation = shard_operation<group_id, key_type, node_id>;
using allocation = shard_id_allocation<group_id, node_id>;

namespace json = boost::json;

/// @brief Equality that takes the same time wherever the inputs differ, so a
///        client cannot learn the token a byte at a time from response
///        latency. Only the length leaks, and the token's length is not the
///        secret. Written out rather than taken from OpenSSL because this
///        binary may be built without it.
[[nodiscard]] inline auto constant_time_equals(std::string_view a, std::string_view b) -> bool {
    if (a.size() != b.size()) {
        return false;
    }
    volatile unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff = diff | static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// The wire codec
// ─────────────────────────────────────────────────────────────────────────────

namespace wire {

inline auto ids(const std::vector<node_id>& v) -> json::array {
    json::array a;
    for (const auto& n : v) {
        a.emplace_back(n);
    }
    return a;
}

inline auto ids_from(const json::value& v) -> std::vector<node_id> {
    std::vector<node_id> out;
    for (const auto& n : v.as_array()) {
        out.push_back(json::value_to<node_id>(n));
    }
    return out;
}

inline auto u64(const json::object& o, std::string_view k) -> std::uint64_t {
    return json::value_to<std::uint64_t>(o.at(k));
}

inline auto num(const json::object& o, std::string_view k) -> double {
    return json::value_to<double>(o.at(k));
}

inline auto encode(const descriptor& d) -> json::object {
    json::object o;
    o["g"] = d._group_id;
    o["lo"] = d._range._start ? json::value(*d._range._start) : json::value(nullptr);
    o["hi"] = d._range._end ? json::value(*d._range._end) : json::value(nullptr);
    o["ev"] = d._epoch._version;
    o["ecv"] = d._epoch._conf_version;
    o["voters"] = ids(d._voters);
    o["learners"] = ids(d._learners);
    o["hint"] = d._leader_hint ? json::value(*d._leader_hint) : json::value(nullptr);
    return o;
}

inline auto decode_descriptor(const json::value& v) -> descriptor {
    const auto& o = v.as_object();
    descriptor d;
    d._group_id = u64(o, "g");
    if (!o.at("lo").is_null()) {
        d._range._start = std::string(o.at("lo").as_string());
    }
    if (!o.at("hi").is_null()) {
        d._range._end = std::string(o.at("hi").as_string());
    }
    d._epoch._version = u64(o, "ev");
    d._epoch._conf_version = u64(o, "ecv");
    d._voters = ids_from(o.at("voters"));
    d._learners = ids_from(o.at("learners"));
    if (!o.at("hint").is_null()) {
        d._leader_hint = json::value_to<node_id>(o.at("hint"));
    }
    return d;
}

inline auto encode(const report& r) -> json::object {
    json::object o;
    o["d"] = encode(r._descriptor);
    o["leader"] = r._leader;
    o["down"] = ids(r._down_replicas);
    o["pending"] = ids(r._pending_replicas);
    o["bytes"] = r._approximate_size_bytes;
    o["keys"] = r._approximate_key_count;
    o["sized"] = r._size_available;
    o["rq"] = r._read_qps;
    o["wq"] = r._write_qps;
    o["rb"] = r._read_bytes_per_sec;
    o["wb"] = r._write_bytes_per_sec;
    o["op"] = static_cast<std::uint64_t>(r._operation);
    o["refusals"] = r._capacity_refusals;
    o["term"] = r._term;
    return o;
}

inline auto decode_report(const json::value& v) -> report {
    const auto& o = v.as_object();
    report r;
    r._descriptor = decode_descriptor(o.at("d"));
    r._leader = u64(o, "leader");
    r._down_replicas = ids_from(o.at("down"));
    r._down_replica_count = r._down_replicas.size();
    r._pending_replicas = ids_from(o.at("pending"));
    r._approximate_size_bytes = u64(o, "bytes");
    r._approximate_key_count = u64(o, "keys");
    r._size_available = o.at("sized").as_bool();
    r._read_qps = num(o, "rq");
    r._write_qps = num(o, "wq");
    r._read_bytes_per_sec = num(o, "rb");
    r._write_bytes_per_sec = num(o, "wb");
    r._operation = static_cast<shard_operation_state>(u64(o, "op"));
    r._capacity_refusals = u64(o, "refusals");
    r._term = u64(o, "term");
    return r;
}

inline auto encode(const node_rep& r) -> json::object {
    json::object o;
    o["id"] = r._node_id;
    o["cap"] = r._capacity_bytes;
    o["avail"] = r._available_bytes;
    o["used"] = r._used_bytes;
    o["shards"] = r._shard_count;
    o["leaders"] = r._leader_count;
    o["rq"] = r._read_qps;
    o["wq"] = r._write_qps;
    o["rb"] = r._read_bytes_per_sec;
    o["wb"] = r._write_bytes_per_sec;
    o["snd"] = r._sending_snapshot_count;
    o["rcv"] = r._receiving_snapshot_count;
    o["app"] = r._applying_snapshot_count;
    o["over"] = r._overloaded;
    json::array labels;
    for (const auto& l : r._labels) {
        labels.emplace_back(l);
    }
    o["labels"] = labels;
    o["uptime_ms"] = static_cast<std::int64_t>(r._uptime.count());
    return o;
}

inline auto decode_node(const json::value& v) -> node_rep {
    const auto& o = v.as_object();
    node_rep r;
    r._node_id = u64(o, "id");
    r._capacity_bytes = u64(o, "cap");
    r._available_bytes = u64(o, "avail");
    r._used_bytes = u64(o, "used");
    r._shard_count = u64(o, "shards");
    r._leader_count = u64(o, "leaders");
    r._read_qps = num(o, "rq");
    r._write_qps = num(o, "wq");
    r._read_bytes_per_sec = num(o, "rb");
    r._write_bytes_per_sec = num(o, "wb");
    r._sending_snapshot_count = u64(o, "snd");
    r._receiving_snapshot_count = u64(o, "rcv");
    r._applying_snapshot_count = u64(o, "app");
    r._overloaded = o.at("over").as_bool();
    for (const auto& l : o.at("labels").as_array()) {
        r._labels.emplace_back(l.as_string());
    }
    r._uptime = std::chrono::milliseconds{json::value_to<std::int64_t>(o.at("uptime_ms"))};
    return r;
}

/// The controller emits only membership and leadership operators; anything
/// else is refused on encode rather than guessed at on decode.
inline auto encode(const operation& op) -> json::object {
    json::object o;
    o["g"] = op._group_id;
    o["id"] = op._operation_id;
    o["ev"] = op._epoch._version;
    o["ecv"] = op._epoch._conf_version;
    std::visit(
        [&]<typename Op>(const Op& k) {
            if constexpr (std::same_as<Op, add_replica_operator<node_id>>) {
                o["kind"] = "add";
                o["node"] = k._node;
                o["learner"] = k._as_learner;
            } else if constexpr (std::same_as<Op, remove_replica_operator<node_id>>) {
                o["kind"] = "remove";
                o["node"] = k._node;
            } else if constexpr (std::same_as<Op, transfer_leader_operator<node_id>>) {
                o["kind"] = "transfer";
                o["node"] = k._to;
            } else {
                throw std::invalid_argument("capacity wire: unsupported operator");
            }
        },
        op._operator);
    return o;
}

inline auto decode_operation(const json::value& v) -> operation {
    const auto& o = v.as_object();
    operation op;
    op._group_id = u64(o, "g");
    op._operation_id = u64(o, "id");
    op._epoch._version = u64(o, "ev");
    op._epoch._conf_version = u64(o, "ecv");
    const auto kind = std::string(o.at("kind").as_string());
    const auto node = u64(o, "node");
    if (kind == "add") {
        op._operator =
            add_replica_operator<node_id>{._node = node, ._as_learner = o.at("learner").as_bool()};
    } else if (kind == "remove") {
        op._operator = remove_replica_operator<node_id>{._node = node};
    } else if (kind == "transfer") {
        op._operator = transfer_leader_operator<node_id>{._to = node};
    } else {
        throw std::invalid_argument("capacity wire: unknown operator " + kind);
    }
    return op;
}

inline auto encode(const operator_outcome& r) -> json::object {
    json::object o;
    o["id"] = r._operation_id;
    o["ok"] = r._accepted;
    o["reason"] = static_cast<std::uint64_t>(r._reason);
    return o;
}

inline auto decode_outcome(const json::value& v) -> operator_outcome {
    const auto& o = v.as_object();
    return operator_outcome{._operation_id = u64(o, "id"),
                            ._accepted = o.at("ok").as_bool(),
                            ._reason = static_cast<skipped_operator_reason>(u64(o, "reason"))};
}

template<typename T, typename F> auto array_of(const std::vector<T>& v, F f) -> json::array {
    json::array a;
    for (const auto& x : v) {
        a.emplace_back(f(x));
    }
    return a;
}

template<typename F> auto vector_of(const json::value& v, F f) {
    std::vector<decltype(f(v))> out;
    for (const auto& x : v.as_array()) {
        out.push_back(f(x));
    }
    return out;
}

}  // namespace wire

// ─────────────────────────────────────────────────────────────────────────────
// The provider-call executor
// ─────────────────────────────────────────────────────────────────────────────

/// @brief One thread that runs the controller's provider work in order.
///
/// The controller hands it only the *start* of each provider call; a
/// `docker_quorum_manager` call is a few local HTTP requests to the daemon, so
/// one thread is plenty, and a single thread keeps provider calls ordered.
class work_queue {
public:
    work_queue() : _thread([this] { run(); }) {}
    ~work_queue() {
        {
            std::lock_guard lock(_m);
            _stop = true;
        }
        _cv.notify_all();
        if (_thread.joinable()) {
            _thread.join();
        }
    }
    work_queue(const work_queue&) = delete;
    auto operator=(const work_queue&) -> work_queue& = delete;

    auto fn() -> std::function<void(std::function<void()>)> {
        return [this](std::function<void()> w) {
            {
                std::lock_guard lock(_m);
                _q.push_back(std::move(w));
            }
            _cv.notify_one();
        };
    }

private:
    auto run() -> void {
        for (;;) {
            std::function<void()> w;
            {
                std::unique_lock lock(_m);
                _cv.wait(lock, [this] { return _stop || !_q.empty(); });
                if (_stop && _q.empty()) {
                    return;
                }
                w = std::move(_q.front());
                _q.pop_front();
            }
            try {
                w();
            } catch (const std::exception& e) {
                std::cerr << "capacity: provider work threw: " << e.what() << std::endl;
            }
        }
    }

    std::mutex _m;
    std::condition_variable _cv;
    std::deque<std::function<void()>> _q;
    bool _stop{false};
    std::thread _thread;
};

// ─────────────────────────────────────────────────────────────────────────────
// The controller side
// ─────────────────────────────────────────────────────────────────────────────

/// @brief The controller, its decorator, and the HTTP surface members call.
class capacity_service {
public:
    using qm_type = docker_quorum_manager<node_id, std::string>;
    using policy_type = threshold_capacity_policy<node_id, group_id, key_type>;
    using ledger_type = memory_capacity_ledger<node_id, std::string>;
    using lease_type = single_process_capacity_lease;
    using controller_type =
        elastic_capacity_controller<qm_type, policy_type, ledger_type, lease_type, group_id,
                                    key_type, noop_metrics, console_logger>;
    using inner_type = no_op_shard_placement_driver<group_id, key_type, node_id>;
    using adapter_type =
        elastic_shard_placement_driver<inner_type, controller_type, group_id, key_type, node_id>;

    /// Split children take ids from here up; the pre-split groups are 1..N.
    static constexpr group_id k_first_split_id = 1000;

    explicit capacity_service(const node_options& opt)
        : _controller(
              qm_type{manager_config(opt)}, policy_type{policy_config(opt)}, _ledger, lease_type{1},
              controller_config(opt), _work.fn(), [] { return std::chrono::system_clock::now(); },
              noop_metrics{}, console_logger{log_level::info}),
          _adapter(_controller, group_id{k_first_split_id}, group_id{std::uint64_t{1} << 62U}),
          _bind(opt._bind_address),
          _port(opt._capacity_port),
          _token(opt._capacity_token) {}

    ~capacity_service() { stop(); }
    capacity_service(const capacity_service&) = delete;
    auto operator=(const capacity_service&) -> capacity_service& = delete;

    // ── the hooks, in process ────────────────────────────────────────────────

    auto allocate(std::size_t n) -> std::vector<allocation> {
        return _adapter.allocate_shard_ids(n).get();
    }
    auto shards(const std::vector<report>& r) -> std::vector<operation> {
        return _adapter.report_shard_heartbeat(r).get();
    }
    auto node(const node_rep& r) -> void { _adapter.report_node_heartbeat(r).get(); }
    auto outcomes(const std::vector<operator_outcome>& o) -> void {
        _adapter.report_operator_outcomes(o);
    }
    auto split(const descriptor& parent, const std::vector<descriptor>& children) -> void {
        _adapter.report_split(parent, children).get();
    }
    auto merge(const descriptor& source, const descriptor& target) -> void {
        _adapter.report_merge(source, target).get();
    }
    auto lookup(const group_id& g) -> std::optional<descriptor> {
        return _adapter.lookup_descriptor(g);
    }

    // ── the HTTP surface ─────────────────────────────────────────────────────

    auto start() -> void {
        // Before routing, so no handler (and no JSON parser) ever sees an
        // unauthenticated body, and an unknown path is refused the same way
        // a known one is rather than revealing which paths exist.
        _server.set_pre_routing_handler([this](const auto& req, auto& res) {
            if (_token.empty() ||
                constant_time_equals(req.get_header_value("Authorization"), "Bearer " + _token)) {
                return httplib::Server::HandlerResponse::Unhandled;
            }
            res.status = 401;
            res.set_header("WWW-Authenticate", "Bearer");
            res.set_content("capacity: missing or wrong bearer token", "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        });
        _server.Post("/capacity/allocate", [this](const auto& req, auto& res) {
            serve(res, [&] {
                const auto n = json::value_to<std::size_t>(json::parse(req.body));
                return json::value(wire::array_of(
                    allocate(n), [](const allocation& a) { return json::value(a._group_id); }));
            });
        });
        _server.Post("/capacity/shards", [this](const auto& req, auto& res) {
            serve(res, [&] {
                const auto ops =
                    shards(wire::vector_of(json::parse(req.body), wire::decode_report));
                return json::value(
                    wire::array_of(ops, [](const operation& op) { return wire::encode(op); }));
            });
        });
        _server.Post("/capacity/node", [this](const auto& req, auto& res) {
            serve(res, [&] {
                node(wire::decode_node(json::parse(req.body)));
                return json::value(nullptr);
            });
        });
        _server.Post("/capacity/outcomes", [this](const auto& req, auto& res) {
            serve(res, [&] {
                outcomes(wire::vector_of(json::parse(req.body), wire::decode_outcome));
                return json::value(nullptr);
            });
        });
        _server.Post("/capacity/split", [this](const auto& req, auto& res) {
            serve(res, [&] {
                const auto body = json::parse(req.body).as_object();
                split(wire::decode_descriptor(body.at("parent")),
                      wire::vector_of(body.at("children"), wire::decode_descriptor));
                return json::value(nullptr);
            });
        });
        _server.Post("/capacity/merge", [this](const auto& req, auto& res) {
            serve(res, [&] {
                const auto body = json::parse(req.body).as_object();
                merge(wire::decode_descriptor(body.at("source")),
                      wire::decode_descriptor(body.at("target")));
                return json::value(nullptr);
            });
        });
        _server.Get("/capacity/descriptor", [this](const auto& req, auto& res) {
            serve(res, [&] {
                const auto d = lookup(std::stoull(req.get_param_value("group")));
                return d ? json::value(wire::encode(*d)) : json::value(nullptr);
            });
        });
        _server.Get("/capacity/status", [this](const auto&, auto& res) {
            serve(res, [&] { return json::value(status()); });
        });
        if (!_server.bind_to_port(_bind, _port)) {
            throw std::runtime_error("capacity: cannot bind the control plane to " + _bind + ":" +
                                     std::to_string(_port));
        }
        _thread = std::thread([this] { _server.listen_after_bind(); });
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

    /// @brief What the scenario test reads: the controller's own view.
    [[nodiscard]] auto status() -> json::object {
        const auto s = _controller.snapshot();
        json::object o;
        o["cluster_size"] = s.cluster_size();
        json::array nodes;
        for (const auto& n : s._nodes) {
            json::object v;
            v["id"] = n.node_id();
            v["stale"] = n._stale;
            v["age_ms"] = static_cast<std::int64_t>(n._age.count());
            v["shards"] = n._report._shard_count;
            nodes.emplace_back(std::move(v));
        }
        o["nodes"] = std::move(nodes);
        json::array unreachable;
        if (s._health) {
            for (const auto& n : s._health->unreachable_nodes) {
                unreachable.emplace_back(n);
            }
        }
        o["unreachable"] = std::move(unreachable);
        o["shards"] = wire::array_of(s._shards, [](const report& r) {
            json::object v = wire::encode(r._descriptor);
            v["leader"] = r._leader;
            v["pending"] = wire::ids(r._pending_replicas);
            return v;
        });
        json::array intents;
        for (const auto& i : _ledger.intents()) {
            json::object v;
            v["key"] = i._key;
            v["kind"] = to_string(i._kind);
            v["state"] = to_string(i._state);
            v["node"] = i._node ? json::value(*i._node) : json::value(nullptr);
            v["note"] = i._note;
            intents.emplace_back(std::move(v));
        }
        o["intents"] = std::move(intents);
        json::object counters;
        for (const auto& [k, v] : _controller.counters()) {
            counters[k] = v;
        }
        o["counters"] = std::move(counters);
        return o;
    }

private:
    template<typename F> static auto serve(httplib::Response& res, F f) -> void {
        try {
            res.set_content(json::serialize(f()), "application/json");
        } catch (const std::exception& e) {
            res.status = 500;
            res.set_content(e.what(), "text/plain");
        }
    }

    static auto manager_config(const node_options& opt) -> docker_quorum_manager_config {
        docker_quorum_manager_config c;
        c.daemon_url = opt._capacity_docker_url;
        c.image = opt._capacity_image;
        c.cluster_name = opt._capacity_cluster;
        c.network_name = opt._capacity_network;
        c.node_port = opt._raft_port;
        c.group_id = "default";
        c.target_count = opt._voters.size();
        c.extra_env = opt._capacity_join_env;
        // A machine this controller creates is a member, and a member of an
        // authenticated plane needs the token. Passed here rather than as a
        // `--capacity-join-env`, which would put it on this process's
        // command line.
        if (!opt._capacity_token.empty()) {
            c.extra_env.push_back(std::string(k_capacity_token_env) + "=" + opt._capacity_token);
        }
        return c;
    }

    static auto policy_config(const node_options& opt) -> threshold_capacity_policy_config {
        threshold_capacity_policy_config p;
        p._enabled = true;
        p._shards_per_node = {._high = opt._capacity_shards_high, ._low = opt._capacity_shards_low};
        // One signal drives the scenario; the others stay out of its way.
        p._storage._enabled = false;
        p._leaders_per_node._enabled = false;
        p._overloaded._enabled = false;
        p._split_pressure_enabled = false;
        p._sustained_for = opt._capacity_sustained;
        p._cooldown = opt._capacity_sustained;
        return p;
    }

    static auto controller_config(const node_options& opt) -> elastic_capacity_config {
        using namespace std::chrono_literals;
        elastic_capacity_config c;
        c.enabled = true;
        c.dry_run = opt._capacity_dry_run;
        c.min_cluster_size = opt._voters.size();
        c.max_cluster_size = opt._capacity_max_nodes;
        c.evaluation_interval =
            std::max(opt._capacity_heartbeat * 2, std::chrono::milliseconds{1000});
        c.min_provider_call_interval = opt._capacity_sustained;
        c.node_report_staleness = opt._capacity_heartbeat * 5;
        c.assess_interval = opt._capacity_heartbeat * 5;
        c.operator_retry_interval = opt._capacity_heartbeat * 5;
        c.operator_busy_backoff = opt._capacity_heartbeat * 5;
        c.move_cooldown_after_split = opt._capacity_heartbeat * 5;
        c.reconcile_deadline = opt._capacity_heartbeat * 10;
        return c;
    }

    work_queue _work;
    ledger_type _ledger;
    controller_type _controller;
    adapter_type _adapter;
    std::string _bind;
    std::uint16_t _port;
    std::string _token;
    httplib::Server _server;
    std::thread _thread;
    std::atomic<bool> _stopped{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// The member side
// ─────────────────────────────────────────────────────────────────────────────

/// @brief A host's hooks as calls to the controller's control plane.
///
/// Every call has a bounded timeout and every failure has a defined answer —
/// no operators, no ids, no descriptor — so an unreachable controller makes
/// this host a static one, never a stuck one.
class capacity_client {
public:
    capacity_client(const std::string& controller, const std::string& token)
        : _client("http://" + controller) {
        if (!token.empty()) {
            _client.set_bearer_token_auth(token);
        }
        _client.set_connection_timeout(std::chrono::seconds{2});
        _client.set_read_timeout(std::chrono::seconds{5});
        _client.set_tcp_nodelay(true);
    }

    auto allocate(std::size_t n) -> std::vector<allocation> {
        auto v = post("/capacity/allocate", json::value(n));
        if (!v) {
            throw std::runtime_error("capacity: the controller allocated no ids");
        }
        return wire::vector_of(*v, [](const json::value& x) {
            return allocation{._group_id = json::value_to<group_id>(x)};
        });
    }
    auto shards(const std::vector<report>& r) -> std::vector<operation> {
        auto v =
            post("/capacity/shards",
                 json::value(wire::array_of(r, [](const report& x) { return wire::encode(x); })));
        if (!v) {
            return {};
        }
        return wire::vector_of(*v, wire::decode_operation);
    }
    auto node(const node_rep& r) -> void { post("/capacity/node", json::value(wire::encode(r))); }
    auto outcomes(const std::vector<operator_outcome>& o) -> void {
        post("/capacity/outcomes", json::value(wire::array_of(o, [](const operator_outcome& x) {
                 return wire::encode(x);
             })));
    }
    auto split(const descriptor& parent, const std::vector<descriptor>& children) -> void {
        json::object body;
        body["parent"] = wire::encode(parent);
        body["children"] =
            wire::array_of(children, [](const descriptor& d) { return wire::encode(d); });
        post("/capacity/split", json::value(std::move(body)));
    }
    auto merge(const descriptor& source, const descriptor& target) -> void {
        json::object body;
        body["source"] = wire::encode(source);
        body["target"] = wire::encode(target);
        post("/capacity/merge", json::value(std::move(body)));
    }
    auto lookup(const group_id& g) -> std::optional<descriptor> {
        std::lock_guard lock(_m);
        auto res = _client.Get("/capacity/descriptor?group=" + std::to_string(g));
        if (!res || res->status != 200) {
            return std::nullopt;
        }
        try {
            const auto v = json::parse(res->body);
            return v.is_null() ? std::nullopt : std::optional{wire::decode_descriptor(v)};
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

private:
    auto post(const std::string& path, const json::value& body) -> std::optional<json::value> {
        std::lock_guard lock(_m);
        auto res = _client.Post(path, json::serialize(body), "application/json");
        if (!res || res->status != 200) {
            return std::nullopt;
        }
        try {
            return json::parse(res->body);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

    std::mutex _m;
    httplib::Client _client;
};

// ─────────────────────────────────────────────────────────────────────────────
// Wiring
// ─────────────────────────────────────────────────────────────────────────────

/// @brief Points a host's placement-driver hooks at `plane` and turns on what
///        the scenario needs: a heartbeat, node labels, and — when
///        `--split-keys` is set — a key-count split policy.
template<typename Config, typename Plane>
auto wire_hooks(Config& cfg, const node_options& opt, Plane& plane) -> void {
    cfg.allocate_shard_ids = [&plane](std::size_t n) { return plane.allocate(n); };
    cfg.report_shard_heartbeat = [&plane](const std::vector<report>& r) { return plane.shards(r); };
    cfg.report_node_heartbeat = [&plane](const node_rep& r) { plane.node(r); };
    cfg.report_operator_outcomes = [&plane](const std::vector<operator_outcome>& o) {
        plane.outcomes(o);
    };
    cfg.report_split = [&plane](const descriptor& p, const std::vector<descriptor>& c) {
        plane.split(p, c);
    };
    cfg.report_merge = [&plane](const descriptor& s, const descriptor& t) { plane.merge(s, t); };
    cfg.lookup_descriptor = [&plane](const group_id& g) { return plane.lookup(g); };
    cfg.heartbeat_interval = opt._capacity_heartbeat;
    cfg.node_labels = {"default"};

    if (opt._split_keys > 0) {
        threshold_split_merge_policy_config s;
        // Only the key count may trigger: the byte thresholds are out of reach
        // and the merge floor is the smallest valid one.
        s._shard_split_keys = opt._split_keys;
        s._shard_max_keys = opt._split_keys * 2;
        s._shard_merge_max_keys = 1;
        s._shard_split_size_bytes = std::size_t{1} << 40U;
        s._shard_max_size_bytes = std::size_t{1} << 41U;
        s._shard_merge_max_size_bytes = 1;
        auto policy = std::make_shared<threshold_split_merge_policy<group_id, key_type>>(s);
        if (!policy->validate()) {
            throw std::runtime_error("multi_raft_node: --split-keys gives an invalid split policy");
        }
        cfg.evaluate_split = [policy](const auto& stats) { return policy->evaluate_split(stats); };
        cfg.automatic_split_merge_enabled = true;
        cfg.policy_interval = std::chrono::milliseconds{500};
        cfg.split_merge_interval = std::chrono::seconds{2};
    }
}

}  // namespace kythira::bench::capacity
