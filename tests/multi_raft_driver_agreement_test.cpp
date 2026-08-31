// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_driver_agreement_test.cpp
/// @brief `.kiro/specs/multi-raft-host-binary/` task 5 — the test that decides
///        whether any of the rest is worth trusting.
///
/// The spec's own words: *if a future reader finds the driver generating its
/// own keys, this design has failed and every cross-tier delta it produced is a
/// comparison of two workloads.* Requirement 4.5 makes a disagreement between
/// the driver and the in-process harness a **defect in one of them** rather
/// than a tier effect, and without this test every cross-tier delta in the
/// comparison document would be unfalsifiable.
///
/// Both paths run at **Tier B**, against the same in-process cluster over
/// loopback, so that nothing but the submit step differs.
///
/// ## What is asserted, and what is only recorded
///
/// **Asserted: the offered workload is byte-for-byte identical.** A recording
/// wrapper captures every `(key, command)` pair each path submits, and the two
/// sequences must match exactly. That is the property Requirement 4.1 is about,
/// it is deterministic, and it fails loudly the moment either consumer starts
/// building its own commands.
///
/// **Asserted: both paths complete every operation they offer.** A row that
/// silently dropped operations on one path and not the other would make every
/// throughput comparison between them meaningless.
///
/// **Recorded, not asserted: throughput and latency.** The two paths differ by
/// an HTTP round trip *by construction* — that is what Tier C buys and what it
/// costs — so an equality assertion on them would be either vacuous (a band
/// wide enough to pass) or flaky (a band narrow enough to mean something).
/// Requirement 4.5's "treat it as a defect" applies to things that *can* agree,
/// and the workload is the thing that can.

#define BOOST_TEST_MODULE multi_raft_driver_agreement_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_benchmark_rows.hpp"
#include "multi_raft_transport_harness.hpp"
#include "test_timeout_scale.hpp"

#include "control_server.hpp"
#include "data_path_target.hpp"
#include "kv_data_server.hpp"

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_driver_agreement_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::bench::control_server;
using kythira::bench::data_path_target;
using kythira::bench::host_endpoint;
using kythira::bench::kv_data_server;
using kythira::bench::node_options;
using kythira::bench::target_options;
using kythira::testing::benchmark_result;
using kythira::testing::kv_cluster;
using kythira::testing::operation_tally;
using kythira::testing::read_kind;
using kythira::testing::workload_options;

using json_wire = kythira::json_serializer;

// Beast where it exists, cpp-httplib otherwise. Not a preference about which
// transport is better: this case is about the *workload* being identical
// across the seam, and cpp-httplib's Raft path costs ~250 ms per committed
// operation on this machine — a Tier C row over it measured 14.6 ops/sec
// against beast's 1397.1 on the same cluster — which would make an identity
// test take minutes to prove something transport-independent.
#if defined(KYTHIRA_BENCH_HAS_BEAST)
using transport_type = kythira::testing::beast_http_transport<json_wire>;
#else
using transport_type = kythira::testing::cpp_httplib_transport<json_wire>;
#endif
using cluster_type = kv_cluster<transport_type>;
using host_type = cluster_type::host_type;

/// @brief What one path offered, in the order each worker offered it.
///
/// Sorted before comparison: the workers are threads, so the interleaving
/// across them is not deterministic even though each worker's own sequence is
/// (the sampler is seeded per worker). Sorting compares the *set of work
/// offered*, which is the claim — and the claim would be just as broken by a
/// missing key as by a reordered one.
struct offered_log {
    std::mutex _mu;
    std::vector<std::string> _entries;

    auto record(const std::string& key, const std::vector<std::byte>& command) -> void {
        std::string line = key;
        line += '|';
        line.reserve(line.size() + command.size());
        for (auto b : command) {
            line.push_back(static_cast<char>(b));
        }
        std::lock_guard lock(_mu);
        _entries.push_back(std::move(line));
    }

    [[nodiscard]] auto sorted() -> std::vector<std::string> {
        std::lock_guard lock(_mu);
        auto copy = _entries;
        std::sort(copy.begin(), copy.end());
        return copy;
    }
};

/// @brief Any workload target, with what it was asked to submit written down.
template<typename Inner> class recording_target {
public:
    recording_target(Inner& inner, offered_log& log) : _inner(inner), _log(log) {}

    auto submit_write(const std::string& key, const std::vector<std::byte>& command,
                      std::chrono::milliseconds timeout, operation_tally& tally)
        -> std::optional<std::chrono::nanoseconds> {
        _log.record(key, command);
        return _inner.submit_write(key, command, timeout, tally);
    }

    auto submit_read(read_kind kind, const std::string& key, std::chrono::milliseconds timeout,
                     operation_tally& tally, std::uint64_t& bytes)
        -> std::optional<std::chrono::nanoseconds> {
        _log.record(key, {});
        return _inner.submit_read(kind, key, timeout, tally, bytes);
    }

    [[nodiscard]] auto rpc_counts() const -> kythira::testing::rpc_snapshot {
        return _inner.rpc_counts();
    }
    [[nodiscard]] auto durability_counts() const -> kythira::testing::durability_snapshot {
        return _inner.durability_counts();
    }
    auto describe(benchmark_result& row) const -> void { _inner.describe(row); }

private:
    Inner& _inner;
    offered_log& _log;
};

