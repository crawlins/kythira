// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_coap_test.cpp
/// @brief `multi_raft` over the libcoap backend, end to end
///        (.kiro/specs/coap-transport-multi-raft/ task 13).
///
/// Every other multi-Raft suite runs on the in-process message fabric, and the
/// CoAP suite runs `node<Types>` one group at a time. This is where the two
/// meet: three hosts, each with **one** `coap_client` and **one** `coap_server`
/// on an ephemeral loopback port, carrying every group's traffic, exactly the
/// shape a deployment has.
///
/// Group routing over CoAP is supposed to need no code — the group id rides in
/// the serialized message and `multi_group_network_server` reads it back out.
/// The cases below are what makes that claim checkable:
///
/// - **Isolation.** Each group's replicas hold exactly the keys in its range,
///   and every group id a host receives over the wire is one the cluster
///   actually created. A serializer that dropped the field would deliver every
///   message to group 0 and fail both.
/// - **Split.** Each child gets its own traffic: after a split, the new
///   group's id shows up on the wire at every follower, and a write on each
///   side of the cut lands in its own child and never in the other.
///   The new group starts from a snapshot, so this is also the first
///   InstallSnapshot sent over the libcoap client by a Raft node.
/// - **Merge.** A merge's two-phase protocol completes over CoAP.
/// - **Scatter.** Needs `TimeoutNow` (task 6); before that the libcoap backend
///   did not have it and `scatter` could not move leadership at all.
///
/// OSCORE is not on here. Per-group Security Contexts (tasks 9–11) exist as
/// `oscore_group_contexts.hpp`, but how they reach the libcoap backend — whose
/// OSCORE is libcoap's own, configured once per context — is an open decision
/// recorded in the spec; this suite gains an OSCORE variant once it is made.

#define BOOST_TEST_MODULE multi_raft_coap_test
#include <boost/test/unit_test.hpp>

#include "coap_node_cluster.hpp"
#include "test_timeout_scale.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_coap_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_range;
using kythira::testing::coap_client_handle;
using kythira::testing::coap_node_transport_types;
using kythira::testing::reserve_udp_port;
using kythira::testing::scaled_deadline;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
using transport_types = coap_node_transport_types<serializer_type>;
using coap_client_type = kythira::coap_client<transport_types>;
using coap_server_type = kythira::coap_server<transport_types>;

/// Every group id one host has received over the wire, by RPC.
class inbound_groups {
public:
    auto record(group_id_type group) -> void {
        std::lock_guard lock(_mutex);
        _seen.insert(group);
    }
    [[nodiscard]] auto seen() const -> std::set<group_id_type> {
        std::lock_guard lock(_mutex);
        return _seen;
    }

private:
    mutable std::mutex _mutex;
    std::set<group_id_type> _seen;
};

/// A `coap_server_handle` that also notes the group id of every request the
/// host's demultiplexer is handed. Recorded *after* decoding, at the one
/// handler per RPC type `multi_group_network_server` installs — so it sees
/// exactly what the CoAP wire delivered, before any group code runs.
class recording_server_handle {
public:
    recording_server_handle(coap_server_type& server, inbound_groups& seen) noexcept
        : _server(&server), _seen(&seen) {}

    auto register_request_vote_handler(
        std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> h)
        -> void {
        _server->register_request_vote_handler(wrap(std::move(h)));
    }
    auto register_append_entries_handler(
        std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)>
            h) -> void {
        _server->register_append_entries_handler(wrap(std::move(h)));
    }
    auto register_install_snapshot_handler(std::function<kythira::install_snapshot_response<>(
                                               const kythira::install_snapshot_request<>&)>
                                               h) -> void {
        _server->register_install_snapshot_handler(wrap(std::move(h)));
    }
    auto register_timeout_now_handler(
        std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<>&)> h)
        -> void {
        _server->register_timeout_now_handler(wrap(std::move(h)));
    }

    auto start() -> void { _server->start(); }
    auto stop() -> void {
        if (_server->is_running()) {
            _server->stop();
        }
    }
    [[nodiscard]] auto is_running() const -> bool { return _server->is_running(); }

private:
    template<typename Response, typename Request>
    auto wrap(std::function<Response(const Request&)> h)
        -> std::function<Response(const Request&)> {
        return [seen = _seen, h = std::move(h)](const Request& req) {
            seen->record(req.group_id());
            return h(req);
        };
    }

    coap_server_type* _server;
    inbound_groups* _seen;
};

