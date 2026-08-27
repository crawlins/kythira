// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_scale_test.cpp
/// @brief A thousand groups on three hosts (task 34 of
///        `.kiro/specs/multi-raft/`).
///
/// The claim under test is the one the whole host design rests on: **cost
/// tracks the groups that are doing something, not the groups that exist.**
/// A thread per group works beautifully at ten groups and stops working
/// somewhere in the hundreds, and every structural decision in `multi_raft` —
/// the striped executor, the batched tick, hibernation — is there to keep that
/// from being the shape of this system.
///
/// So the assertions are about *ratios and counts*, never about wall-clock
/// thresholds. A test that asserted "a tick takes under 5 ms" would fail on a
/// loaded CI box for reasons having nothing to do with the property, and
/// passing it would say nothing about ten thousand groups. What is asserted
/// instead:
///
///  * thread count is a property of the machine, not of the group count;
///  * an idle population hibernates almost completely, so `ready` collapses
///    toward nothing while `total` stays at a thousand;
///  * a tick over an idle thousand costs a small fraction of a tick over an
///    active thousand — the ratio is the measurement, and it is compared
///    against itself rather than against a clock.
///
/// This test is deliberately excluded from the default `ctest` run by its
/// `scale` label: it is slow by construction, and a slow test that runs on
/// every commit gets deleted or ignored.

#define BOOST_TEST_MODULE multi_raft_scale_test
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_scale_test"), nullptr};
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
using kythira::shard_epoch;
using kythira::tick_batch_controller;
using kythira::tombstone_reason;
using kythira::testing::fabric_client;
using kythira::testing::fabric_server;
using kythira::testing::message_fabric;

using key_type = std::string;
using group_id_type = std::uint64_t;

/// The host's shared-transport bundle. Note that the client and server named
/// here are the *shared* ones; `multi_raft` derives each group's scoped views
/// from them.
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

auto fast_raft_config() -> kythira::raft_configuration {
    kythira::raft_configuration c;
    c._election_timeout_min = std::chrono::milliseconds{40};
    c._election_timeout_max = std::chrono::milliseconds{80};
    c._heartbeat_interval = std::chrono::milliseconds{10};
    return c;
}

auto make_config(message_fabric& fabric, std::uint64_t node_id) -> config_type {
    config_type cfg{
        .node_id = node_id,
        .network_client = fabric_client{fabric, node_id},
        .network_server = fabric_server{fabric, node_id},
        .store_factory = [](const group_id_type&) { return host_types::persistence_engine_type{}; },
    };
    cfg.config = fast_raft_config();
    cfg.hibernation = hibernation_mode::off;
    cfg.executor_stripes = 4;
    return cfg;
}

using kythira::shard_descriptor;
using descriptor_type = shard_descriptor<group_id_type, key_type, std::uint64_t>;

/// A thousand groups is the number the design's own scale claims are stated
/// against, and it is where a thread-per-group implementation has long since
/// failed.
constexpr std::size_t k_groups = 1000;

/// Three hosts, each holding a replica of every group — the shape a real
/// deployment has, and the one that makes the transport carry a thousand
/// groups' traffic over one connection pair.
constexpr std::uint64_t k_nodes = 3;

auto range_of(std::size_t index) -> kythira::shard_range<key_type> {
    // Fixed-width keys so lexicographic order is numeric order, and every
    // range is a genuine slice of the key space rather than a label.
    const auto bound = [](std::size_t n) {
        std::string s = std::to_string(n);
        return std::string(6 - s.size(), '0') + s;
    };
    kythira::shard_range<key_type> r;
    r._start = index == 0 ? std::optional<key_type>{} : std::optional<key_type>{bound(index)};
    r._end = index + 1 == k_groups ? std::optional<key_type>{}
                                   : std::optional<key_type>{bound(index + 1)};
    return r;
}

class scale_cluster {
public:
    explicit scale_cluster(kythira::hibernation_mode hibernation,
                           std::size_t hibernation_threshold = 64) {
        for (std::uint64_t id = 1; id <= k_nodes; ++id) {
            config_type cfg{
                .node_id = id,
                .network_client = fabric_client{_fabric, id},
                .network_server = fabric_server{_fabric, id},
                .store_factory =
                    [](const group_id_type&) { return host_types::persistence_engine_type{}; },
            };
            cfg.config._election_timeout_min = std::chrono::milliseconds{300};
            cfg.config._election_timeout_max = std::chrono::milliseconds{600};
            cfg.config._heartbeat_interval = std::chrono::milliseconds{50};
            cfg.hibernation = hibernation;
            cfg.hibernation_group_threshold = hibernation_threshold;
            cfg.hibernate_after = std::chrono::milliseconds{200};
            // Long enough that the policy phase never runs during a timing
            // sample; the subject is the tick, not the policy.
            cfg.policy_interval = std::chrono::hours{1};
            cfg.heartbeat_interval = std::chrono::milliseconds{0};
            cfg.executor_stripes = 4;
            _hosts.push_back(std::make_unique<host_type>(std::move(cfg)));
        }

        const std::vector<std::uint64_t> voters{1, 2, 3};
        for (std::size_t i = 0; i < k_groups; ++i) {
            descriptor_type d;
            d._group_id = static_cast<group_id_type>(i + 1);
            d._range = range_of(i);
            d._epoch = kythira::shard_epoch{._version = 1, ._conf_version = 1};
            d._voters = voters;
            for (auto& h : _hosts) {
                h->create_group(d);
            }
        }
        for (auto& h : _hosts) {
            h->start();
        }
    }

