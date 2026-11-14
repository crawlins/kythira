// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file group_range_mux_test.cpp
/// @brief One Raft listener shared by several multi-raft hosts
///        (.kiro/specs/service-colocation/ task 3).
///
/// The unit cases drive `group_range_mux` through a recording server:
/// routing by range, refusals for unclaimed ranges and stopped hosts, the
/// real server's lifecycle, and `stop()` waiting out a call in flight. The
/// cluster case is the point of the design: three nodes, each with one
/// server and two hosts of different state machine types in disjoint
/// module group spaces, both electing and committing over the same
/// listener and client.

#define BOOST_TEST_MODULE group_range_mux_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/examples/counter_state_machine.hpp>
#include <raft/future_default.hpp>
#include <raft/group_range_mux.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/service_module.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("group_range_mux_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::service_node::group_space;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

// ── a server that records its handlers ──────────────────────────────────────

struct recording_server {
    std::function<kythira::request_vote_response<>(const kythira::request_vote_request<>&)> rv;
    std::function<kythira::request_pre_vote_response<>(const kythira::request_pre_vote_request<>&)>
        pv;
    std::function<kythira::append_entries_response<>(const kythira::append_entries_request<>&)> ae;
    std::function<kythira::install_snapshot_response<>(const kythira::install_snapshot_request<>&)>
        is;
    std::function<kythira::fetch_log_entries_response<>(
        const kythira::fetch_log_entries_request<>&)>
        fl;
    std::function<kythira::timeout_now_response<>(const kythira::timeout_now_request<>&)> tn;
    bool running = false;
    int starts = 0;
    int stops = 0;

    void register_request_vote_handler(decltype(rv) h) { rv = std::move(h); }
    void register_request_pre_vote_handler(decltype(pv) h) { pv = std::move(h); }
    void register_append_entries_handler(decltype(ae) h) { ae = std::move(h); }
    void register_install_snapshot_handler(decltype(is) h) { is = std::move(h); }
    void register_fetch_log_entries_handler(decltype(fl) h) { fl = std::move(h); }
    void register_timeout_now_handler(decltype(tn) h) { tn = std::move(h); }
    void start() {
        running = true;
        ++starts;
    }
    void stop() {
        running = false;
        ++stops;
    }
    [[nodiscard]] auto is_running() const -> bool { return running; }
};

using mux_type = kythira::group_range_mux<recording_server>;

static_assert(kythira::network_server<mux_type::view_type>);
static_assert(kythira::network_server_with_pre_vote<mux_type::view_type>);
static_assert(kythira::network_server_with_log_fetch<mux_type::view_type>);
static_assert(kythira::network_server_with_timeout_now<mux_type::view_type>);

auto vote_request(std::uint64_t group, std::uint64_t term = 7) -> kythira::request_vote_request<> {
    kythira::request_vote_request<> r{};
    r._term = term;
    r._candidate_id = 2;
    r._group_id = group;
    return r;
}

/// A handler that grants every vote and counts what it saw.
auto granting(std::atomic<int>& seen) {
    return [&seen](const kythira::request_vote_request<>& r) {
        ++seen;
        return kythira::request_vote_response<>{
            ._term = r.term(), ._vote_granted = true, ._group_id = r.group_id()};
    };
}

}  // namespace

