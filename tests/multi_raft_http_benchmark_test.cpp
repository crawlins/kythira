// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_http_benchmark_test.cpp
/// @brief `multi_raft` over real HTTP transports and real RPC serializers,
///        carrying the key/value payload — Tier B of
///        `.kiro/specs/multi-raft-performance/`.
///
/// ### What is new here
///
/// Every other multi-Raft suite in this tree runs on the in-process fabric.
/// This one puts the host on a socket for the first time: three hosts, four
/// shards each, one shared HTTP client and one shared HTTP server per host,
/// every RPC encoded by a real serializer and demultiplexed by group id at the
/// far end. That makes it two things at once, and the order matters —
///
/// 1. **A correctness gate.** A benchmark over a path that has never been
///    exercised measures nothing trustworthy. The first case elects, commits and
///    reads back over each transport before any number is taken.
/// 2. **The first comparable measurement.** Serialization and the wire are
///    exactly the two costs the fabric removes, and they are the two an external
///    implementation's published number always includes.
///
/// ### The matrix
///
/// Transports: cpp-httplib, Boost.Beast, Proxygen — each built only when its own
/// configure-time flag is set, so a tree without Beast or Proxygen still builds
/// and runs a smaller matrix (and says which rows it dropped, rather than
/// printing a shorter table that reads as a complete one).
///
/// Serializers: JSON and CBOR always; protobuf when
/// `KYTHIRA_BENCH_HAS_PROTOBUF` is defined. These are the "RPC providers" axis —
/// what actually goes on the wire. The node-internal serializer is held at JSON
/// across every row so the axis being swept is the only one moving.
///
/// ### Beast
///
/// This suite is what found the concurrency defect in `boost_beast_client` that
/// `fix(beast): check connections out exclusively per RPC` repaired: multi-Raft
/// is the first workload in the tree to issue concurrent RPCs to one peer, since
/// four groups on four executor stripes replicate to the same two peers at once.
/// The isolated reproduction now lives beside the transport, in
/// `beast_client_test.cpp`, which is where a transport-level regression test
/// belongs. Its rows run here by default.
///
/// ### A word about cpp-httplib's row
///
/// It is expected to be roughly two orders of magnitude slower than the other
/// two, and the cause is known and already documented: cpp-httplib's vendored
/// header defaults `CPPHTTPLIB_TCP_NODELAY` to `false`, so every small RPC body
/// pays the classic ~80ms Nagle/delayed-ACK round trip
/// (`doc/http_transport_performance_comparison.md`, which measured 12 ops/sec
/// against Beast's 3,527 on a bare ping-pong). Beast and Proxygen sit on
/// Asio/Folly `AsyncSocket`, which set `TCP_NODELAY` themselves. That is a
/// configuration default, not a verdict on the transport, and it is why this
/// row runs a much smaller operation budget: at ~83ms per RPC a full-sized
/// budget would take longer than the whole rest of the suite.
///
/// ### CoAP
///
/// Not here, because `multi_raft` has no CoAP binding today. When it gets one,
/// the harness needs a fixture with the shape of `cpp_httplib_transport` and
/// nothing else: the client and server handles forward every optional RPC behind
/// a `requires` clause, so CoAP's documented lack of TimeoutNow makes it fail
/// `network_client_with_timeout_now`, `multi_group_network_server` skips that
/// handler by `if constexpr`, and leadership transfer reports `unsupported` on
/// that row instead of failing to compile.

#define BOOST_TEST_MODULE multi_raft_http_benchmark_test
#include <boost/test/unit_test.hpp>

#include "multi_raft_transport_harness.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>

#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
#include <raft/protobuf_serializer.hpp>
#endif

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
#include <folly/init/Init.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
namespace {
struct folly_init_fixture {
    folly_init_fixture() {
        int argc = 1;
        char* argv_data[] = {const_cast<char*>("multi_raft_http_benchmark_test"), nullptr};
        char** argv = argv_data;
        _init = std::make_unique<folly::Init>(&argc, &argv);
    }
    std::unique_ptr<folly::Init> _init;
};
}  // namespace
BOOST_GLOBAL_FIXTURE(folly_init_fixture);
#endif

