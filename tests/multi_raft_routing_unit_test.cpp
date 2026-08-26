// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_routing_unit_test.cpp
/// @brief Unit tests for client routing and the partitioner (task 13 of
///        `.kiro/specs/multi-raft/`).
///
/// The interesting cases are the failures, because each one has a *different*
/// right answer and collapsing them would be the easy mistake:
///
/// - a stale routing row is repaired from the local replica and retried, so it
///   costs one extra resolution rather than a control-plane query;
/// - a request whose local replica is not the leader fails with the hint
///   attached, so the caller hops once rather than polling;
/// - a command whose own key belongs to another shard is refused outright,
///   because there is no distributed transaction here and applying it anyway
///   would produce a state no invariant catches;
/// - exhausting the retries surfaces the *last real error*, not a generic
///   timeout — "epoch mismatch five times" and "timed out" call for different
///   actions.

#define BOOST_TEST_MODULE multi_raft_routing_unit_test
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

#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_routing_unit_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::cross_shard_command_exception;
using kythira::hibernation_mode;
using kythira::multi_raft;
using kythira::multi_raft_config;
using kythira::shard_descriptor;
using kythira::shard_epoch;
using kythira::shard_epoch_mismatch_exception;
using kythira::shard_not_leader_exception;
using kythira::shard_range;
using kythira::unknown_shard_exception;
using kythira::unrouted_key_exception;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;

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
using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;

/// Encodes a `test_key_value_state_machine` PUT.
auto put_command(const std::string& key, const std::string& value) -> std::vector<std::byte> {
    std::vector<std::byte> out;
    out.push_back(static_cast<std::byte>(1));  // command_type::put
    const auto append_u32 = [&out](std::uint32_t n) {
        std::byte buf[sizeof(std::uint32_t)];
        std::memcpy(buf, &n, sizeof(n));
        out.insert(out.end(), std::begin(buf), std::end(buf));
    };
    const auto append_str = [&out](const std::string& s) {
        for (char c : s) {
            out.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
        }
    };
    append_u32(static_cast<std::uint32_t>(key.size()));
    append_str(key);
    append_u32(static_cast<std::uint32_t>(value.size()));
    append_str(value);
    return out;
}

/// The application's partitioner: the PUT's own key is its routing key.
struct kv_partitioner {
    [[nodiscard]] auto key_of(const std::vector<std::byte>& command) const -> key_type {
        if (command.size() < 1 + sizeof(std::uint32_t)) {
            return {};
        }
        std::uint32_t key_length{};
        std::memcpy(&key_length, command.data() + 1, sizeof(key_length));
        if (1 + sizeof(std::uint32_t) + key_length > command.size()) {
            return {};
        }
        std::string key;
        key.reserve(key_length);
        for (std::uint32_t i = 0; i < key_length; ++i) {
            key.push_back(static_cast<char>(command[1 + sizeof(std::uint32_t) + i]));
        }
        return key;
    }
};
static_assert(kythira::partitioner<kv_partitioner, key_type>);

auto range_of(std::optional<key_type> start, std::optional<key_type> end) -> shard_range<key_type> {
    return shard_range<key_type>{._start = std::move(start), ._end = std::move(end)};
}

auto descriptor(group_id_type g, std::optional<key_type> start, std::optional<key_type> end,
                std::uint64_t version = 0) -> descriptor_type {
    return descriptor_type{._group_id = g,
                           ._range = range_of(std::move(start), std::move(end)),
                           ._epoch = shard_epoch{._version = version, ._conf_version = 0},
                           ._voters = {1},
                           ._learners = {},
                           ._leader_hint = std::nullopt};
}

auto make_config(message_fabric& fabric, std::uint64_t node_id) -> config_type {
    config_type cfg{
        .node_id = node_id,
        .network_client = fabric_client{fabric, node_id},
        .network_server = fabric_server{fabric, node_id},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.config._election_timeout_min = std::chrono::milliseconds{40};
    cfg.config._election_timeout_max = std::chrono::milliseconds{80};
    cfg.config._heartbeat_interval = std::chrono::milliseconds{10};
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 2;
    cfg.partitioner = kythira::make_partitioner<key_type>(kv_partitioner{});
    cfg.route_retry_backoff = std::chrono::milliseconds{1};
    return cfg;
}

template<typename Predicate>
auto tick_until(host_type& host, Predicate predicate,
                std::chrono::milliseconds budget = std::chrono::milliseconds{3000}) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        host.tick();
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}