BOOST_AUTO_TEST_CASE(views_must_not_overlap) {
    recording_server server;
    mux_type mux(server, 1);
    const auto a = group_space::composite(1);
    auto va = mux.view(a.first(), a.last());
    BOOST_TEST(va.first() == a.first());
    BOOST_TEST(va.last() == a.last());
    BOOST_CHECK_THROW(std::ignore = mux.view(a.last(), a.last() + 5), std::invalid_argument);
    BOOST_CHECK_THROW(std::ignore = mux.view(10, 9), std::invalid_argument);
    const auto b = group_space::composite(2);
    BOOST_CHECK_NO_THROW(std::ignore = mux.view(b.first(), b.last()));
    // A stand-alone space covers every id, so nothing else fits beside it.
    BOOST_CHECK_THROW(std::ignore = mux.view(group_space::standalone(3).first(),
                                             group_space::standalone(3).last()),
                      std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(messages_reach_the_host_owning_their_range) {
    recording_server server;
    mux_type mux(server, 1);
    const auto a = group_space::composite(1);
    const auto b = group_space::composite(2);
    auto va = mux.view(a.first(), a.last());
    auto vb = mux.view(b.first(), b.last());
    std::atomic<int> seen_a{0};
    std::atomic<int> seen_b{0};
    va.register_request_vote_handler(granting(seen_a));
    vb.register_request_vote_handler(granting(seen_b));

    // Registered but not started: refused, as a group not held here.
    auto refused = server.rv(vote_request(a.to_host(1)));
    BOOST_TEST(!refused._vote_granted);
    BOOST_TEST(refused._term == 7U);
    BOOST_TEST(refused._group_id == a.to_host(1));
    BOOST_TEST(mux.unrouted_count() == 1U);

    va.start();
    vb.start();
    BOOST_TEST(server.rv(vote_request(a.to_host(1)))._vote_granted);
    BOOST_TEST(server.rv(vote_request(a.to_host(99)))._vote_granted);
    BOOST_TEST(server.rv(vote_request(b.to_host(1)))._vote_granted);
    BOOST_TEST(seen_a.load() == 2);
    BOOST_TEST(seen_b.load() == 1);

    // No module owns group 5 or module 9's range.
    BOOST_TEST(!server.rv(vote_request(5))._vote_granted);
    BOOST_TEST(!server.rv(vote_request(group_space::composite(9).to_host(1)))._vote_granted);
    BOOST_TEST(mux.unrouted_count() == 3U);

    // An optional RPC the host never registered is refused, not counted.
    kythira::append_entries_request<> ae{};
    ae._term = 4;
    ae._group_id = a.to_host(1);
    const auto ae_res = server.ae(ae);
    BOOST_TEST(!ae_res._success);
    BOOST_TEST(ae_res._group_id == a.to_host(1));
    kythira::fetch_log_entries_request<> fl{};
    fl._group_id = b.to_host(3);
    const auto fl_res = server.fl(fl);
    BOOST_TEST(!fl_res._available);
    BOOST_TEST(fl_res._responder_id == 1U);
    BOOST_TEST(mux.unrouted_count() == 3U);

    vb.stop();
    BOOST_TEST(!server.rv(vote_request(b.to_host(1)))._vote_granted);
    BOOST_TEST(server.rv(vote_request(a.to_host(1)))._vote_granted);
}

BOOST_AUTO_TEST_CASE(the_server_runs_while_any_host_does) {
    recording_server server;
    mux_type mux(server, 1);
    auto va = mux.view(group_space::composite(1).first(), group_space::composite(1).last());
    auto vb = mux.view(group_space::composite(2).first(), group_space::composite(2).last());
    BOOST_TEST(!server.running);
    va.start();
    BOOST_TEST(server.running);
    BOOST_TEST(va.is_running());
    BOOST_TEST(!vb.is_running());
    vb.start();
    va.start();  // a second start of one view counts once
    BOOST_TEST(server.starts == 1);
    BOOST_TEST(mux.running_views() == 2U);
    va.stop();
    BOOST_TEST(server.running);
    va.stop();
    BOOST_TEST(server.running);
    vb.stop();
    BOOST_TEST(!server.running);
    BOOST_TEST(server.stops == 1);
    // A host restarting brings the server back.
    va.start();
    BOOST_TEST(server.running);
    BOOST_TEST(server.starts == 2);
    va.stop();
}

BOOST_AUTO_TEST_CASE(stop_waits_for_a_call_inside_the_host) {
    recording_server server;
    mux_type mux(server, 1);
    auto va = mux.view(0, 100);
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<bool> returned{false};
    va.register_request_vote_handler([&](const kythira::request_vote_request<>& r) {
        entered.set_value();
        released.wait();
        returned = true;
        return kythira::request_vote_response<>{
            ._term = r.term(), ._vote_granted = true, ._group_id = r.group_id()};
    });
    va.start();
    std::thread caller([&] { std::ignore = server.rv(vote_request(1)); });
    entered.get_future().wait();
    std::atomic<bool> stopped{false};
    std::thread stopper([&] {
        va.stop();
        stopped = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    BOOST_TEST(!stopped.load());
    release.set_value();
    stopper.join();
    caller.join();
    BOOST_TEST(stopped.load());
    BOOST_TEST(returned.load());
    BOOST_TEST(!server.rv(vote_request(1))._vote_granted);
}

// ── two hosts of different state machines behind one listener ───────────────

namespace {

template<typename StateMachine, typename Server> struct host_types {
    using future_type = kythira::future_default<std::vector<std::byte>>;
    using promise_type = kythira::promise_default<std::vector<std::byte>>;
    using try_type = kythira::try_default<std::vector<std::byte>>;

    using node_id_type = std::uint64_t;
    using term_id_type = std::uint64_t;
    using log_index_type = std::uint64_t;
    using group_id_type = std::uint64_t;

    using serialized_data_type = std::vector<std::byte>;
    using serializer_type = kythira::json_rpc_serializer<serialized_data_type>;

    using network_client_type = fabric_client;
    using network_server_type = Server;

    using persistence_engine_type =
        kythira::memory_persistence_engine<node_id_type, term_id_type, log_index_type>;
    using logger_type = kythira::console_logger;
    using metrics_type = kythira::noop_metrics;
    using membership_manager_type = kythira::default_membership_manager<node_id_type>;
    using state_machine_type = StateMachine;

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

using fabric_mux = kythira::group_range_mux<fabric_server>;
using view_type = fabric_mux::view_type;
using kv_types = host_types<kythira::test_key_value_state_machine<std::uint64_t>, view_type>;
using counter_types = host_types<kythira::examples::counter_state_machine, view_type>;
using kv_host = kythira::multi_raft<kv_types, std::string, std::uint64_t>;
using counter_host = kythira::multi_raft<counter_types, std::string, std::uint64_t>;

auto bytes(std::string_view s) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    for (const char c : s) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

auto text(const std::vector<std::byte>& b) -> std::string {
    std::string out;
    for (const auto x : b) {
        out.push_back(static_cast<char>(x));
    }
    return out;
}

/// `test_key_value_state_machine`'s put: type 1, then length-prefixed key
/// and value.
auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    std::vector<std::byte> out{std::byte{1}};
    const auto append = [&out](const std::string& s) {
        const auto n = static_cast<std::uint32_t>(s.size());
        std::byte buf[sizeof(n)];
        std::memcpy(buf, &n, sizeof(n));
        out.insert(out.end(), std::begin(buf), std::end(buf));
        const auto b = bytes(s);
        out.insert(out.end(), b.begin(), b.end());
    };
    append(key);
    append(value);
    return out;
}

/// Every command of the one-shard hosts below routes to the same key.
struct one_key_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>&) const -> std::string { return "k"; }
};

template<typename Host> auto one_shard(std::uint64_t group) {
    using descriptor = kythira::shard_descriptor<std::uint64_t, std::string, std::uint64_t>;
    return descriptor{
        ._group_id = group,
        ._range = kythira::shard_range<std::string>{._start = std::nullopt, ._end = std::nullopt},
        ._epoch = kythira::shard_epoch{},
        ._voters = {1, 2, 3}};
}

template<typename Types> auto host_config(std::uint64_t id, fabric_client client, view_type view) {
    kythira::multi_raft_config<Types, std::string, std::uint64_t> cfg{
        .node_id = id,
        .network_client = client,
        .network_server = std::move(view),
        .store_factory =
            [](const std::uint64_t&) { return typename Types::persistence_engine_type{}; },
    };
    cfg.config._election_timeout_min = std::chrono::milliseconds{120};
    cfg.config._election_timeout_max = std::chrono::milliseconds{260};
    cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
    cfg.hibernation = kythira::hibernation_mode::off;
    cfg.executor_stripes = 2;
    cfg.partitioner = kythira::make_partitioner<std::string>(one_key_partitioner{});
    return cfg;
}

/// One node: one server, one client, one mux, and a host per module.
struct composite_node {
    composite_node(message_fabric& fabric, std::uint64_t id)
        : server(fabric, id), client(fabric, id), mux(server, id) {
        const auto kv_space = group_space::composite(kythira::service_node::module_numbers::redis);
        const auto counter_space =
            group_space::composite(kythira::service_node::module_numbers::kinesis);
        kv = std::make_unique<kv_host>(
            host_config<kv_types>(id, client, mux.view(kv_space.first(), kv_space.last())));
        counter = std::make_unique<counter_host>(host_config<counter_types>(
            id, client, mux.view(counter_space.first(), counter_space.last())));
        kv->create_group(one_shard<kv_host>(kv_space.to_host(1)));
        counter->create_group(one_shard<counter_host>(counter_space.to_host(1)));
    }

    composite_node(const composite_node&) = delete;
    auto operator=(const composite_node&) -> composite_node& = delete;
    composite_node(composite_node&&) = delete;
    auto operator=(composite_node&&) -> composite_node& = delete;
    ~composite_node() {
        counter->stop();
        kv->stop();
    }

    fabric_server server;
    fabric_client client;
    fabric_mux mux;
    std::unique_ptr<kv_host> kv;
    std::unique_ptr<counter_host> counter;
};

template<typename Host, typename Nodes, typename Pick>
auto submit(Nodes& nodes, Pick pick, std::uint64_t group, const std::vector<std::byte>& command)
    -> std::optional<std::vector<std::byte>> {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto& n : nodes) {
            Host& h = pick(*n);
            auto* g = h.group_node(group);
            if (g == nullptr || !g->is_leader()) {
                continue;
            }
            auto f = h.submit_command("k", command, std::chrono::milliseconds{2000});
            if (f.wait(std::chrono::milliseconds{2000})) {
                try {
                    return std::move(f).get();
                } catch (...) {
                    // Leadership moved; try again.
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    return std::nullopt;
}

}  // namespace

BOOST_AUTO_TEST_CASE(two_hosts_share_one_listener) {
    message_fabric fabric;
    std::vector<std::unique_ptr<composite_node>> nodes;
    for (std::uint64_t id = 1; id <= 3; ++id) {
        nodes.push_back(std::make_unique<composite_node>(fabric, id));
    }
    for (auto& n : nodes) {
        n->kv->start();
        n->counter->start();
        BOOST_TEST(n->mux.running_views() == 2U);
        BOOST_TEST(n->server.is_running());
    }
    std::atomic<bool> running{true};
    std::thread driver([&] {
        while (running) {
            for (auto& n : nodes) {
                n->kv->tick();
                n->counter->tick();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
    });

    const auto kv_group =
        group_space::composite(kythira::service_node::module_numbers::redis).to_host(1);
    const auto counter_group =
        group_space::composite(kythira::service_node::module_numbers::kinesis).to_host(1);
    const auto pick_kv = [](composite_node& n) -> kv_host& { return *n.kv; };
    const auto pick_counter = [](composite_node& n) -> counter_host& { return *n.counter; };

    BOOST_TEST(submit<kv_host>(nodes, pick_kv, kv_group, put_command("a", "1")).has_value());
    std::optional<std::vector<std::byte>> last;
    for (int i = 0; i < 3; ++i) {
        last = submit<counter_host>(nodes, pick_counter, counter_group, bytes("INC"));
        BOOST_REQUIRE(last.has_value());
    }
    BOOST_TEST(text(*last) == "3");

    // Each host holds only its own module's group.
    for (auto& n : nodes) {
        BOOST_TEST(n->kv->group_node(counter_group) == nullptr);
        BOOST_TEST(n->counter->group_node(kv_group) == nullptr);
    }
    BOOST_TEST(nodes[0]->mux.unrouted_count() == 0U);

    // Stopping one module's host on a node leaves the listener up for the
    // other, which keeps committing.
    nodes[2]->kv->stop();
    BOOST_TEST(nodes[2]->server.is_running());
    last = submit<counter_host>(nodes, pick_counter, counter_group, bytes("INC"));
    BOOST_REQUIRE(last.has_value());
    BOOST_TEST(text(*last) == "4");

    running = false;
    driver.join();
    for (auto& n : nodes) {
        n->counter->stop();
        n->kv->stop();
        BOOST_TEST(!n->server.is_running());
    }
    fabric.shutdown();
}
