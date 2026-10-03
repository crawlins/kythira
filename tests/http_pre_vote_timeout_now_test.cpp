// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file http_pre_vote_timeout_now_test.cpp
/// @brief PreVote and TimeoutNow end to end over cpp-httplib (the transport
///        `cmd/multi_raft_node` runs by default), plus leadership transfer over
///        Beast and Proxygen when the build has them
///        (.kiro/specs/http-coap-pre-vote-timeout-now/ tasks 5.4, 5.5 and 6).
///
/// Three `multi_raft` hosts over real sockets, one group. Before this spec the
/// HTTP transports carried neither extension, so on this stack:
///
///  * a follower that lost its inbound traffic for a few election timeouts
///    campaigned on its own and, on rejoining, forced the healthy leader out
///    with its higher term; and
///  * `transfer_leadership()` and `scatter()` ended `unsupported`, which is
///    what left the elastic-capacity move-off-leader path one voter over.
///
/// The concept-level facts (which handlers a group server installs) are
/// asserted at compile time by `group_transport.hpp`; this file checks that
/// they hold on the wire.

#define BOOST_TEST_MODULE http_pre_vote_timeout_now_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_transport_harness.hpp"
#include "test_timeout_scale.hpp"

#include <raft/exceptions.hpp>
#include <raft/json_serializer.hpp>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <chrono>
#include <cstdint>
#include <exception>
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
        char* argv_data[] = {const_cast<char*>("http_pre_vote_timeout_now_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using serializer_type = kythira::json_rpc_serializer<std::vector<std::byte>>;
using transport_type = kythira::testing::cpp_httplib_transport<serializer_type>;
using cluster_type = kythira::testing::kv_cluster<transport_type>;

constexpr std::uint64_t k_group = 1;

auto three_hosts_one_group() -> kythira::testing::kv_cluster_options {
    kythira::testing::kv_cluster_options options;
    options._nodes = 3;
    options._groups = 1;
    options._key_count = 100;
    options._election_timeout_min = std::chrono::milliseconds{300};
    options._election_timeout_max = std::chrono::milliseconds{600};
    options._heartbeat_interval = std::chrono::milliseconds{50};
    return options;
}

/// Host ids are 1-based and hosts are stored in that order.
auto host_index(std::uint64_t id) -> std::size_t {
    return static_cast<std::size_t>(id - 1);
}

template<typename Cluster> auto leader_id(Cluster& cluster) -> std::optional<std::uint64_t> {
    for (std::size_t i = 0; i < cluster.host_count(); ++i) {
        auto* n = cluster.host(i).group_node(k_group);
        if (n != nullptr && n->is_leader()) {
            return static_cast<std::uint64_t>(i + 1);
        }
    }
    return std::nullopt;
}

template<typename Cluster> auto term_of(Cluster& cluster, std::uint64_t id) -> std::uint64_t {
    auto* n = cluster.host(host_index(id)).group_node(k_group);
    return n == nullptr ? 0 : n->get_current_term();
}

template<typename Predicate>
auto wait_until(Predicate predicate, std::chrono::milliseconds budget) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
    return predicate();
}

template<typename Cluster> auto require_leader(Cluster& cluster) -> std::uint64_t {
    std::optional<std::uint64_t> id;
    wait_until(
        [&] {
            id = leader_id(cluster);
            return id.has_value();
        },
        kythira::testing::scaled_deadline(15000));
    BOOST_REQUIRE_MESSAGE(id.has_value(),
                          "no leader was elected over " << Cluster::transport_type::name());
    return *id;
}

/// The exception a future settled with, or nullptr. The cluster's own driver
/// threads tick underneath, so a plain timed wait is enough here.
template<typename Future> auto outcome_of(Future future) -> std::exception_ptr {
    if (!future.wait(kythira::testing::scaled_deadline(20000))) {
        return std::make_exception_ptr(std::runtime_error("future never resolved"));
    }
    try {
        std::ignore = std::move(future).get();
    } catch (...) {
        return std::current_exception();
    }
    return nullptr;
}

auto describe(const std::exception_ptr& e) -> std::string {
    if (!e) {
        return "(none)";
    }
    try {
        std::rethrow_exception(e);
    } catch (const std::exception& ex) {
        return ex.what();
    } catch (...) {
        return "(non-std exception)";
    }
}

/// Requirement 6.2 and 6.5: transfer_leadership() reaches the named node, and
/// scatter() moves leadership off this host, rather than ending `unsupported`.
template<typename Transport> void transfer_and_scatter_move_leadership() {
    kythira::testing::kv_cluster<Transport> cluster{three_hosts_one_group()};

    auto from = require_leader(cluster);
    const std::uint64_t to = from == 3 ? 1 : from + 1;
    std::exception_ptr transferred;
    for (int attempt = 0; attempt < 3; ++attempt) {
        transferred = outcome_of(
            cluster.host(host_index(from))
                .transfer_leadership(k_group, to, kythira::testing::scaled_deadline(5000)));
        // Leadership can move on its own between the sample and the call;
        // that refusal is the test's race, not the product's.
        if (!transferred || describe(transferred).find("not the leader") == std::string::npos) {
            break;
        }
        from = require_leader(cluster);
    }
    BOOST_CHECK_MESSAGE(!transferred, "transfer failed: " << describe(transferred));
    BOOST_CHECK_MESSAGE(wait_until([&] { return leader_id(cluster) == std::optional{to}; },
                                   kythira::testing::scaled_deadline(10000)),
                        "leadership did not reach node " << to);

    const auto before_scatter = require_leader(cluster);
    const auto scattered =
        outcome_of(cluster.host(host_index(before_scatter))
                       .scatter(k_group, kythira::testing::scaled_deadline(5000)));
    BOOST_CHECK_MESSAGE(!scattered, "scatter failed: " << describe(scattered));
    BOOST_CHECK(wait_until(
        [&] {
            auto now = leader_id(cluster);
            return now.has_value() && *now != before_scatter;
        },
        kythira::testing::scaled_deadline(10000)));
    cluster.shutdown();
}

}  // namespace

