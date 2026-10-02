// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file elastic_capacity_fabric_test.cpp
/// @brief Elastic capacity on real `multi_raft` hosts over the in-process
///        fabric (tasks 11 and 13 of `.kiro/specs/elastic-shard-capacity/`).
///
/// The controller suites run against a simulated cluster; this one is where
/// the host's side of the contract is shown to hold for real:
///
/// - a host with zero groups heartbeats, so a new machine is visible as
///   capacity before it holds anything (Requirement 2.3);
/// - a learner added by an operator materialises on the new machine through
///   lazy replica creation and the adapter's `lookup_descriptor`
///   (Requirement 10.2), catches up, and shows up in the leader's report;
/// - promotion happens through `add_replica{as_learner=false}`, and the
///   displaced replica is removed only after (Requirement 10.3);
/// - the move ends with the new machine holding a voting replica, the data
///   intact, and the group never below three voters.

#define BOOST_TEST_MODULE elastic_capacity_fabric_test
#include <boost/test/unit_test.hpp>

#include "elastic_capacity_test_support.hpp"
#include "multi_raft_test_fabric.hpp"

#include <raft/capacity_lease.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/console_logger.hpp>
#include <raft/elastic_capacity_controller.hpp>
#include <raft/elastic_shard_placement_driver.hpp>
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
#include <optional>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("elastic_capacity_fabric_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using namespace std::chrono_literals;
using kythira::hibernation_mode;
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

using host_type = kythira::multi_raft<host_types, key_type, group_id_type>;
using config_type = kythira::multi_raft_config<host_types, key_type, group_id_type>;
using descriptor_type = shard_descriptor<group_id_type, key_type, node_id_t>;

using mock_t = kythira::testing::mock_capacity_quorum_manager<>;
using ledger_t = kythira::memory_capacity_ledger<node_id_t, std::string>;
using ctl_t =
    kythira::elastic_capacity_controller<mock_t, kythira::testing::scripted_capacity_policy,
                                         ledger_t, kythira::single_process_capacity_lease,
                                         group_id_type, key_type>;
using inner_t = kythira::no_op_shard_placement_driver<group_id_type, key_type, node_id_t>;
using adapter_t =
    kythira::elastic_shard_placement_driver<inner_t, ctl_t, group_id_type, key_type, node_id_t>;