/// @brief The data path in front of an in-process cluster, on loopback.
///
/// This is what makes the comparison Tier B on both sides: the same three
/// hosts, in the same process, reached once directly and once through the
/// binary's own server. The only thing added is the wire, which is the thing
/// being priced.
class data_path_front {
public:
    explicit data_path_front(cluster_type& cluster) {
        for (std::size_t i = 0; i < cluster.host_count(); ++i) {
            const auto node_id = static_cast<std::uint64_t>(i + 1);
            const auto data_port = kythira::testing::reserve_port();
            const auto control_port = kythira::testing::reserve_port();

            auto options = std::make_unique<node_options>();
            options->_node_id = node_id;
            options->_groups = cluster.options()._groups;
            options->_key_count = cluster.options()._key_count;
            options->_tick_interval = cluster.options()._tick_interval;

            _data.push_back(std::make_unique<kv_data_server<host_type>>(
                cluster.host(i), "127.0.0.1", data_port, "application/json",
                std::chrono::milliseconds{3000}));
            _control.push_back(std::make_unique<control_server<host_type>>(
                cluster.host(i), *options, "cpp-httplib", "127.0.0.1", control_port));
            _options.push_back(std::move(options));

            _endpoints.push_back(host_endpoint{._node_id = node_id,
                                               ._address = "127.0.0.1",
                                               ._data_port = data_port,
                                               ._control_port = control_port});
        }
        for (auto& server : _data) {
            server->start();
        }
        for (auto& server : _control) {
            server->start();
        }
    }

    ~data_path_front() { stop(); }
    data_path_front(const data_path_front&) = delete;
    auto operator=(const data_path_front&) -> data_path_front& = delete;

    /// Stopped before the cluster it fronts, always: a handler still inside a
    /// request holds a reference into a host that `kv_cluster::shutdown()` is
    /// about to destroy.
    auto stop() -> void {
        for (auto& server : _data) {
            server->stop();
        }
        for (auto& server : _control) {
            server->stop();
        }
    }

    [[nodiscard]] auto endpoints() const -> const std::vector<host_endpoint>& { return _endpoints; }

private:
    std::vector<std::unique_ptr<node_options>> _options;
    std::vector<std::unique_ptr<kv_data_server<host_type>>> _data;
    std::vector<std::unique_ptr<control_server<host_type>>> _control;
    std::vector<host_endpoint> _endpoints;
};

auto agreement_cluster_options() -> kythira::testing::kv_cluster_options {
    auto options = kythira::testing::standard_cluster_options();
    options._nodes = 3;
    options._groups = 2;
    return options;
}

auto agreement_workload() -> workload_options {
    workload_options workload;
    // Small and quick: this case is about identity, not about throughput, and
    // a long window buys nothing that a short one does not already prove.
    workload._operations = 64;
    workload._in_flight = 4;
    workload._value_bytes = 64;
    workload._key_count = 256;
    workload._scenario = "agreement";
    return workload;
}

}  // namespace

BOOST_AUTO_TEST_SUITE(driver_agreement)

/// **Requirement 4.1 and 4.5 — one workload, two submit steps.**
///
/// The strongest form of the claim, asserted exactly rather than approximated
/// by comparing two noisy numbers: given the same options, the in-process path
/// and the data-path driver offer the **same commands for the same keys**.
BOOST_AUTO_TEST_CASE(both_paths_offer_the_same_workload,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(300))) {
    cluster_type cluster{agreement_cluster_options()};
    BOOST_REQUIRE(cluster.await_all_leaders(std::chrono::seconds{30}));

    const auto workload = agreement_workload();

    offered_log in_process_log;
    {
        kythira::testing::in_process_target<transport_type> inner{cluster, workload};
        recording_target<kythira::testing::in_process_target<transport_type>> recorder{
            inner, in_process_log};
        const auto row = kythira::testing::run_put_workload_on(recorder, workload);
        BOOST_TEST_MESSAGE("  in-process: " << row._ops_per_second << " ops/sec, p50 "
                                            << row._p50.count() / 1000 << " us, completed "
                                            << row._tally._completed << '/' << row._tally._offered);
        BOOST_CHECK_EQUAL(row._tally._completed, row._tally._offered);
        BOOST_CHECK(row._internal_counters);
    }

    offered_log out_of_process_log;
    {
        data_path_front front{cluster};
        target_options options;
        options._hosts = front.endpoints();
        options._groups = cluster.options()._groups;
        options._key_count = cluster.options()._key_count;
        options._tier = kythira::testing::deployment_tier::b_loopback;
        options._tick_interval = cluster.options()._tick_interval;

        data_path_target inner{options};
        BOOST_REQUIRE(inner.await_ready(std::chrono::seconds{30}));

        recording_target<data_path_target> recorder{inner, out_of_process_log};
        const auto row = kythira::testing::run_put_workload_on(recorder, workload);
        BOOST_TEST_MESSAGE("  data path:  " << row._ops_per_second << " ops/sec, p50 "
                                            << row._p50.count() / 1000 << " us, completed "
                                            << row._tally._completed << '/' << row._tally._offered);
        BOOST_CHECK_EQUAL(row._tally._completed, row._tally._offered);
        // The row must say the cluster's own counters were not visible, so the
        // artifacts write those columns empty rather than zero.
        BOOST_CHECK(!row._internal_counters);
        front.stop();
    }

    cluster.shutdown();

    const auto a = in_process_log.sorted();
    const auto b = out_of_process_log.sorted();
    BOOST_REQUIRE_EQUAL(a.size(), b.size());
    BOOST_CHECK_MESSAGE(a == b,
                        "the two paths offered different work. Requirement 4.5: that is a defect "
                        "in one of them, not a tier effect — and every cross-tier delta in the "
                        "comparison document rests on this being false");
}