namespace {

using kythira::testing::benchmark_result;
using kythira::testing::cpp_httplib_transport;
using kythira::testing::describe_machine;
using kythira::testing::k_required_repetitions;
using kythira::testing::key_distribution;
using kythira::testing::kv_cluster;
using kythira::testing::kv_cluster_options;
using kythira::testing::kv_get;
using kythira::testing::kv_key;
using kythira::testing::kv_put;
using kythira::testing::kv_value;
using kythira::testing::machine_description;
using kythira::testing::operation_tally;
using kythira::testing::repeated_result;
using kythira::testing::run_put_workload;
using kythira::testing::to_string;
using kythira::testing::workload_options;

using json = kythira::json_serializer;
using cbor = kythira::cbor_serializer;

/// The cluster shape every row shares. Comparability requires that the only
/// thing differing between rows is the axis under test, so these are constants
/// rather than per-transport tuning.
///
/// The election timings are deliberately far more generous than the fabric
/// suites use. On cpp-httplib a single tick's send phase blocks for one ~83ms
/// round trip per follower per stripe, so a 300ms election timeout — fine over
/// an in-process fabric — would have followers timing out on a leader that is
/// merely mid-heartbeat. Four stripes for four groups keeps each stripe to one
/// group's worth of blocking I/O per tick.
auto standard_cluster_options() -> kv_cluster_options {
    kv_cluster_options options;
    options._nodes = 3;
    options._groups = 4;
    options._key_count = 100000;
    options._executor_stripes = 4;
    options._tick_interval = std::chrono::milliseconds{2};
    options._election_timeout_min = std::chrono::milliseconds{2000};
    options._election_timeout_max = std::chrono::milliseconds{4000};
    options._heartbeat_interval = std::chrono::milliseconds{400};
    return options;
}

constexpr auto k_election_budget = std::chrono::milliseconds{30000};

auto us(std::chrono::nanoseconds d) -> double {
    return std::chrono::duration<double, std::micro>(d).count();
}

/// @brief The machine every number in this process was taken on.
///
/// Captured on first use, which the global fixture below forces to be before
/// the first case runs: the load average is the point, and by the second case
/// it is measuring this suite rather than what else the machine was doing.
///
/// One description for the whole process is also what makes Requirement 6.6
/// hold structurally — every ours-vs-ours comparison this binary prints came
/// from the same machine in the same session, because there is only one.
auto machine() -> const machine_description& {
    static const machine_description described = describe_machine(".");
    return described;
}

auto report(const machine_description& m) -> void {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "machine (Requirement 6.4):"
        << "\n    cpu:      " << m._cpu_model << " x" << m._logical_cpus << " logical"
        << "\n    memory:   " << static_cast<double>(m._memory_bytes) / (1024.0 * 1024 * 1024)
        << " GiB"
        << "\n    kernel:   " << m._kernel << "\n    storage:  " << m._described_path << " on "
        << m._filesystem << ", device " << m._device << " (" << m._device_kind << ")"
        << "\n    build:    " << m._compiler << ", " << m._build_type << ", sanitizer "
        << m._sanitizer << ", future backend " << m._future_backend
        << "\n    flags:    " << m._cxx_flags << "\n    load:     " << std::setprecision(2)
        << m._load_average_1m << " (1m) — "
        << (m._quiet_at_start ? "machine was quiet at start"
                              : "OTHER LOAD WAS PRESENT AT START; these numbers are not "
                                "publication-grade (Requirement 6.5)");
    BOOST_TEST_MESSAGE(out.str());
}

/// One repetition, printed so the spread behind a headline is visible rather
/// than summarised away.
auto report(std::size_t repetition, const benchmark_result& row) -> void {
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "    run " << (repetition + 1) << "/" << k_required_repetitions << ": "
        << row._ops_per_second << " ops/sec | p50=" << us(row._p50) << "us p95=" << us(row._p95)
        << "us";
    if (row._p99.has_value()) {
        out << " p99=" << us(*row._p99) << "us";
    } else {
        out << " p99=n/a (" << row._tally._completed << " samples)";
    }
    out << " | offered=" << row._tally._offered << " completed=" << row._tally._completed
        << " not_leader=" << row._tally._not_leader << " timeout=" << row._tally._timeout
        << " other=" << row._tally._other;
    BOOST_TEST_MESSAGE(out.str());
}

/// One row, printed in the shape the comparison document wants: a headline that
/// exists only when enough repetitions stand behind it, the spread beside it,
/// and the verdict that decides whether the row may be compared to anything.
auto report(const repeated_result& row) -> void {
    const auto& median = row.median_run();
    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "  " << median._transport << " / " << median._serializer << " / " << median._scenario
        << ": nodes=" << median._nodes << " groups=" << median._groups
        << " value=" << median._value_bytes << "B in_flight=" << median._in_flight
        << "\n      headline: ";
    if (const auto headline = row.headline_ops_per_second()) {
        out << *headline << " ops/sec (median of " << row.runs() << " runs)";
    } else {
        out << "NONE — " << row.runs() << " run(s), " << k_required_repetitions
            << " required before a headline exists";
    }
    out << " | min " << row.min_ops_per_second() << " max " << row.max_ops_per_second()
        << " | spread " << std::setprecision(1) << (row.spread() * 100.0) << "% of median"
        << "\n      verdict:  " << to_string(row.verdict());
    if (!row.comparable()) {
        out << " — MUST NOT ENTER A COMPARISON TABLE (Requirement 6.3)";
    }
    if (!machine()._quiet_at_start) {
        out << "; machine was not quiet at start";
    }
    out << "\n      median run: p50=" << us(median._p50) << "us p95=" << us(median._p95) << "us";
    if (median._p99.has_value()) {
        out << " p99=" << us(*median._p99) << "us";
    } else {
        out << " p99=n/a (" << median._tally._completed << " samples)";
    }
    out << "\n      counts:   " << row._warmup_operations
        << " warm-up operations discarded per run, " << row._measured_operations
        << " offered per measured run (Requirement 6.1)";
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief Elect, commit a PUT on every shard, read each one back through the
/// log, and confirm the routing table still tiles.
///
/// The read is a `GET` command rather than `read_state`: it returns one value
/// rather than the whole store, so a value that landed in the wrong shard is
/// visible as a wrong answer instead of being averaged into a blob.
template<typename Transport> auto smoke_test() -> void {
    kv_cluster<Transport> cluster{standard_cluster_options()};
    BOOST_REQUIRE_MESSAGE(cluster.await_all_leaders(k_election_budget),
                          Transport::name() << ": no leader on every shard within budget");

    operation_tally tally;
    const auto options = cluster.options();

    // One key per shard, taken from the middle of each shard's own range so a
    // boundary bug cannot make the choice accidentally correct.
    for (std::size_t g = 0; g < options._groups; ++g) {
        const auto n = options._key_count * (2 * g + 1) / (2 * options._groups);
        const auto key = kv_key(n);
        const auto value = kv_value(n, 64);

        auto put_latency =
            cluster.run_command(key, kv_put(key, value), std::chrono::milliseconds{20000}, tally);
        BOOST_REQUIRE_MESSAGE(put_latency.has_value(),
                              Transport::name() << ": PUT of '" << key << "' did not commit");

        std::vector<std::byte> read_back;
        auto get_latency = cluster.run_command(key, kv_get(key), std::chrono::milliseconds{20000},
                                               tally, &read_back);
        BOOST_REQUIRE_MESSAGE(get_latency.has_value(),
                              Transport::name() << ": GET of '" << key << "' did not commit");

        std::string observed;
        observed.reserve(read_back.size());
        for (auto b : read_back) {
            observed.push_back(static_cast<char>(b));
        }
        BOOST_CHECK_MESSAGE(observed == value,
                            Transport::name() << ": '" << key << "' read back " << observed.size()
                                              << " bytes, expected " << value.size());
    }

    const auto problem = cluster.tiling_problem();
    BOOST_CHECK_MESSAGE(!problem.has_value(), Transport::name()
                                                  << ": tiling broken: " << problem.value_or(""));
    BOOST_TEST_MESSAGE("  " << Transport::name() << ": " << tally._completed << "/"
                            << tally._offered << " operations committed over a real socket");
}

/// @brief How many operations a repetition throws away before it starts
/// measuring. An eighth of the budget, but never fewer than one per client
/// thread — otherwise a highly-concurrent row warms up some workers and not
/// others, and the measured window pays the difference.
auto warmup_operations(std::size_t operations, std::size_t in_flight) -> std::size_t {
    return std::max<std::size_t>(in_flight, operations / 8);
}

/// @brief One repetition: a fresh cluster, a discarded warm-up, one measured
/// window.
///
/// `operations` is per-transport on purpose. Throughput is a rate, so rows with
/// different budgets remain comparable on ops/sec; what a smaller budget costs
/// is the tail — which is why `latency_sample_set` refuses to print a p99 it
/// does not have the samples for rather than printing a flattering one.
template<typename Transport>
auto one_measurement(std::size_t operations, std::size_t in_flight, std::size_t value_bytes)
    -> benchmark_result {
    kv_cluster<Transport> cluster{standard_cluster_options()};
    BOOST_REQUIRE_MESSAGE(cluster.await_all_leaders(k_election_budget),
                          Transport::name() << ": no leader on every shard within budget");

    const auto terms_before = cluster.term_sum();

    workload_options workload;
    workload._in_flight = in_flight;
    workload._operations = operations;
    workload._value_bytes = value_bytes;
    workload._key_count = cluster.options()._key_count;
    workload._distribution = key_distribution::uniform;
    workload._op_timeout = std::chrono::milliseconds{20000};
    workload._scenario = "put";

    // Warm-up: elections have settled but connections, allocators and the
    // serializer's own buffers have not. Discarded.
    workload_options warmup = workload;
    warmup._operations = warmup_operations(operations, in_flight);
    std::ignore = run_put_workload(cluster, warmup);

    auto row = run_put_workload(cluster, workload);

    const auto terms_after = cluster.term_sum();
    // A window that spanned an election measured election recovery, not steady
    // state. Reported rather than asserted: on a loaded machine an election is
    // a fact about the machine, and failing the build for it would make this
    // test flaky in exactly the way doctrine warns against. It is also one of
    // the things a wide run-to-run spread is usually explained by, which is why
    // it is printed per repetition rather than per row.
    BOOST_TEST_MESSAGE(
        "    " << Transport::name() << ": term sum " << terms_before << " -> " << terms_after
               << (terms_before == terms_after ? " (steady)" : " (AN ELECTION OCCURRED)"));

    BOOST_CHECK_MESSAGE(row._tally._completed > 0, Transport::name()
                                                       << ": no operation completed at all");
    return row;
}

/// @brief One row of the matrix: `k_required_repetitions` whole measurements,
/// reported as a median headline with its spread and a verdict.
///
/// Each repetition builds its own cluster and elects again. That is more
/// expensive than re-running the workload against one long-lived cluster, and
/// it is the point: the ±21% Beast/JSON/128 B spread that made Requirement 6.2
/// necessary appeared *between* freshly-elected clusters, so a repetition that
/// reused one would have measured a narrower thing than the number is quoted as.
template<typename Transport>
auto throughput_row(std::size_t operations, std::size_t in_flight, std::size_t value_bytes,
                    double floor_ops_per_second) -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(operations, in_flight);
    row._measured_operations = operations;

    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = one_measurement<Transport>(operations, in_flight, value_bytes);
        report(repetition, run);
        row.record(std::move(run));
    }
    report(row);

