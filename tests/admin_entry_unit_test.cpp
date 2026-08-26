// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file admin_entry_unit_test.cpp
/// @brief The one change to the consensus core: administration entries
///        (task 15 of `.kiro/specs/multi-raft/`).
///
/// The property that makes split and merge work is not "an entry commits" — it
/// is that the handler fires **on every replica, at the same index, and never
/// on the state machine**. A leader-only callback would make split apply a
/// leader-only decision that followers would have to be told about separately,
/// and an entry that reached the state machine would be applied twice: once as
/// an opaque command it cannot parse, once as a shard operation.
///
/// So the assertions here are about *where* the entry went, on each of three
/// replicas, and about the index it arrived at.

#define BOOST_TEST_MODULE admin_entry_unit_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>
#include <raft/test_state_machine.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("admin_entry_unit_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::entry_type;
using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;

struct host_types {
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
    using network_server_type = fabric_server;

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
using node_type = host_type::group_node_type;

constexpr std::size_t k_node_count = 3;

auto payload_of(const std::string& text) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.reserve(text.size());
    for (char c : text) {
        out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    }
    return out;
}

auto text_of(const std::vector<std::byte>& bytes) -> std::string {
    std::string out;
    out.reserve(bytes.size());
    for (auto b : bytes) {
        out.push_back(static_cast<char>(b));
    }
    return out;
}

/// What one replica's handler saw.
struct observation {
    node_id_t _node{0};
    entry_type _type{entry_type::normal};
    std::uint64_t _index{0};
    std::string _payload;
};

/// Three hosts, one group replicated on all of them, each with an
/// administration-entry handler that records what it saw.
class three_node_group {
public:
    three_node_group() {
        for (node_id_t id = 1; id <= k_node_count; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
            _hosts.back()->create_group(
                descriptor_type{._group_id = 1,
                                ._range = kythira::unbounded_shard_range<key_type>(),
                                ._epoch = shard_epoch{},
                                ._voters = {1, 2, 3}});

            auto* n = _hosts.back()->group_node(1);
            n->set_admin_entry_handler([this, id](const host_types::log_entry_type& entry,
                                                  std::uint64_t index,
                                                  host_types::state_machine_type& sm) {
                // Touching the state machine here is exactly what split apply
                // does, and the reason the handler is handed it by reference:
                // the node's mutex is held, so it cannot be reached any other
                // way without deadlocking.
                _sm_sizes[id - 1] = sm.approximate_key_count();
                std::lock_guard lock(_mutex);
                _seen.push_back(observation{._node = id,
                                            ._type = entry.type(),
                                            ._index = index,
                                            ._payload = text_of(entry.command())});
            });
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] {
                while (_running.load()) {
                    _hosts[i]->tick();
                    std::this_thread::sleep_for(std::chrono::milliseconds{5});
                }
            });
        }
    }

    ~three_node_group() {
        _running = false;
        for (auto& t : _drivers) {
            if (t.joinable()) {
                t.join();
            }
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    three_node_group(const three_node_group&) = delete;
    auto operator=(const three_node_group&) -> three_node_group& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto node(node_id_t id) -> node_type* { return _hosts.at(id - 1)->group_node(1); }

    [[nodiscard]] auto leader() -> node_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(1);
            if (n != nullptr && n->is_leader()) {
                return n;
            }
        }
        return nullptr;
    }

    auto await_leader(std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (leader() != nullptr) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return false;
    }

    [[nodiscard]] auto observations() -> std::vector<observation> {
        std::lock_guard lock(_mutex);
        return _seen;
    }

    /// @brief Wait until every replica's handler has fired `count` times.
    auto await_observations(std::size_t count, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            if (observations().size() >= count) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        return observations().size() >= count;
    }

    [[nodiscard]] auto state_machine_size(node_id_t id) -> std::size_t {
        return _sm_sizes.at(id - 1).load();
    }

    [[nodiscard]] auto fabric() -> message_fabric& { return _fabric; }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = std::chrono::milliseconds{120};
        cfg.config._election_timeout_max = std::chrono::milliseconds{260};
        cfg.config._heartbeat_interval = std::chrono::milliseconds{25};
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        return cfg;
    }

    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::mutex _mutex;
    std::vector<observation> _seen;
    std::array<std::atomic<std::size_t>, k_node_count> _sm_sizes{};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(admin_entry_unit)