static_assert(kythira::network_server_with_timeout_now<recording_server_handle>);
static_assert(kythira::network_client_with_timeout_now<coap_client_handle<coap_client_type>>);

struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = ::serializer_type;

    using network_client_type = coap_client_handle<coap_client_type>;
    using network_server_type = recording_server_handle;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = kythira::test_key_value_state_machine<log_index_type>;

    using configuration_type = kythira::raft_configuration;

    using log_entry_type = kythira::log_entry<term_id_type, log_index_type>;
    using cluster_configuration_type = kythira::cluster_configuration<node_id_type>;
    using snapshot_type = kythira::snapshot<node_id_type, term_id_type, log_index_type>;

    using request_vote_request_type =
        kythira::request_vote_request<node_id_type, term_id_type, log_index_type, group_id_type>;
    using request_vote_response_type = kythira::request_vote_response<term_id_type, group_id_type>;
    using append_entries_request_type =
        kythira::append_entries_request<node_id_type, term_id_type, log_index_type, log_entry_type,
                                        group_id_type>;
    using append_entries_response_type =
        kythira::append_entries_response<term_id_type, log_index_type, group_id_type>;
    using install_snapshot_request_type =
        kythira::install_snapshot_request<node_id_type, term_id_type, log_index_type,
                                          group_id_type>;
    using install_snapshot_response_type =
        kythira::install_snapshot_response<term_id_type, group_id_type>;
};

using host_type = multi_raft<host_types, key_type, group_id_type>;
using config_type = multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

constexpr std::size_t k_node_count = 3;
constexpr group_id_type k_left = 1;
constexpr group_id_type k_right = 2;

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