    // A sanity floor, not a target: low enough that a loaded runner passes,
    // high enough that a structural regression (a replication round that stopped
    // batching, a lost connection pool) does not. Asserted against the median
    // rather than every run — one slow repetition out of five is the ordinary
    // behaviour of a shared machine, and it is `spread()`, not this floor, that
    // is supposed to notice it.
    if (const auto headline = row.headline_ops_per_second()) {
        BOOST_CHECK_MESSAGE(*headline >= floor_ops_per_second,
                            Transport::name() << ": median " << *headline
                                              << " ops/sec is below the sanity floor of "
                                              << floor_ops_per_second);
    }
    return row;
}

/// @brief Prints the machine description before the first case runs.
///
/// A global fixture rather than a first test case: a case can be deselected by
/// `--run_test`, and a number whose provenance was deselected along with it is
/// exactly the thing Requirement 6.4 exists to prevent.
struct machine_description_fixture {
    machine_description_fixture() { report(machine()); }
};

}  // namespace

BOOST_GLOBAL_FIXTURE(machine_description_fixture);

BOOST_AUTO_TEST_SUITE(multi_raft_http_benchmark)

// ── correctness first ────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_cpp_httplib, *boost::unit_test::timeout(600)) {
    smoke_test<cpp_httplib_transport<json>>();
}