    ~scale_cluster() {
        for (auto& h : _hosts) {
            h->stop();
        }
    }

    scale_cluster(const scale_cluster&) = delete;
    auto operator=(const scale_cluster&) -> scale_cluster& = delete;

    [[nodiscard]] auto host(std::size_t index) -> host_type& { return *_hosts[index]; }
    [[nodiscard]] auto size() const -> std::size_t { return _hosts.size(); }

    /// One tick of every host, returning the first host's report.
    auto tick() -> kythira::tick_report {
        kythira::tick_report first;
        for (std::size_t i = 0; i < _hosts.size(); ++i) {
            auto report = _hosts[i]->tick();
            if (i == 0) {
                first = report;
            }
        }
        return first;
    }

    auto tick_for(std::chrono::milliseconds budget) -> void {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            tick();
        }
    }

    /// Wake every group on the first host. Untimed, so it never lands in a
    /// measurement — the subject is what a tick costs, not what waking costs.
    auto wake_all() -> void {
        for (std::size_t i = 0; i < k_groups; ++i) {
            std::ignore = _hosts[0]->wake(static_cast<group_id_type>(i + 1));
        }
    }

    /// The median total tick duration over `samples` ticks, with `before` run
    /// outside the timed region.
    ///
    /// Median rather than mean: one scheduling hiccup on a shared machine
    /// should not decide the answer, and the property being measured is what a
    /// typical tick costs.
    auto median_tick(int samples, const std::function<void()>& before = {})
        -> std::chrono::nanoseconds {
        std::vector<std::chrono::nanoseconds> times;
        times.reserve(static_cast<std::size_t>(samples));
        for (int i = 0; i < samples; ++i) {
            if (before) {
                before();
            }
            const auto start = std::chrono::steady_clock::now();
            _hosts[0]->tick();
            times.push_back(std::chrono::steady_clock::now() - start);
            for (std::size_t i2 = 1; i2 < _hosts.size(); ++i2) {
                _hosts[i2]->tick();
            }
        }
        std::sort(times.begin(), times.end());
        return times[times.size() / 2];
    }

private:
    message_fabric _fabric{8};
    std::vector<std::unique_ptr<host_type>> _hosts;
};

}  // namespace

BOOST_AUTO_TEST_SUITE(multi_raft_scale)

BOOST_AUTO_TEST_CASE(a_thousand_groups_share_a_fixed_number_of_stripes,
                     *boost::unit_test::timeout(900)) {
    // The single most important scale property, and the cheapest to check:
    // thread count is a property of the MACHINE, never of the shard count. An
    // implementation that gave each group a thread would pass every functional
    // test in this repository and fall over here.
    scale_cluster c{kythira::hibernation_mode::off};

    BOOST_CHECK_EQUAL(c.host(0).group_count(), k_groups);
    BOOST_CHECK_EQUAL(c.host(0).executor_stripe_count(), 4u);

    // ...and the groups are actually spread across those stripes rather than
    // piled onto one, which a per-group lookup test alone would not catch.
    std::set<std::size_t> used;
    for (std::size_t i = 0; i < k_groups; ++i) {
        const auto stripe = c.host(0).stripe_of(static_cast<group_id_type>(i + 1));
        BOOST_REQUIRE_LT(stripe, 4u);
        used.insert(stripe);
    }
    BOOST_CHECK_EQUAL(used.size(), 4u);
}

BOOST_AUTO_TEST_CASE(the_routing_table_tiles_the_key_space_at_a_thousand_shards,
                     *boost::unit_test::timeout(900)) {
    // Tiling is checked everywhere else at a handful of shards. At a thousand
    // it also exercises the map's ordering and bound comparisons at a scale
    // where an off-by-one in `compare_end_bound` would actually show up.
    scale_cluster c{kythira::hibernation_mode::off};

    const auto map = c.host(0).shard_map_snapshot();
    BOOST_CHECK_EQUAL(map.size(), k_groups);
    const auto problem = map.check_tiling();
    BOOST_CHECK_MESSAGE(!problem.has_value(), "tiling broken: " << problem.value_or(""));

    // Every shard's own range resolves back to it — a thousand independent
    // lookups against a thousand rows.
    for (std::size_t i = 0; i < k_groups; ++i) {
        const auto range = range_of(i);
        const auto probe = range._start.value_or(key_type{"000000"});
        auto resolved = c.host(0).resolve(probe);
        BOOST_REQUIRE_MESSAGE(resolved.has_value(), "no shard owns '" << probe << "'");
        BOOST_REQUIRE_EQUAL(resolved->_group_id, static_cast<group_id_type>(i + 1));
    }
}

