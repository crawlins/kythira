// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file capacity_coordination_test.cpp
/// @brief The lease and the replicated ledger (tasks 5 and 6 of
///        `.kiro/specs/elastic-shard-capacity/`), first against hooks and then
///        against a real coordination group on the in-process fabric.
///
/// The two shipped defaults share one consensus decision on purpose: the
/// lease is leadership of the coordination group, and the ledger is a state
/// machine in that same group. The real-group cases here are what show the
/// pairing holds up — that a ledger record survives the loss of the leader
/// that wrote it, and that the successor's fencing token is strictly larger.

#define BOOST_TEST_MODULE capacity_coordination_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_test_fabric.hpp"

#include <raft/capacity_lease.hpp>
#include <raft/capacity_ledger.hpp>
#include <raft/console_logger.hpp>
#include <raft/future_default.hpp>
#include <raft/metrics.hpp>
#include <raft/multi_raft_impl.hpp>
#include <raft/persistence.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
        char* argv_data[] = {const_cast<char*>("capacity_coordination_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::capacity_intent_kind;
using kythira::capacity_intent_state;
using kythira::capacity_ledger_write_status;
using kythira::hibernation_mode;
using kythira::raft_leadership_lease;
using kythira::single_process_capacity_lease;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;
using node_id_t = std::uint64_t;
using pg_t = std::string;
using ledger_sm_t = kythira::capacity_ledger_state_machine<node_id_t, pg_t>;
using intent_t = kythira::capacity_intent<node_id_t, pg_t>;

/// A host whose groups run the ledger state machine: the coordination group.
struct coordination_types {
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
    using state_machine_type = ledger_sm_t;

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

using host_type = kythira::multi_raft<coordination_types, key_type, group_id_type>;
using config_type = kythira::multi_raft_config<coordination_types, key_type, group_id_type>;
using future_t = coordination_types::future_type;
using replicated_ledger_t = kythira::replicated_capacity_ledger<node_id_t, pg_t, future_t>;

constexpr group_id_type k_coordination = 1;

auto make_host(message_fabric& fabric, node_id_t id) -> std::unique_ptr<host_type> {
    config_type cfg{
        .node_id = id,
        .network_client = fabric_client{fabric, id},
        .network_server = fabric_server{fabric, id},
        .store_factory =
            [](const group_id_type&) { return coordination_types::persistence_engine_type{}; },
    };
    cfg.config._election_timeout_min = std::chrono::milliseconds{60};
    cfg.config._election_timeout_max = std::chrono::milliseconds{120};
    cfg.config._heartbeat_interval = std::chrono::milliseconds{15};
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 2;
    cfg.heartbeat_interval = std::chrono::milliseconds{0};
    return std::make_unique<host_type>(std::move(cfg));
}

/// The ledger as the controller on `host` would build it.
auto ledger_on(host_type& host) -> replicated_ledger_t {
    return replicated_ledger_t{
        [&host](std::vector<std::byte> bytes) {
            const auto epoch = host.local_descriptor(k_coordination)->_epoch;
            return host.submit_command(k_coordination, epoch, bytes, std::chrono::seconds{2});
        },
        [&host] {
            auto* n = host.group_node(k_coordination);
            if (n == nullptr) {
                return std::vector<intent_t>{};
            }
            return n->with_state_machine([](ledger_sm_t& sm) { return sm.intents(); });
        }};
}

auto make_intent(std::string key, std::uint64_t fencing) -> intent_t {
    intent_t i;
    i._key = std::move(key);
    i._kind = capacity_intent_kind::scale_out;
    i._state = capacity_intent_state::requested;
    i._group = "zone-a";
    i._fencing_token = fencing;
    i._created_at = std::chrono::system_clock::now();
    i._updated_at = i._created_at;
    return i;
}

template<typename Pred>
auto tick_until(const std::vector<host_type*>& hosts, Pred pred,
                std::chrono::milliseconds budget = std::chrono::milliseconds{8000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        for (auto* h : hosts) {
            h->tick();
        }
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return pred();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(capacity_coordination)

// ── the lease, against hooks ─────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(leadership_loss_flips_held_on_the_very_next_check) {
    // Requirement 7.3: the controller stops issuing provider calls "before its
    // next step". The lease is checked at the top of every step, so this is
    // the property that delivers it.
    bool leader = true;
    std::uint64_t term = 3;
    raft_leadership_lease lease{[&] { return leader; }, [&] { return term; }};
    BOOST_CHECK(lease.held());
    BOOST_CHECK_EQUAL(lease.fencing_token(), 3U);
    leader = false;
    BOOST_CHECK(!lease.held());
    leader = true;
    term = 5;
    BOOST_CHECK(lease.held());
    BOOST_CHECK_EQUAL(lease.fencing_token(), 5U);
}

BOOST_AUTO_TEST_CASE(an_indeterminate_lease_is_not_held) {
    // Requirement 7.4: unknown is "no".
    bool throw_leader = false;
    bool throw_term = false;
    std::uint64_t term = 4;
    raft_leadership_lease lease{[&] {
                                    if (throw_leader) {
                                        throw std::runtime_error("probe");
                                    }
                                    return true;
                                },
                                [&] {
                                    if (throw_term) {
                                        throw std::runtime_error("probe");
                                    }
                                    return term;
                                }};
    BOOST_CHECK(lease.held());
    throw_leader = true;
    BOOST_CHECK(!lease.held());
    throw_leader = false;
    throw_term = true;
    BOOST_CHECK(!lease.held());
    throw_term = false;
    // A term that goes backwards is impossible for a correct node, and
    // precisely the kind of impossible thing not to act through.
    term = 2;
    BOOST_CHECK(!lease.held());
    // Unset hooks are unknown too.
    raft_leadership_lease empty{{}, {}};
    BOOST_CHECK(!empty.held());
}

BOOST_AUTO_TEST_CASE(the_single_process_lease_checks_nothing_and_says_so) {
    single_process_capacity_lease lease{42};
    BOOST_CHECK(lease.held());
    BOOST_CHECK_EQUAL(lease.fencing_token(), 42U);
}

// ── against a real coordination group ────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_group_lease_is_held_by_the_leader_and_dropped_with_the_group,
                     *boost::unit_test::timeout(120)) {
    message_fabric fabric{2};
    auto host = make_host(fabric, 1);
    host->create_group(k_coordination, {1});
    host->start();
    auto lease = kythira::make_group_leadership_lease(*host, k_coordination);
    BOOST_REQUIRE(tick_until({host.get()}, [&] { return lease.held(); }));
    BOOST_CHECK_EQUAL(lease.fencing_token(), host->group_node(k_coordination)->get_current_term());
    BOOST_CHECK_GE(lease.fencing_token(), 1U);

    host->destroy_group(k_coordination, kythira::tombstone_reason::admin);
    BOOST_CHECK(!lease.held());
    host->stop();
}

BOOST_AUTO_TEST_CASE(a_replicated_record_is_pending_then_committed_then_minted,
                     *boost::unit_test::timeout(120)) {
    message_fabric fabric{2};
    auto host = make_host(fabric, 1);
    host->create_group(k_coordination, {1});
    host->start();
    auto lease = kythira::make_group_leadership_lease(*host, k_coordination);
    BOOST_REQUIRE(tick_until({host.get()}, [&] { return lease.held(); }));

    auto ledger = ledger_on(*host);
    const auto rec = ledger.record(make_intent("k1", lease.fencing_token()));
    BOOST_REQUIRE(tick_until(
        {host.get()}, [&] { return ledger.status(rec) != capacity_ledger_write_status::pending; }));
    BOOST_CHECK(ledger.status(rec) == capacity_ledger_write_status::committed);
    const auto token = ledger.token(rec);
    BOOST_REQUIRE(token.has_value());
    BOOST_CHECK_EQUAL(token->key(), "k1");
    BOOST_CHECK_EQUAL(token->fencing_token(), lease.fencing_token());

    // An illegal transition commits to the log and is refused by the state
    // machine: the ticket says rejected and the state is unchanged.
    const auto bad = ledger.transition("k1", capacity_intent_state::orphaned, {},
                                       std::chrono::system_clock::now());
    BOOST_REQUIRE(tick_until(
        {host.get()}, [&] { return ledger.status(bad) != capacity_ledger_write_status::pending; }));
    BOOST_CHECK(ledger.status(bad) == capacity_ledger_write_status::rejected);
    BOOST_CHECK(ledger.find("k1")->_state == capacity_intent_state::requested);
    host->stop();
}

BOOST_AUTO_TEST_CASE(the_ledger_survives_a_failover_and_the_fencing_token_grows,
                     *boost::unit_test::timeout(180)) {
    message_fabric fabric{4};
    std::vector<std::unique_ptr<host_type>> hosts;
    for (node_id_t id = 1; id <= 3; ++id) {
        hosts.push_back(make_host(fabric, id));
        hosts.back()->create_group(k_coordination, {1, 2, 3});
        hosts.back()->start();
    }
    std::vector<raft_leadership_lease> leases;
    for (auto& h : hosts) {
        leases.push_back(kythira::make_group_leadership_lease(*h, k_coordination));
    }
    const auto all = std::vector<host_type*>{hosts[0].get(), hosts[1].get(), hosts[2].get()};
    const auto holder = [&]() -> std::optional<std::size_t> {
        std::optional<std::size_t> found;
        for (std::size_t i = 0; i < leases.size(); ++i) {
            if (leases[i].held()) {
                if (found) {
                    return std::nullopt;  // two claimants mid-election: not settled
                }
                found = i;
            }
        }
        return found;
    };
    BOOST_REQUIRE(tick_until(all, [&] { return holder().has_value(); }));
    const auto first = *holder();
    const auto first_token = leases[first].fencing_token();

    auto ledger = ledger_on(*hosts[first]);
    const auto rec = ledger.record(make_intent("before-failover", first_token));
    BOOST_REQUIRE(tick_until(
        all, [&] { return ledger.status(rec) != capacity_ledger_write_status::pending; }));
    BOOST_REQUIRE(ledger.status(rec) == capacity_ledger_write_status::committed);

    // The holder dies. Its own replica may go on believing it leads for a
    // while — which is exactly why the token, not the belief, is what orders
    // writers.
    fabric.kill(hosts[first]->node_id());
    std::vector<host_type*> survivors;
    std::vector<std::size_t> survivor_index;
    for (std::size_t i = 0; i < hosts.size(); ++i) {
        if (i != first) {
            survivors.push_back(hosts[i].get());
            survivor_index.push_back(i);
        }
    }
    std::optional<std::size_t> second;
    BOOST_REQUIRE(tick_until(survivors, [&] {
        for (auto i : survivor_index) {
            if (leases[i].held()) {
                second = i;
                return true;
            }
        }
        return false;
    }));
    BOOST_CHECK_GT(leases[*second].fencing_token(), first_token);

    // The successor reads the intent its predecessor recorded.
    auto successor_ledger = ledger_on(*hosts[*second]);
    BOOST_REQUIRE(tick_until(survivors,
                             [&] { return successor_ledger.find("before-failover").has_value(); }));
    BOOST_CHECK(successor_ledger.find("before-failover")->_state ==
                capacity_intent_state::requested);
    BOOST_CHECK_EQUAL(successor_ledger.find("before-failover")->_fencing_token, first_token);

    fabric.heal_all();
    for (auto& h : hosts) {
        h->stop();
    }
}

BOOST_AUTO_TEST_SUITE_END()