/// `(-inf, "m")` and `["m", +inf)`: two groups over all three hosts, colocated
/// by construction so a merge's precondition holds from the start.
auto left_descriptor() -> descriptor_type {
    return descriptor_type{._group_id = k_left,
                           ._range = range_of(std::nullopt, key_type{"m"}),
                           ._epoch = shard_epoch{._version = 2, ._conf_version = 0},
                           ._voters = {1, 2, 3},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}
auto right_descriptor() -> descriptor_type {
    return descriptor_type{._group_id = k_right,
                           ._range = range_of(key_type{"m"}, std::nullopt),
                           ._epoch = shard_epoch{._version = 2, ._conf_version = 0},
                           ._voters = {1, 2, 3},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}

auto left_keys() -> std::vector<key_type> {
    return {"alpha", "bravo", "charlie", "delta"};
}
auto right_keys() -> std::vector<key_type> {
    return {"november", "oscar", "papa"};
}
auto all_keys() -> std::vector<key_type> {
    auto out = left_keys();
    for (auto& k : right_keys()) {
        out.push_back(k);
    }
    return out;
}

/// Hands out group ids from one place, as a cluster-scope authority would.
class id_authority {
public:
    auto allocate(std::size_t n) -> std::vector<group_id_type> {
        std::lock_guard lock(_mutex);
        std::vector<group_id_type> out;
        for (std::size_t i = 0; i < n; ++i) {
            out.push_back(_next++);
        }
        return out;
    }

private:
    std::mutex _mutex;
    group_id_type _next{100};
};

/// Three `multi_raft` hosts, one CoAP client and one CoAP server each.
class coap_multi_raft_cluster {
public:
    coap_multi_raft_cluster() {
        std::map<node_id_t, std::string> endpoints;
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            const auto port = reserve_udp_port();
            endpoints.emplace(id, "coap://127.0.0.1:" + std::to_string(port));
            _servers.emplace(
                id, std::make_unique<coap_server_type>(
                        "127.0.0.1", port, kythira::coap_server_config{}, kythira::noop_metrics{}));
            _seen.emplace(id, std::make_unique<inbound_groups>());
        }
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            std::unordered_map<std::uint64_t, std::string> peers;
            for (const auto& [peer, endpoint] : endpoints) {
                if (peer != id) {
                    peers.emplace(peer, endpoint);
                }
            }
            _clients.emplace(
                id, std::make_unique<coap_client_type>(
                        std::move(peers), kythira::coap_client_config{}, kythira::noop_metrics{}));
        }
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            _hosts.back()->create_group(left_descriptor());
            _hosts.back()->create_group(right_descriptor());
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] { drive(i); });
        }
    }

    // Hosts stop before the transports they hold handles to are destroyed;
    // the members below are declared so the maps outlive `_hosts`.
    ~coap_multi_raft_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (auto& h : _hosts) {
            h->stop();
        }
        _hosts.clear();
    }

    coap_multi_raft_cluster(const coap_multi_raft_cluster&) = delete;
    auto operator=(const coap_multi_raft_cluster&) -> coap_multi_raft_cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto inbound(node_id_t id) const -> std::set<group_id_type> {
        return _seen.at(id)->seen();
    }

    [[nodiscard]] auto leader_of(group_id_type group) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    [[nodiscard]] auto leader_id_of(group_id_type group) -> std::optional<node_id_t> {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            auto* n = _hosts.at(id - 1)->group_node(group);
            if (n != nullptr && n->is_leader()) {
                return id;
            }
        }
        return std::nullopt;
    }

    auto await(const std::function<bool()>& predicate, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (predicate()) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return predicate();
    }

    auto await_leaders(std::chrono::milliseconds budget) -> bool {
        return await([&] { return leader_of(k_left) != nullptr && leader_of(k_right) != nullptr; },
                     budget);
    }

    /// @brief Write `key` through whichever host leads the shard owning it.
    auto put(const key_type& key, const std::string& value) -> bool {
        const auto rpc_budget = scaled_deadline(3000);
        const auto deadline = std::chrono::steady_clock::now() + scaled_deadline(15000);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& h : _hosts) {
                auto desc = h->resolve(key);
                if (!desc.has_value()) {
                    continue;
                }
                auto* n = h->group_node(desc->_group_id);
                if (n == nullptr || !n->is_leader()) {
                    continue;
                }
                auto f = h->submit_command(
                    key, host_types::state_machine_type::make_put_command(key, value), rpc_budget);
                if (f.wait(rpc_budget)) {
                    try {
                        std::ignore = std::move(f).get();
                        return true;
                    } catch (...) {
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    /// @brief Which of `candidates` node `node`'s replica of `group` holds.
    [[nodiscard]] auto keys_of(node_id_t node, group_id_type group,
                               const std::vector<key_type>& candidates = all_keys())
        -> std::vector<key_type> {
        auto* n = _hosts.at(node - 1)->group_node(group);
        if (n == nullptr) {
            return {};
        }
        return n->with_state_machine([&](host_types::state_machine_type& sm) {
            std::vector<key_type> out;
            for (const auto& k : candidates) {
                if (sm.contains(k)) {
                    out.push_back(k);
                }
            }
            return out;
        });
    }

    /// @brief Wait until every replica of `group` holds exactly `expected`.
    auto await_contents(group_id_type group, const std::vector<key_type>& expected,
                        std::chrono::milliseconds budget) -> bool {
        return await(
            [&] {
                for (node_id_t id = 1; id <= k_node_count; ++id) {
                    if (keys_of(id, group) != expected) {
                        return false;
                    }
                }
                return true;
            },
            budget);
    }

    [[nodiscard]] auto tiling_problem() -> std::optional<std::string> {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (auto problem = _hosts.at(id - 1)->shard_map_snapshot().check_tiling()) {
                return "node " + std::to_string(id) + ": " + *problem;
            }
        }
        return std::nullopt;
    }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = coap_client_handle<coap_client_type>{*_clients.at(id)},
            .network_server = recording_server_handle{*_servers.at(id), *_seen.at(id)},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        // The same scaled timings the single-group CoAP suite uses, so a slow
        // build keeps heartbeat, election timeout and RPC timeout in ratio.
        cfg.config = kythira::testing::coap_cluster_raft_config();
        // Errors only: at debug, every heartbeat of every group is a line,
        // and the console becomes the slowest thing in the test.
        cfg.logger = host_types::logger_type{kythira::log_level::error};
        cfg.logger_factory = [](const group_id_type&) {
            return host_types::logger_type{kythira::log_level::error};
        };
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.allocate_group_ids = [this](std::size_t n) { return _ids.allocate(n); };
        // No arbiter cooldown: each case runs one operator-initiated split or
        // merge, and the one-hour production default would gate the second.
        cfg.split_merge_interval = std::chrono::milliseconds{0};
        return cfg;
    }

    // Slower than the fabric suites' 5 ms on purpose. `multi_raft::tick()`
    // replicates every group on every tick, acknowledged or not, so the send
    // rate is the tick rate. The libcoap client polls for replies every 5 ms,
    // which puts a CoAP round trip right at a 5 ms tick: replies then trail
    // the next round, each round re-sends what is still unacknowledged, and
    // the backlog feeds itself until a write takes longer than any budget.
    // 20 ms keeps a round trip well inside one tick and a heartbeat (60 ms)
    // still several ticks long.
    static constexpr std::chrono::milliseconds k_tick_interval{20};

    auto drive(std::size_t index) -> void {
        while (_running.load()) {
            _hosts[index]->tick();
            std::this_thread::sleep_for(k_tick_interval);
        }
    }

    id_authority _ids;
    std::map<node_id_t, std::unique_ptr<inbound_groups>> _seen;
    std::map<node_id_t, std::unique_ptr<coap_server_type>> _servers;
    std::map<node_id_t, std::unique_ptr<coap_client_type>> _clients;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
};