/// A host with two static shards, both led locally, ready to route against.
struct two_shard_host {
    message_fabric _fabric{2};
    std::unique_ptr<host_type> _host;

    two_shard_host() {
        _host = std::make_unique<host_type>(make_config(_fabric, 1));
        _host->create_group(descriptor(1, std::nullopt, key_type{"m"}));
        _host->create_group(descriptor(2, key_type{"m"}, std::nullopt));
        _host->start();
        const bool led = tick_until(*_host, [&] {
            return _host->group_node(1)->is_leader() && _host->group_node(2)->is_leader();
        });
        BOOST_REQUIRE(led);
    }
    ~two_shard_host() { _host->stop(); }
};

/// Resolve `future` and report what happened, without letting an exception
/// escape into the test body.
template<typename Future>
auto settle(Future&& f, std::chrono::milliseconds budget = std::chrono::milliseconds{2000})
    -> std::exception_ptr {
    if (!f.wait(budget)) {
        return std::make_exception_ptr(std::runtime_error("settle: future never resolved"));
    }
    try {
        std::ignore = f.get();
        return nullptr;
    } catch (...) {
        return std::current_exception();
    }
}

template<typename E> auto is_a(const std::exception_ptr& e) -> bool {
    if (!e) {
        return false;
    }
    try {
        std::rethrow_exception(e);
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_routing_unit)

// ── the partitioner ──────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(the_partitioner_concept_is_satisfied_by_a_key_of_function) {
    static_assert(kythira::partitioner<kv_partitioner, key_type>);
    struct wrong_return {
        [[nodiscard]] auto key_of(const std::vector<std::byte>&) const -> int { return 0; }
    };
    static_assert(!kythira::partitioner<wrong_return, key_type>);
    struct no_key_of {};
    static_assert(!kythira::partitioner<no_key_of, key_type>);

    const kv_partitioner p;
    BOOST_CHECK_EQUAL(p.key_of(put_command("alpha", "v")), "alpha");
    // A malformed command yields an empty key rather than reading past the end.
    BOOST_CHECK_EQUAL(p.key_of({std::byte{1}}), "");
}