BOOST_AUTO_TEST_SUITE(http_pre_vote_timeout_now)

// Requirement 2.3: a multi-Raft host over cpp-httplib serves both extensions.
// Before this spec both paths answered 404, which the group server's
// `if constexpr` hid by never registering them.
BOOST_AUTO_TEST_CASE(a_group_server_answers_both_extension_rpcs,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(120))) {
    cluster_type cluster{three_hosts_one_group()};
    const auto leader = require_leader(cluster);
    const std::uint64_t peer = leader == 1 ? 2 : 1;
    auto& client = cluster.transport().client(leader);

    kythira::request_pre_vote_request<> pre_vote{};
    pre_vote._term = 1;
    pre_vote._candidate_id = leader;
    pre_vote._group_id = k_group;
    BOOST_CHECK_MESSAGE(outcome_of(client.send_request_pre_vote(
                            peer, pre_vote, kythira::testing::scaled_deadline(5000))) == nullptr,
                        "pre-vote did not reach a handler");

    // A TimeoutNow from a term the peer has already left is refused, but it is
    // answered: the point here is that the route exists and dispatches.
    kythira::timeout_now_request<> timeout_now{};
    timeout_now._term = 0;
    timeout_now._leader_id = leader;
    timeout_now._group_id = k_group;
    BOOST_CHECK_MESSAGE(outcome_of(client.send_timeout_now(
                            peer, timeout_now, kythira::testing::scaled_deadline(5000))) == nullptr,
                        "timeout_now did not reach a handler");
    cluster.shutdown();
}

BOOST_AUTO_TEST_CASE(transfer_and_scatter_move_leadership_over_cpp_httplib,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    transfer_and_scatter_move_leadership<transport_type>();
}

// Task 5.5: the same over the two asynchronous HTTP transports, when the build
// has them.
#if defined(KYTHIRA_BENCH_HAS_BEAST)
BOOST_AUTO_TEST_CASE(transfer_and_scatter_move_leadership_over_beast,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    transfer_and_scatter_move_leadership<kythira::testing::beast_http_transport<serializer_type>>();
}
#endif

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
BOOST_AUTO_TEST_CASE(transfer_and_scatter_move_leadership_over_proxygen,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    transfer_and_scatter_move_leadership<
        kythira::testing::proxygen_http_transport<serializer_type>>();
}
#endif

// Requirement 6.2: the disruptive-server case PreVote exists for. A follower
// that hears nothing for several election timeouts keeps running pre-vote
// rounds, which its peers refuse because they still hear from the leader, so
// it never raises its term. When it is reachable again the leader's term is
// unchanged. Without PreVote it would campaign at a higher term and depose
// the leader the moment its RequestVote arrived.
BOOST_AUTO_TEST_CASE(an_isolated_follower_rejoins_without_raising_the_term,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(180))) {
    cluster_type cluster{three_hosts_one_group()};
    const auto leader = require_leader(cluster);
    // Let the term settle: a second election racing the first would move it
    // for reasons that have nothing to do with the isolated node.
    std::this_thread::sleep_for(std::chrono::milliseconds{1000});
    const auto leader_now = require_leader(cluster);
    const auto term_before = term_of(cluster, leader_now);
    const std::uint64_t follower = leader_now == 1 ? 2 : 1;
    const auto follower_term_before = term_of(cluster, follower);
    BOOST_TEST_MESSAGE("leader " << leader << " -> " << leader_now << ", term " << term_before
                                 << ", isolating follower " << follower);

    // Stopping the follower's server cuts everything addressed to it: no
    // heartbeats reach it, so its election timer fires repeatedly.
    cluster.transport().server(follower).stop();
    std::this_thread::sleep_for(std::chrono::milliseconds{3000});  // ~5-10 timeouts
    BOOST_CHECK_EQUAL(term_of(cluster, follower), follower_term_before);
    cluster.transport().server(follower).start();

    // Give it time to rejoin and to do any damage it is going to do.
    std::this_thread::sleep_for(std::chrono::milliseconds{2000});
    BOOST_CHECK(leader_id(cluster) == std::optional{leader_now});
    BOOST_CHECK_EQUAL(term_of(cluster, leader_now), term_before);
    BOOST_CHECK_EQUAL(term_of(cluster, follower), term_before);
    cluster.shutdown();
}

BOOST_AUTO_TEST_SUITE_END()