auto range_descriptor(group_id_type g, std::optional<key_type> start, std::optional<key_type> end)
    -> descriptor_type {
    return descriptor_type{._group_id = g,
                           ._range = kythira::shard_range<key_type>{._start = std::move(start),
                                                                    ._end = std::move(end)},
                           ._epoch = shard_epoch{},
                           ._voters = {1, 2, 3},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}

/// Four hosts; groups 1 and 2 on hosts 1-3; host 4 empty, waiting to be
/// provisioned. One controller, one adapter per host.
class fabric_cluster {
public:
    fabric_cluster() {
        auto cfg = kythira::elastic_capacity_config{};
        cfg.enabled = true;
        cfg.dry_run = false;
        cfg.evaluation_interval = 1min;
        cfg.min_provider_call_interval = 0ms;
        cfg.operator_retry_interval = 3s;  // manual clock: 1s per loop
        cfg.node_report_staleness = 30s;
        cfg.jitter_seed = 11;
        _mgr.add_existing(1, "a");
        _mgr.add_existing(2, "b");
        _mgr.add_existing(3, "c");
        _ctl = std::make_unique<ctl_t>(_mgr, _policy, _ledger,
                                       kythira::single_process_capacity_lease{}, cfg, _ex.fn(),
                                       _clock.fn());
        for (node_id_t id = 1; id <= 4; ++id) {
            _adapters.push_back(std::make_unique<adapter_t>(*_ctl, group_id_type{1000 * id},
                                                            group_id_type{1000 * id + 999}));
        }
        for (node_id_t id = 1; id <= 4; ++id) {
            _hosts.push_back(std::make_unique<host_type>(make_config(id)));
        }
        for (node_id_t id = 1; id <= 3; ++id) {
            host(id).create_group(range_descriptor(1, std::nullopt, key_type{"m"}));
            host(id).create_group(range_descriptor(2, key_type{"m"}, std::nullopt));
        }
        for (auto& h : _hosts) {
            h->start();
        }
        _running = true;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            _drivers.emplace_back([this, i] {
                while (_running.load()) {
                    _hosts[i]->tick();
                    std::this_thread::sleep_for(5ms);
                }
            });
        }
    }

    ~fabric_cluster() {
        _running = false;
        for (auto& t : _drivers) {
            t.join();
        }
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    fabric_cluster(const fabric_cluster&) = delete;
    auto operator=(const fabric_cluster&) -> fabric_cluster& = delete;

    [[nodiscard]] auto host(node_id_t id) -> host_type& { return *_hosts.at(id - 1); }
    [[nodiscard]] auto controller() -> ctl_t& { return *_ctl; }
    [[nodiscard]] auto policy() -> kythira::testing::scripted_capacity_policy& { return _policy; }
    [[nodiscard]] auto ledger() -> ledger_t& { return _ledger; }
    [[nodiscard]] auto manager() -> mock_t& { return _mgr; }

    [[nodiscard]] auto leader_of(group_id_type g) -> host_type* {
        for (auto& h : _hosts) {
            auto* n = h->group_node(g);
            if (n != nullptr && n->is_leader()) {
                return h.get();
            }
        }
        return nullptr;
    }

    /// One heartbeat round: every host reports, the controller's provider
    /// work runs, and its clock moves a second.
    auto heartbeat_round() -> void {
        for (auto& h : _hosts) {
            h->heartbeat();
        }
        _ex.run_all();
        _mgr.settle();
        _clock.advance(1s);
        // Track the fewest voters any leader ever reported.
        for (group_id_type g = 1; g <= 2; ++g) {
            if (auto* l = leader_of(g)) {
                if (auto d = l->local_descriptor(g)) {
                    _min_voters = std::min(_min_voters, d->_voters.size());
                }
            }
        }
    }

    template<typename Pred> auto run_until(Pred pred, std::chrono::milliseconds budget) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            heartbeat_round();
            if (pred()) {
                return true;
            }
            std::this_thread::sleep_for(20ms);
        }
        return pred();
    }

    auto put(const key_type& key, const std::string& value) -> bool {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& h : _hosts) {
                auto desc = h->resolve(key);
                if (!desc) {
                    continue;
                }
                auto* n = h->group_node(desc->_group_id);
                if (n == nullptr || !n->is_leader()) {
                    continue;
                }
                auto f = h->submit_command(
                    key, host_types::state_machine_type::make_put_command(key, value), 2s);
                if (f.wait(2s)) {
                    try {
                        static_cast<void>(std::move(f).get());
                        return true;
                    } catch (...) {
                    }
                }
            }
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    [[nodiscard]] auto min_voters() const -> std::size_t { return _min_voters; }

private:
    auto make_config(node_id_t id) -> config_type {
        config_type cfg{
            .node_id = id,
            .network_client = fabric_client{_fabric, id},
            .network_server = fabric_server{_fabric, id},
            .store_factory =
                [](const group_id_type&) { return host_types::persistence_engine_type{}; },
        };
        cfg.config._election_timeout_min = 120ms;
        cfg.config._election_timeout_max = 260ms;
        cfg.config._heartbeat_interval = 25ms;
        cfg.hibernation = hibernation_mode::off;
        cfg.executor_stripes = 2;
        cfg.heartbeat_interval = 0ms;  // driven by heartbeat_round()
        cfg.node_labels = {id == 4 ? std::string{"a"}
                                   : std::string(1, static_cast<char>('a' + id - 1))};
        cfg.unknown_group_lookup_interval = 100ms;
        auto* a = _adapters.at(id - 1).get();
        cfg.allocate_shard_ids = [a](std::size_t n) { return a->allocate_shard_ids(n).get(); };
        cfg.report_shard_heartbeat = [a](const auto& r) {
            return a->report_shard_heartbeat(r).get();
        };
        cfg.report_node_heartbeat = [a](const auto& r) { a->report_node_heartbeat(r).get(); };
        cfg.report_operator_outcomes = [a](const auto& o) { a->report_operator_outcomes(o); };
        cfg.lookup_descriptor = [a](const group_id_type& g) { return a->lookup_descriptor(g); };
        return cfg;
    }

    message_fabric _fabric{5};
    kythira::testing::manual_clock _clock;
    kythira::testing::manual_executor _ex;
    mock_t _mgr{_clock,
                {.groups = {{.group_id = "a", .target_count = 1},
                            {.group_id = "b", .target_count = 1},
                            {.group_id = "c", .target_count = 1}}},
                /*first_new_id=*/4};
    ledger_t _ledger;
    kythira::testing::scripted_capacity_policy _policy;
    std::unique_ptr<ctl_t> _ctl;
    std::vector<std::unique_ptr<adapter_t>> _adapters;
    std::vector<std::unique_ptr<host_type>> _hosts;
    std::vector<std::thread> _drivers;
    std::atomic<bool> _running{false};
    std::size_t _min_voters{3};
};

}  // namespace