BOOST_AUTO_TEST_CASE(an_idle_population_hibernates_almost_completely,
                     *boost::unit_test::timeout(900)) {
    // Requirement 5.5. A thousand idle groups still cost a heartbeat each per
    // interval without this, which is the difference between a host that can
    // hold a thousand shards and one that can hold a hundred.
    scale_cluster c{kythira::hibernation_mode::on};

    // Let the population elect, then go quiet long enough to qualify.
    c.tick_for(std::chrono::milliseconds{4000});

    auto report = c.tick();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    while (report._hibernating_count < k_groups * 9 / 10 &&
           std::chrono::steady_clock::now() < deadline) {
        report = c.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }

    BOOST_CHECK_EQUAL(report._total_count, k_groups);
    BOOST_CHECK_MESSAGE(
        report._hibernating_count >= k_groups * 9 / 10,
        "only " << report._hibernating_count << " of " << k_groups << " groups hibernated");
    // The point of hibernation stated the other way round: `ready` is what the
    // tick pays for, and it has collapsed.
    BOOST_CHECK_LT(report._ready_count, k_groups / 10);
}

BOOST_AUTO_TEST_CASE(tick_cost_tracks_ready_groups_rather_than_total_groups,
                     *boost::unit_test::timeout(900)) {
    // The claim in design §4.2, measured as a RATIO against the same cluster's
    // own active cost rather than against a wall-clock threshold. A threshold
    // would be a statement about this machine; the ratio is a statement about
    // the implementation.
    scale_cluster c{kythira::hibernation_mode::on};
    c.tick_for(std::chrono::milliseconds{3000});

    // The ACTIVE sample: every group woken immediately before each timed tick,
    // so the tick really does have a thousand ready groups to drive. Without
    // this the population has already hibernated by the time the first sample
    // is taken and both halves of the comparison measure the same idle tick —
    // which is how this test first passed for entirely the wrong reason.
    c.wake_all();
    const auto active_report = c.tick();
    const auto active = c.median_tick(40, [&c] { c.wake_all(); });
    BOOST_TEST_MESSAGE("active: ready=" << active_report._ready_count
                                        << " median=" << active.count() / 1000 << "us");
    BOOST_REQUIRE_MESSAGE(
        active_report._ready_count >= k_groups * 9 / 10,
        "the active sample only had " << active_report._ready_count << " ready groups");

    // Now let them all go quiet.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    kythira::tick_report idle_report;
    do {
        idle_report = c.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    } while (idle_report._hibernating_count < k_groups * 9 / 10 &&
             std::chrono::steady_clock::now() < deadline);
    BOOST_REQUIRE_GE(idle_report._hibernating_count, k_groups * 9 / 10);

    const auto idle = c.median_tick(40);
    BOOST_TEST_MESSAGE("idle: ready=" << idle_report._ready_count
                                      << " median=" << idle.count() / 1000 << "us");

    // Half is a deliberately loose bound. The property is that the cost fell
    // with the ready count, not that it fell by any particular factor — and a
    // tight bound here would be a timing assertion wearing a ratio's clothes.
    BOOST_CHECK_MESSAGE(idle <= active / 2,
                        "an idle tick over " << k_groups << " groups cost " << idle.count() / 1000
                                             << "us against an active " << active.count() / 1000
                                             << "us; cost is not tracking the ready count");
}

BOOST_AUTO_TEST_CASE(a_request_wakes_only_the_shard_it_is_addressed_to,
                     *boost::unit_test::timeout(900)) {
    // Hibernation would be useless if any request woke the whole population.
    scale_cluster c{kythira::hibernation_mode::on};
    c.tick_for(std::chrono::milliseconds{4000});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    kythira::tick_report report;
    do {
        report = c.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    } while (report._hibernating_count < k_groups * 9 / 10 &&
             std::chrono::steady_clock::now() < deadline);
    BOOST_REQUIRE_GE(report._hibernating_count, k_groups * 9 / 10);

    const auto hibernating_before = report._hibernating_count;
    BOOST_REQUIRE(c.host(0).wake(500));

    const auto after = c.tick();
    BOOST_CHECK_MESSAGE(
        after._hibernating_count + 8 >= hibernating_before,
        "waking one shard woke " << hibernating_before - after._hibernating_count << " of them");
}

BOOST_AUTO_TEST_SUITE_END()