#if defined(KYTHIRA_BENCH_HAS_BEAST)
BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_beast, *boost::unit_test::timeout(600)) {
    smoke_test<kythira::testing::beast_http_transport<json>>();
}
#endif

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_proxygen, *boost::unit_test::timeout(600)) {
    smoke_test<kythira::testing::proxygen_http_transport<json>>();
}
#endif

// ── the transport axis ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(write_throughput_by_transport, *boost::unit_test::timeout(5400)) {
    BOOST_TEST_MESSAGE("write throughput, 128B values, JSON on the wire:");

    // cpp-httplib: 24 operations at four in flight. At ~83ms per round trip
    // (Nagle/delayed-ACK, see this file's header) that is already tens of
    // seconds, and the client threads contend with the tick threads for the same
    // per-peer connection.
    std::ignore = throughput_row<cpp_httplib_transport<json>>(24, 4, 128, 0.2);

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::ignore = throughput_row<kythira::testing::beast_http_transport<json>>(600, 16, 128, 5.0);
#endif
#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
    std::ignore =
        throughput_row<kythira::testing::proxygen_http_transport<json>>(600, 16, 128, 5.0);
#endif

#if !defined(KYTHIRA_BENCH_HAS_BEAST)
    BOOST_TEST_MESSAGE("  beast row: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
#if !defined(KYTHIRA_BENCH_HAS_PROXYGEN)
    BOOST_TEST_MESSAGE("  proxygen row: NOT RUN (KYTHIRA_BENCH_HAS_PROXYGEN undefined)");
#endif
}