/// **Requirement 2.2 — not-leader is returned, and nothing is forwarded.**
///
/// Addressed at a host that does not lead the target shard, the data path must
/// answer 421 and name the leader when it knows one. It must **not** proxy: a
/// cluster that silently forwarded would move the routing cost inside itself,
/// where no client-side measurement can see it, and
/// `.kiro/specs/multi-raft-performance/` Requirement 8.3 exists to price
/// exactly that cost.
///
/// The evidence that nothing was forwarded is the status code: a forwarded
/// request would have come back 200 with the value stored.
BOOST_AUTO_TEST_CASE(a_misdirected_write_is_answered_not_forwarded,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(300))) {
    cluster_type cluster{agreement_cluster_options()};
    BOOST_REQUIRE(cluster.await_all_leaders(std::chrono::seconds{30}));

    data_path_front front{cluster};
    const auto& endpoints = front.endpoints();
    BOOST_REQUIRE_GE(endpoints.size(), 2u);

    const auto key = kythira::testing::kv_key(7);
    const auto group = cluster.group_of_key(key);
    BOOST_REQUIRE(group.has_value());

    // Whichever host does NOT lead that shard.
    auto* leader_host = cluster.leader_of(*group);
    BOOST_REQUIRE(leader_host != nullptr);
    std::size_t follower_index = endpoints.size();
    for (std::size_t i = 0; i < cluster.host_count(); ++i) {
        if (&cluster.host(i) != leader_host) {
            follower_index = i;
            break;
        }
    }
    BOOST_REQUIRE_LT(follower_index, endpoints.size());

    kythira::bench::kv_data_client client{
        endpoints[follower_index]._address, endpoints[follower_index]._data_port,
        "application/json", std::chrono::milliseconds{2000}, std::chrono::milliseconds{5000}};

    // Requirement 2.2 says the leader is identified **if known**, and a
    // follower learns who leads only when an AppendEntries reaches it. Electing
    // is not the same event as every follower having heard from the winner, so
    // this waits for the second rather than assuming the first implies it —
    // asserting on the first attempt would be a flake dressed as a
    // requirement.
    kythira::bench::kv_result result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
    do {
        result = client.put(key, kythira::testing::kv_put(key, "v"));
        // The 421 itself is the primary claim and must hold on every attempt:
        // a forwarded request would have come back 200 with the value stored.
        BOOST_REQUIRE(result._outcome == kythira::bench::kv_outcome::not_leader);
        if (result._leader.has_value()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{100});
    } while (std::chrono::steady_clock::now() < deadline);

    BOOST_CHECK_MESSAGE(result._leader.has_value(),
                        "a not-leader answer from a replica that has heard from the leader must "
                        "name it, or the client has nowhere to go and the routing cost becomes a "
                        "retry storm rather than one redirect");
    BOOST_CHECK(result._group.has_value());

    front.stop();
    cluster.shutdown();
}

/// **Requirement 1.4 — the shutdown ordering, which fails by terminating the
/// process rather than by failing an assertion.**
///
/// `kv_cluster::shutdown()` is the reference implementation and the host binary
/// follows it: stop the servers, stop the host so its groups' nodes are stopped
/// before they are destroyed, then release the transport. A host destroyed with
/// a group still running takes the process with it, because `~group_state`
/// destroys unstopped nodes through a deferred closure's reference.
///
/// Repeated, because the failure is a race and one clean pass proves very
/// little about a race.
BOOST_AUTO_TEST_CASE(the_data_path_starts_and_stops_repeatedly,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(300))) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        cluster_type cluster{agreement_cluster_options()};
        BOOST_REQUIRE(cluster.await_all_leaders(std::chrono::seconds{30}));
        {
            data_path_front front{cluster};
            target_options options;
            options._hosts = front.endpoints();
            options._groups = cluster.options()._groups;
            options._key_count = cluster.options()._key_count;
            data_path_target target{options};
            BOOST_CHECK(target.await_ready(std::chrono::seconds{30}));
            front.stop();
        }
        cluster.shutdown();
    }
    BOOST_CHECK(true);  // Reaching here at all is the assertion.
}

BOOST_AUTO_TEST_SUITE_END()