template<typename Future>
auto settle(Future&& f, std::chrono::milliseconds budget) -> std::exception_ptr {
    if (!f.wait(budget)) {
        return std::make_exception_ptr(std::runtime_error("settle: future never resolved"));
    }
    try {
        std::ignore = std::forward<Future>(f).get();
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

auto describe(const std::exception_ptr& e) -> std::string {
    if (!e) {
        return "ok";
    }
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return "unknown exception";
    }
}

auto seed(coap_multi_raft_cluster& c) -> void {
    for (const auto& k : all_keys()) {
        BOOST_REQUIRE_MESSAGE(c.put(k, "v-" + k), "put " << k << " never committed over CoAP");
    }
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_coap)

BOOST_AUTO_TEST_CASE(each_group_holds_only_its_own_keys_over_one_shared_client,
                     *boost::unit_test::timeout(180)) {
    coap_multi_raft_cluster c;
    BOOST_REQUIRE(c.await_leaders(scaled_deadline(20000)));
    seed(c);

    BOOST_CHECK(c.await_contents(k_left, left_keys(), scaled_deadline(10000)));
    BOOST_CHECK(c.await_contents(k_right, right_keys(), scaled_deadline(10000)));
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_CHECK_MESSAGE(c.keys_of(id, k_left) == left_keys(),
                            "node " << id << "'s left replica holds a key it does not own");
        BOOST_CHECK_MESSAGE(c.keys_of(id, k_right) == right_keys(),
                            "node " << id << "'s right replica holds a key it does not own");
        // Nothing arrived addressed to a group the cluster never created.
        for (auto group : c.inbound(id)) {
            BOOST_CHECK_MESSAGE(group == k_left || group == k_right,
                                "node " << id << " received traffic for unknown group " << group);
        }
    }
    // The group id survived encoding: each group's followers received that
    // group's traffic over CoAP. (A host leading a group receives only its
    // followers' replies for it, which are not requests, so it is skipped.)
    for (auto group : {k_left, k_right}) {
        const auto leader = c.leader_id_of(group);
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            if (leader.has_value() && *leader == id) {
                continue;
            }
            BOOST_CHECK_MESSAGE(
                c.inbound(id).contains(group),
                "node " << id << " never received group " << group << "'s traffic over CoAP");
        }
    }
}