// ── the RPC-provider axis ────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(write_throughput_by_rpc_serializer, *boost::unit_test::timeout(5400)) {
    BOOST_TEST_MESSAGE("write throughput by wire serializer, 128B values:");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::ignore = throughput_row<kythira::testing::beast_http_transport<json>>(600, 16, 128, 5.0);
    std::ignore = throughput_row<kythira::testing::beast_http_transport<cbor>>(600, 16, 128, 5.0);
#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
    std::ignore =
        throughput_row<kythira::testing::beast_http_transport<kythira::protobuf_serializer>>(
            600, 16, 128, 5.0);
#else
    BOOST_TEST_MESSAGE("  protobuf row: NOT RUN (KYTHIRA_BENCH_HAS_PROTOBUF undefined)");
#endif
#else
    // Without Beast the serializer axis would have to ride on cpp-httplib,
    // whose ~83ms round trip dwarfs any encoding difference — the sweep would
    // measure Nagle, not the serializer. Saying so beats printing it.
    BOOST_TEST_MESSAGE(
        "  serializer sweep: NOT RUN (needs a transport whose round trip is not "
        "dominated by Nagle; KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the payload axis ─────────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(write_throughput_by_value_size, *boost::unit_test::timeout(5400)) {
    BOOST_TEST_MESSAGE("write throughput by value size, JSON on the wire:");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    for (std::size_t bytes :
         {std::size_t{16}, std::size_t{128}, std::size_t{1024}, std::size_t{4096}}) {
        std::ignore =
            throughput_row<kythira::testing::beast_http_transport<json>>(400, 16, bytes, 2.0);
    }
#else
    BOOST_TEST_MESSAGE("  value-size sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

BOOST_AUTO_TEST_SUITE_END()