BOOST_AUTO_TEST_CASE(an_admin_entry_fires_on_every_replica_at_the_same_index,
                     *boost::unit_test::timeout(120)) {
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    auto* leader = g.leader();
    BOOST_REQUIRE(leader != nullptr);
    auto f = leader->propose_admin_entry(entry_type::split, payload_of("split-payload"),
                                         std::chrono::milliseconds{5000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = f.get());

    // Every replica, not just the leader — the property that makes split and
    // merge deterministic without a second protocol to tell followers.
    BOOST_REQUIRE(g.await_observations(k_node_count, std::chrono::seconds{20}));
    const auto seen = g.observations();
    BOOST_REQUIRE_EQUAL(seen.size(), k_node_count);

    std::set<node_id_t> nodes;
    for (const auto& o : seen) {
        nodes.insert(o._node);
        BOOST_CHECK(o._type == entry_type::split);
        BOOST_CHECK_EQUAL(o._payload, "split-payload");
        // Same index on every one of them.
        BOOST_CHECK_EQUAL(o._index, seen.front()._index);
    }
    BOOST_CHECK_EQUAL(nodes.size(), k_node_count);
}

BOOST_AUTO_TEST_CASE(an_admin_entry_never_reaches_the_state_machine,
                     *boost::unit_test::timeout(120)) {
    // An admin entry the state machine saw would be applied twice: once as an
    // opaque command it cannot parse, once as a shard operation.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    auto* leader = g.leader();
    // A real command first, so "the state machine is empty" cannot pass by
    // accident on a state machine nothing ever reached.
    auto normal =
        leader->submit_command(host_types::state_machine_type::make_put_command("alpha", "1"),
                               std::chrono::milliseconds{5000});
    BOOST_REQUIRE(normal.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = normal.get());

    auto admin = leader->propose_admin_entry(entry_type::merge_prepare, payload_of("prepare"),
                                             std::chrono::milliseconds{5000});
    BOOST_REQUIRE(admin.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = admin.get());

    BOOST_REQUIRE(g.await_observations(k_node_count, std::chrono::seconds{20}));

    // The handler read the state machine's key count as it stood when the admin
    // entry applied: one key from the PUT, and nothing from the admin entry.
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        BOOST_CHECK_EQUAL(g.state_machine_size(id), 1u);
    }
}