BOOST_AUTO_TEST_SUITE(elastic_capacity_fabric)

BOOST_AUTO_TEST_CASE(an_empty_host_is_visible_as_capacity) {
    // Requirement 2.3, end to end: host 4 has no groups and is still in the
    // controller's inventory.
    fabric_cluster c;
    const auto host4 = [&] {
        const auto s = c.controller().snapshot();
        const auto it = std::find_if(s._nodes.begin(), s._nodes.end(),
                                     [](const auto& n) { return n.node_id() == 4; });
        return it == s._nodes.end() ? std::nullopt : std::optional{it->_report};
    };
    BOOST_REQUIRE(c.run_until([&] { return c.controller().snapshot().cluster_size() == 4; }, 10s));
    BOOST_REQUIRE(host4().has_value());
    BOOST_CHECK_EQUAL(host4()->_shard_count, 0U);
    // The first heartbeat can land within a millisecond of start; uptime only
    // has to become positive, which is what attribution reads.
    BOOST_CHECK(c.run_until([&] { return host4() && host4()->_uptime.count() > 0; }, 10s));
}

BOOST_AUTO_TEST_CASE(a_provisioned_machine_is_admitted_onto_real_hosts) {
    fabric_cluster c;
    BOOST_REQUIRE(c.run_until([&] { return c.leader_of(1) && c.leader_of(2); }, 10s));
    for (const auto* k : {"alpha", "bravo", "charlie", "november", "oscar", "papa"}) {
        BOOST_REQUIRE(c.put(k, std::string("v-") + k));
    }

    c.policy().push(kythira::testing::cap_decision::scale_out_in(
        "a", 1, kythira::capacity_reason::density, {}));
    const bool done = c.run_until(
        [&] {
            const auto all = c.ledger().intents();
            return !all.empty() && all.front()._state == kythira::capacity_intent_state::completed;
        },
        60s);
    for (const auto& [k, v] : c.controller().counters()) {
        BOOST_TEST_MESSAGE(k << "=" << v);
    }
    BOOST_REQUIRE(done);

    // Host 4 now holds a voting replica of at least one group, in a group
    // that still has exactly three voters and never had fewer.
    bool voter_somewhere = false;
    for (group_id_type g = 1; g <= 2; ++g) {
        auto* l = c.leader_of(g);
        BOOST_REQUIRE(l != nullptr);
        const auto d = l->local_descriptor(g);
        BOOST_REQUIRE(d.has_value());
        BOOST_CHECK_EQUAL(d->_voters.size(), 3U);
        voter_somewhere = voter_somewhere || d->has_voter(4);
    }
    BOOST_CHECK(voter_somewhere);
    BOOST_CHECK_GE(c.min_voters(), 3U);
    // Materialised through lazy creation and the adapter's lookup.
    BOOST_CHECK_GE(c.host(4).lazily_created_replica_count(), 1U);
    BOOST_CHECK_GE(c.host(4).descriptor_lookup_count(), 1U);
    BOOST_CHECK_GE(c.controller().counter("shards_moved"), 1U);
}

BOOST_AUTO_TEST_SUITE_END()