// ── happy path ───────────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_command_is_routed_to_the_shard_owning_its_key,
                     *boost::unit_test::timeout(60)) {
    two_shard_host env;
    auto& host = *env._host;

    BOOST_CHECK_EQUAL(host.resolve("apple")->_group_id, 1u);
    BOOST_CHECK_EQUAL(host.resolve("zebra")->_group_id, 2u);

    BOOST_CHECK(settle(host.submit_command(key_type{"apple"}, put_command("apple", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
    BOOST_CHECK(settle(host.submit_command(key_type{"zebra"}, put_command("zebra", "2"),
                                           std::chrono::milliseconds{2000})) == nullptr);

    // Load is counted at the routing layer, which is what makes load-based
    // split work for a state machine with no sizing hooks at all.
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 1u);
    BOOST_CHECK_EQUAL(host.load_counters(2).second, 1u);
}

BOOST_AUTO_TEST_CASE(the_partitioner_form_derives_the_key_from_the_command,
                     *boost::unit_test::timeout(60)) {
    two_shard_host env;
    auto& host = *env._host;
    BOOST_CHECK(settle(host.submit_command(put_command("apple", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 1u);
}

BOOST_AUTO_TEST_CASE(a_read_is_routed_and_counted_separately_from_a_write,
                     *boost::unit_test::timeout(60)) {
    two_shard_host env;
    auto& host = *env._host;
    BOOST_CHECK(settle(host.read_state(key_type{"apple"}, std::chrono::milliseconds{2000})) ==
                nullptr);
    BOOST_CHECK_EQUAL(host.load_counters(1).first, 1u);
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 0u);
}

BOOST_AUTO_TEST_CASE(the_group_addressed_form_serves_a_matching_epoch,
                     *boost::unit_test::timeout(60)) {
    two_shard_host env;
    auto& host = *env._host;
    const auto epoch = host.local_descriptor(1)->_epoch;
    BOOST_CHECK(settle(host.submit_command(group_id_type{1}, epoch, put_command("apple", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
}

// ── failures, each with its own answer ───────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_key_no_shard_owns_is_reported_as_unrouted) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    // Deliberately partial coverage: nothing owns anything at or above "m".
    host.create_group(descriptor(1, std::nullopt, key_type{"m"}));
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    const auto err = settle(host.submit_command(key_type{"zebra"}, put_command("zebra", "1"),
                                                std::chrono::milliseconds{500}));
    BOOST_CHECK(is_a<unrouted_key_exception<key_type>>(err));
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_shard_with_no_local_replica_names_the_voters_to_try) {
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(descriptor(1, std::nullopt, key_type{"m"}));
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    // A routing row learned from elsewhere for a shard this node does not hold.
    BOOST_CHECK(host.learn_descriptors({descriptor(9, key_type{"m"}, std::nullopt, 1)}));

    const auto err = settle(host.submit_command(key_type{"zebra"}, put_command("zebra", "1"),
                                                std::chrono::milliseconds{500}));
    using unknown_error = unknown_shard_exception<group_id_type>;
    BOOST_REQUIRE(is_a<unknown_error>(err));
    try {
        std::rethrow_exception(err);
    } catch (const unknown_error& e) {
        BOOST_CHECK(std::string{e.what()}.find("voters") != std::string::npos);
    }
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_non_leader_replica_fails_with_the_leader_hint_attached,
                     *boost::unit_test::timeout(60)) {
    // MicroRaft's contract, surfaced at the boundary that can honour it:
    // Kythira's transport carries no client-command RPC, so the host cannot
    // forward — it names where to go instead.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    // Three voters but only this replica exists, and the other two are
    // unreachable — so it can never win an election and stays a candidate or
    // follower forever.
    auto desc = descriptor(1, std::nullopt, std::nullopt);
    desc._voters = {1, 2, 3};
    host.create_group(desc);
    host.start();
    for (int i = 0; i < 20; ++i) {
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    BOOST_REQUIRE(!host.group_node(1)->is_leader());

    const auto err = settle(host.submit_command(key_type{"apple"}, put_command("apple", "1"),
                                                std::chrono::milliseconds{500}));
    using not_leader_error = shard_not_leader_exception<group_id_type, std::uint64_t>;
    BOOST_REQUIRE(is_a<not_leader_error>(err));
    try {
        std::rethrow_exception(err);
    } catch (const not_leader_error& e) {
        BOOST_CHECK_EQUAL(e.group_id(), 1u);
        // No leader is known yet, and that is a real state rather than an
        // error: the message says so instead of pretending to a hint.
        BOOST_CHECK(!e.leader_hint().has_value());
        BOOST_CHECK(std::string{e.what()}.find("no leader is known yet") != std::string::npos);
    }
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_stale_routing_row_costs_one_extra_resolution_and_then_succeeds,
                     *boost::unit_test::timeout(60)) {
    // The split-repair path, exercised without a split. The local replica's own
    // descriptor is the authority and the map is a cache of it, so a map that
    // disagrees is repaired from the replica rather than from a control-plane
    // query — which is what makes a split cost an in-flight request one extra
    // resolution instead of a round trip.
    two_shard_host env;
    auto& host = *env._host;

    // Advance shard 1's authority WITHOUT publishing: exactly the window split
    // apply opens between writing the descriptor rows (step E) and publishing
    // the map (step G).
    auto advanced = *host.local_descriptor(1);
    advanced._epoch._version = 7;
    BOOST_REQUIRE(host.set_local_descriptor(1, advanced));

    BOOST_CHECK_EQUAL(host.local_descriptor(1)->_epoch._version, 7u);
    BOOST_REQUIRE_EQUAL(host.shard_map_snapshot().find(1)->_epoch._version, 0u);

    // Routing resolves against the stale row, notices the disagreement, repairs
    // the map and retries — and the caller sees a success, not a failure.
    BOOST_CHECK(settle(host.submit_command(key_type{"apple"}, put_command("apple", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
    BOOST_CHECK_EQUAL(host.shard_map_snapshot().find(1)->_epoch._version, 7u);
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 1u);
}

BOOST_AUTO_TEST_CASE(an_epoch_the_caller_named_explicitly_is_refused_rather_than_repaired,
                     *boost::unit_test::timeout(60)) {
    // The group-addressed form is different in kind from the key-addressed one:
    // the caller computed this request against a descriptor that has since
    // moved, so serving it would apply a command to a range the caller no
    // longer believes is there. Repairing and retrying would hide that.
    two_shard_host env;
    auto& host = *env._host;

    auto advanced = *host.local_descriptor(1);
    advanced._epoch._version = 7;
    BOOST_REQUIRE(host.update_descriptor(1, advanced));

    const shard_epoch stale_epoch{._version = 0, ._conf_version = 0};
    const auto err = settle(host.submit_command(
        group_id_type{1}, stale_epoch, put_command("apple", "1"), std::chrono::milliseconds{500}));
    using epoch_error = shard_epoch_mismatch_exception<group_id_type, key_type, std::uint64_t>;
    BOOST_REQUIRE(is_a<epoch_error>(err));
    try {
        std::rethrow_exception(err);
    } catch (const epoch_error& e) {
        BOOST_CHECK_EQUAL(e.requested_epoch()._version, 0u);
        BOOST_CHECK_EQUAL(e.current_epoch()._version, 7u);
        // The payload repairs the caller's map without a further round trip.
        BOOST_REQUIRE_EQUAL(e.current_descriptors().size(), 1u);
        BOOST_CHECK_EQUAL(e.current_descriptors()[0]._epoch._version, 7u);
    }
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 0u);
}

BOOST_AUTO_TEST_CASE(a_command_whose_own_key_belongs_elsewhere_is_refused,
                     *boost::unit_test::timeout(60)) {
    // There is no distributed transaction here. Applying it anyway would
    // produce a state no invariant in the system catches.
    two_shard_host env;
    auto& host = *env._host;

    // Routed by "apple" (shard 1) but the command itself writes "zebra"
    // (shard 2).
    const auto err = settle(host.submit_command(key_type{"apple"}, put_command("zebra", "1"),
                                                std::chrono::milliseconds{500}));
    using cross_shard_error = cross_shard_command_exception<group_id_type, key_type>;
    BOOST_REQUIRE(is_a<cross_shard_error>(err));
    try {
        std::rethrow_exception(err);
    } catch (const cross_shard_error& e) {
        BOOST_CHECK_EQUAL(e.key(), "zebra");
        BOOST_CHECK_EQUAL(e.group_id(), 1u);
    }
    // Nothing was submitted.
    BOOST_CHECK_EQUAL(host.load_counters(1).second, 0u);
}

BOOST_AUTO_TEST_CASE(without_a_partitioner_the_command_form_says_so_rather_than_guessing) {
    message_fabric fabric{2};
    auto cfg = make_config(fabric, 1);
    cfg.partitioner = nullptr;
    host_type host{std::move(cfg)};
    host.create_group(descriptor(1, std::nullopt, std::nullopt));
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    const auto err =
        settle(host.submit_command(put_command("apple", "1"), std::chrono::milliseconds{500}));
    BOOST_CHECK(is_a<std::logic_error>(err));

    // And the key-addressed form still works: only the admission check is lost.
    BOOST_CHECK(settle(host.submit_command(key_type{"apple"}, put_command("zebra", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_hole_in_the_map_is_repaired_from_the_local_groups_once,
                     *boost::unit_test::timeout(60)) {
    // A routing row can be dropped while the replica is still here — merge
    // apply does exactly that to the source. The next resolution repairs it
    // from the replica rather than failing the request.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(descriptor(1, std::nullopt, std::nullopt));
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    BOOST_REQUIRE(host.forget_routing_row(1));
    BOOST_REQUIRE(!host.shard_map_snapshot().find(1).has_value());
    BOOST_CHECK(!host.resolve("apple").has_value());

    BOOST_CHECK(settle(host.submit_command(key_type{"apple"}, put_command("apple", "1"),
                                           std::chrono::milliseconds{2000})) == nullptr);
    BOOST_CHECK(host.shard_map_snapshot().find(1).has_value());
    host.stop();
}

BOOST_AUTO_TEST_CASE(a_hole_with_no_local_replica_to_repair_it_from_stays_a_hole,
                     *boost::unit_test::timeout(60)) {
    // The repair is bounded: it consults the local replicas once. If the shard
    // genuinely is not here, the request fails rather than spinning.
    message_fabric fabric{2};
    host_type host{make_config(fabric, 1)};
    host.create_group(descriptor(1, std::nullopt, key_type{"m"}));
    host.start();
    BOOST_REQUIRE(tick_until(host, [&] { return host.group_node(1)->is_leader(); }));

    const auto err = settle(host.submit_command(key_type{"zebra"}, put_command("zebra", "1"),
                                                std::chrono::milliseconds{500}));
    BOOST_CHECK(is_a<unrouted_key_exception<key_type>>(err));
    host.stop();
}

BOOST_AUTO_TEST_SUITE_END()