BOOST_AUTO_TEST_CASE(a_split_completes_and_each_child_gets_its_own_traffic,
                     *boost::unit_test::timeout(240)) {
    coap_multi_raft_cluster c;
    BOOST_REQUIRE(c.await_leaders(scaled_deadline(20000)));
    seed(c);

    auto* leader = c.leader_of(k_left);
    BOOST_REQUIRE(leader != nullptr);
    const auto err = settle(leader->split_shard(k_left, {"charlie"}, scaled_deadline(10000)),
                            scaled_deadline(15000));
    BOOST_REQUIRE_MESSAGE(err == nullptr, "split over CoAP failed: " << describe(err));

    // Every host learns the split from the entry itself and tiles the key space.
    BOOST_REQUIRE(c.await(
        [&] {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.host(id).shard_map_snapshot().descriptors().size() != 3) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(20000)));
    BOOST_CHECK(!c.tiling_problem().has_value());

    // `(-inf, "charlie")` is the derived child and keeps the parent's id;
    // `["charlie", "m")` is the new group the id authority handed out.
    const auto derived = c.host(1).resolve("alpha");
    const auto fresh = c.host(1).resolve("delta");
    BOOST_REQUIRE(derived.has_value() && fresh.has_value());
    BOOST_REQUIRE_EQUAL(derived->_group_id, k_left);
    BOOST_REQUIRE_NE(fresh->_group_id, k_left);
    const auto child = fresh->_group_id;

    // The new group elects over CoAP and its traffic reaches every follower
    // under its own id, not the parent's.
    BOOST_REQUIRE(c.await([&] { return c.leader_of(child) != nullptr; }, scaled_deadline(20000)));
    BOOST_CHECK(c.await(
        [&] {
            const auto child_leader = c.leader_id_of(child);
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if ((!child_leader || *child_leader != id) && !c.inbound(id).contains(child)) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(10000)));

    // A write on the parent's side of the cut lands in the derived child's
    // replicas and never in the new group's.
    BOOST_REQUIRE(c.put("baker", "after-split"));
    const std::vector<key_type> probe{"baker"};
    BOOST_CHECK(c.await(
        [&] {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.keys_of(id, k_left, probe) != probe) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(10000)));
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_CHECK_MESSAGE(c.keys_of(id, child, probe).empty(),
                            "node " << id << ": a write to one child reached the other's replica");
    }

    // And a write on the other side lands in the new group, which started from
    // a snapshot with an empty log. Its first AppendEntries has the snapshot's
    // last entry as its previous entry, so this is also the case
    // `fix(raft): match on the snapshot boundary so split children replicate`
    // fixed: before it, this write never committed on any transport.
    BOOST_REQUIRE(c.put("echo", "in-new-group"));
    const std::vector<key_type> fresh_probe{"echo"};
    BOOST_CHECK(c.await(
        [&] {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.keys_of(id, child, fresh_probe) != fresh_probe) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(10000)));
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_CHECK_MESSAGE(
            c.keys_of(id, k_left, fresh_probe).empty(),
            "node " << id << ": a write to the new group reached the derived child");
    }
}

BOOST_AUTO_TEST_CASE(a_merge_completes_over_coap, *boost::unit_test::timeout(240)) {
    coap_multi_raft_cluster c;
    BOOST_REQUIRE(c.await_leaders(scaled_deadline(20000)));
    seed(c);

    auto* source_leader = c.leader_of(k_right);
    BOOST_REQUIRE(source_leader != nullptr);
    const auto err = settle(source_leader->merge_shards(k_right, k_left, scaled_deadline(10000)),
                            scaled_deadline(15000));
    BOOST_REQUIRE_MESSAGE(err == nullptr, "merge over CoAP failed: " << describe(err));

    BOOST_REQUIRE(c.await(
        [&] {
            for (node_id_t id = 1; id <= k_node_count; ++id) {
                if (c.host(id).applied_merge_count() == 0) {
                    return false;
                }
            }
            return true;
        },
        scaled_deadline(30000)));

    // The survivor now owns every key, on every replica.
    BOOST_CHECK(c.await_contents(k_left, all_keys(), scaled_deadline(10000)));
    BOOST_CHECK(!c.tiling_problem().has_value());
    BOOST_CHECK(c.host(1).resolve("papa").has_value() &&
                c.host(1).resolve("papa")->_group_id == k_left);
}

BOOST_AUTO_TEST_CASE(scatter_moves_leadership_over_coap, *boost::unit_test::timeout(180)) {
    coap_multi_raft_cluster c;
    BOOST_REQUIRE(c.await_leaders(scaled_deadline(20000)));

    const auto from = c.leader_id_of(k_left);
    BOOST_REQUIRE(from.has_value());
    auto future = c.host(*from).scatter(k_left, scaled_deadline(5000));

    const bool moved = c.await(
        [&] {
            const auto now = c.leader_id_of(k_left);
            return now.has_value() && *now != *from;
        },
        scaled_deadline(15000));
    BOOST_CHECK_MESSAGE(moved, "scatter left leadership where it was over CoAP");
    // The other group was not part of this: its leader may have changed for
    // unrelated reasons, but it still has one.
    BOOST_CHECK(c.leader_of(k_right) != nullptr);
    std::ignore = settle(std::move(future), scaled_deadline(10000));
}

BOOST_AUTO_TEST_SUITE_END()