BOOST_AUTO_TEST_CASE(a_follower_that_learns_the_entry_by_replication_fires_too,
                     *boost::unit_test::timeout(120)) {
    // The leader applies the entry it proposed; a follower applies one that
    // arrived inside AppendEntries. Those are different code paths in the apply
    // loop's callers, and only the second one is the interesting case.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    auto* leader = g.leader();
    const auto leader_id = leader->get_node_id();

    auto f = leader->propose_admin_entry(entry_type::merge_commit, payload_of("commit"),
                                         std::chrono::milliseconds{5000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
    BOOST_REQUIRE(g.await_observations(k_node_count, std::chrono::seconds{20}));

    std::size_t followers_that_fired = 0;
    for (const auto& o : g.observations()) {
        if (o._node != leader_id) {
            ++followers_that_fired;
        }
    }
    BOOST_CHECK_EQUAL(followers_that_fired, k_node_count - 1);
}

BOOST_AUTO_TEST_CASE(all_five_admin_types_are_routed_to_the_handler,
                     *boost::unit_test::timeout(120)) {
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    const std::vector<entry_type> types{entry_type::split, entry_type::merge_prepare,
                                        entry_type::merge_commit, entry_type::merge_rollback,
                                        entry_type::merge_abandoned};
    for (auto t : types) {
        auto* leader = g.leader();
        BOOST_REQUIRE(leader != nullptr);
        auto f = leader->propose_admin_entry(t, payload_of("p"), std::chrono::milliseconds{5000});
        BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
        BOOST_CHECK_NO_THROW(std::ignore = f.get());
    }

    BOOST_REQUIRE(g.await_observations(types.size() * k_node_count, std::chrono::seconds{30}));
    std::set<entry_type> observed;
    for (const auto& o : g.observations()) {
        observed.insert(o._type);
    }
    BOOST_CHECK_EQUAL(observed.size(), types.size());
}

BOOST_AUTO_TEST_CASE(a_non_admin_type_is_refused_rather_than_silently_losing_the_command,
                     *boost::unit_test::timeout(120)) {
    // Appending a `normal` entry through this path would produce a command the
    // apply loop then routes AWAY from the state machine: silently lost.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));
    auto* leader = g.leader();

    for (auto t : {entry_type::normal, entry_type::configuration, entry_type::no_op}) {
        auto f = leader->propose_admin_entry(t, payload_of("p"), std::chrono::milliseconds{1000});
        BOOST_REQUIRE(f.wait(std::chrono::milliseconds{1000}));
        BOOST_CHECK_THROW(std::ignore = f.get(), std::invalid_argument);
    }
    BOOST_CHECK(g.observations().empty());
}

BOOST_AUTO_TEST_CASE(a_follower_refuses_to_propose_an_admin_entry,
                     *boost::unit_test::timeout(120)) {
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    const auto leader_id = g.leader()->get_node_id();
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        if (id == leader_id) {
            continue;
        }
        auto f = g.node(id)->propose_admin_entry(entry_type::split, payload_of("p"),
                                                 std::chrono::milliseconds{1000});
        BOOST_REQUIRE(f.wait(std::chrono::milliseconds{1000}));
        BOOST_CHECK_THROW(std::ignore = f.get(), std::runtime_error);
    }
}

BOOST_AUTO_TEST_CASE(match_index_of_answers_only_for_a_leader, *boost::unit_test::timeout(120)) {
    // The merge coordinator computes `min_index` from this. A follower's
    // `_match_index` is leftovers from a term it no longer leads, and returning
    // those would let the coordinator carry a tail computed from fiction.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    // Commit something so the indices are non-trivial.
    auto* leader = g.leader();
    auto f = leader->submit_command(host_types::state_machine_type::make_put_command("alpha", "1"),
                                    std::chrono::milliseconds{5000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
    std::this_thread::sleep_for(std::chrono::milliseconds{400});

    const auto leader_id = leader->get_node_id();

    // The leader holds everything in its own log by definition.
    const auto own = leader->match_index_of(leader_id);
    BOOST_REQUIRE(own.has_value());
    BOOST_CHECK_GE(*own, 1u);

    // And it tracks its peers.
    std::size_t peers_tracked = 0;
    for (node_id_t id = 1; id <= k_node_count; ++id) {
        if (id == leader_id) {
            continue;
        }
        if (leader->match_index_of(id).has_value()) {
            ++peers_tracked;
        }
        // A follower answers for nobody, including itself.
        BOOST_CHECK(!g.node(id)->match_index_of(id).has_value());
        BOOST_CHECK(!g.node(id)->match_index_of(leader_id).has_value());
    }
    BOOST_CHECK_EQUAL(peers_tracked, k_node_count - 1);

    // An untracked node id is not invented.
    BOOST_CHECK(!leader->match_index_of(99).has_value());
}

BOOST_AUTO_TEST_CASE(removing_the_handler_leaves_the_entry_applied_as_a_no_op,
                     *boost::unit_test::timeout(120)) {
    // A committed entry is on a majority and every replica must advance past
    // it. Stalling would leave this replica stuck forever on an entry it can
    // never skip; the miss is logged loudly instead.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    for (node_id_t id = 1; id <= k_node_count; ++id) {
        g.node(id)->set_admin_entry_handler({});
    }

    auto* leader = g.leader();
    auto f = leader->propose_admin_entry(entry_type::split, payload_of("p"),
                                         std::chrono::milliseconds{5000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = f.get());
    BOOST_CHECK(g.observations().empty());

    // And the group is still usable afterwards.
    auto after =
        leader->submit_command(host_types::state_machine_type::make_put_command("beta", "2"),
                               std::chrono::milliseconds{5000});
    BOOST_REQUIRE(after.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = after.get());
}

BOOST_AUTO_TEST_CASE(a_throwing_handler_does_not_stall_the_apply_loop,
                     *boost::unit_test::timeout(120)) {
    // Same reasoning: the entry is committed and unskippable, so a handler that
    // throws must not stop this replica from ever applying anything again.
    three_node_group g;
    BOOST_REQUIRE(g.await_leader(std::chrono::seconds{20}));

    for (node_id_t id = 1; id <= k_node_count; ++id) {
        g.node(id)->set_admin_entry_handler(
            [](const host_types::log_entry_type&, std::uint64_t, host_types::state_machine_type&) {
                throw std::runtime_error("handler failed");
            });
    }

    auto* leader = g.leader();
    auto f = leader->propose_admin_entry(entry_type::split, payload_of("p"),
                                         std::chrono::milliseconds{5000});
    BOOST_REQUIRE(f.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = f.get());

    auto after =
        leader->submit_command(host_types::state_machine_type::make_put_command("gamma", "3"),
                               std::chrono::milliseconds{5000});
    BOOST_REQUIRE(after.wait(std::chrono::milliseconds{5000}));
    BOOST_CHECK_NO_THROW(std::ignore = after.get());
}

BOOST_AUTO_TEST_SUITE_END()
