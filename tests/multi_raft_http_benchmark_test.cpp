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
#include "test_timeout_scale.hpp"

#include <raft/cbor_serializer.hpp>
#include <raft/json_serializer.hpp>

#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
#include <raft/protobuf_serializer.hpp>
#endif

#if defined(KYTHIRA_BENCH_HAS_ION)
#include <raft/ion_serializer.hpp>
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
using kythira::testing::consistency_of;
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
using kythira::testing::preload_keys;
using kythira::testing::read_kind;
using kythira::testing::repeated_result;
using kythira::testing::run_put_workload;
using kythira::testing::run_read_workload;
using kythira::testing::to_string;
using kythira::testing::workload_options;

using json = kythira::json_serializer;
using cbor = kythira::cbor_serializer;
#if defined(KYTHIRA_BENCH_HAS_ION)
// Ion's `media_type()` is the one in this suite that depends on instance state
// -- binary and text are different media types off the same class -- so the row
// is deliberately left to the default-constructed encoding (binary) rather than
// naming a media type here. `run_put_workload` reads the label off the
// serializer, so the row says which one actually went on the wire.
using ion = kythira::ion_serializer;
#endif

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

/// @brief How long a repetition waits for every shard to elect.
///
/// Scaled, not hard-coded, for the reason `test_timeout_scale.hpp` exists and
/// `multi_raft_scale_test` was fixed for: a fixed millisecond budget is being
/// applied to a build whose speed is not fixed. Unscaled, this was the binding
/// constraint on a sanitizer build -- 30 seconds is ample in Release and not
/// enough under ASan to elect four shards over a real socket at all, so the
/// case failed on its own deadline while measuring nothing.
const auto k_election_budget = kythira::testing::scaled_deadline(30000);

/// @brief The per-operation deadline every row hands to `run_command`. Scaled
/// for the same reason, and by the same factor, as the election budget: an
/// operation that times out is counted as a failure rather than as a slow
/// build.
const auto k_operation_timeout = kythira::testing::scaled_deadline(20000);

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
    // The two replication ratios per repetition, not only for the median run.
    // A row whose throughput is UNSTABLE can still carry a ratio that repeats,
    // and the only way a reader can tell the two apart is to see all five.
    out << std::setprecision(2) << " | ent/AE=";
    if (const auto batching = row._rpc.entries_per_append_entries()) {
        out << *batching;
    } else {
        out << "n/a";
    }
    out << " RPC/commit=";
    if (const auto cost = row.rpcs_per_committed_entry()) {
        out << *cost;
    } else {
        out << "n/a";
    }
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief The replication cost of one measured window, in the two ratios
///        Hypotheses H1 and H2 are stated in.
///
/// Printed for the *median run* rather than summed across repetitions: the
/// headline names a real run (Requirement 6.2), and a batching factor averaged
/// over five clusters would not describe the run the headline came from.
///
/// Both ratios are `std::optional` at source and print `n/a` with the reason
/// rather than a zero — 0/0 entries per AppendEntries is "nothing replicated",
/// which is a different statement from "no batching".
auto replication_cost(const benchmark_result& row) -> std::string {
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "replication (median run): " << row._rpc._append_entries << " AppendEntries ("
        << row._rpc._append_entries_empty << " empty) carrying " << row._rpc._entries
        << " entries, " << row._rpc._request_vote << " RequestVote, " << row._rpc._install_snapshot
        << " InstallSnapshot";
    out << "\n      H1 entries/AppendEntries: ";
    if (const auto batching = row._rpc.entries_per_append_entries()) {
        out << *batching << " (over the " << row._rpc.carrying() << " that carried anything)";
    } else {
        out << "n/a — no AppendEntries carried an entry";
    }
    out << "\n      H2 RPCs/committed entry:  ";
    if (const auto cost = row.rpcs_per_committed_entry()) {
        out << *cost << " (" << row._rpc.total_rpcs() << " RPCs / " << row._tally._completed
            << " commits)";
    } else {
        out << "n/a — nothing committed";
    }
    return out.str();
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
    if (median._read_kind.has_value()) {
        out << "\n      read kind: " << to_string(*median._read_kind)
            << "\n      consistency: " << consistency_of(*median._read_kind)
            << "\n      bytes:    " << median._bytes_returned << " returned in the median run, "
            << median.bytes_per_second() / (1024.0 * 1024.0) << " MiB/sec";
        if (const auto per_op = median.bytes_per_operation()) {
            out << ", " << *per_op << " bytes/operation";
        } else {
            out << ", bytes/operation n/a — nothing completed";
        }
    }
    out << "\n      " << replication_cost(median);
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

        auto put_latency = cluster.run_command(key, kv_put(key, value), k_operation_timeout, tally);
        BOOST_REQUIRE_MESSAGE(put_latency.has_value(),
                              Transport::name() << ": PUT of '" << key << "' did not commit");

        std::vector<std::byte> read_back;
        auto get_latency =
            cluster.run_command(key, kv_get(key), k_operation_timeout, tally, &read_back);
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
auto one_measurement(std::size_t operations, std::size_t in_flight, std::size_t value_bytes,
                     key_distribution distribution = key_distribution::uniform)
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
    workload._distribution = distribution;
    workload._op_timeout = k_operation_timeout;
    // The scenario carries the distribution, because the two arms of the
    // distribution sweep are otherwise identical in every printed field and a
    // reader would have no way to tell the rows apart.
    workload._scenario = distribution == key_distribution::zipfian ? "put/zipfian" : "put/uniform";

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

    // The swept axis is the *wire* serializer; the node-internal one (log
    // entries, snapshots) is held at JSON so rows differing only in `_serializer`
    // differ only in what crossed the socket. `kv_host_types` pins it by type
    // alias, but a pin nothing reads is a pin that can be moved without anyone
    // noticing — so every repetition of every row checks the media type the
    // node bundle reports for itself (Requirement 8.4).
    BOOST_CHECK_MESSAGE(row._node_serializer == "application/json",
                        Transport::name()
                            << " / " << row._serializer
                            << ": node-internal serializer drifted off JSON, it reports '"
                            << row._node_serializer
                            << "' — this row is not comparable with the "
                               "others on the serializer axis");
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
                    double floor_ops_per_second,
                    key_distribution distribution = key_distribution::uniform) -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(operations, in_flight);
    row._measured_operations = operations;

    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = one_measurement<Transport>(operations, in_flight, value_bytes, distribution);
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

BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_cpp_httplib,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    smoke_test<cpp_httplib_transport<json>>();
}

#if defined(KYTHIRA_BENCH_HAS_BEAST)
BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_beast,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    smoke_test<kythira::testing::beast_http_transport<json>>();
}
#endif

#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
BOOST_AUTO_TEST_CASE(a_kv_cluster_commits_over_proxygen,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    smoke_test<kythira::testing::proxygen_http_transport<json>>();
}
#endif

// ── the transport axis ───────────────────────────────────────────────────────

BOOST_AUTO_TEST_CASE(write_throughput_by_transport,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
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

BOOST_AUTO_TEST_CASE(write_throughput_by_rpc_serializer,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
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
#if defined(KYTHIRA_BENCH_HAS_ION)
    std::ignore = throughput_row<kythira::testing::beast_http_transport<ion>>(600, 16, 128, 5.0);
#else
    // Not the same "absent dependency" as protobuf's: ion-c is installed here,
    // but CONFIG_ION_SERIALIZER is unset in every checked-in defconfig, so this
    // row needs a build configured for it rather than a different vcpkg tree.
    BOOST_TEST_MESSAGE(
        "  ion row: NOT RUN (KYTHIRA_BENCH_HAS_ION undefined -- needs "
        "CONFIG_ION_SERIALIZER)");
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

BOOST_AUTO_TEST_CASE(write_throughput_by_value_size,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
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

// ── the concurrency axis ─────────────────────────────────────────────────────

/// @brief How many operations a concurrency row offers.
///
/// Not a constant, and that is the whole point of the helper.
/// `run_put_workload` splits the budget across the workers, so a fixed budget
/// at 64 in flight leaves each worker six operations and a measured window that
/// is mostly thread start-up and join. Twenty-five operations per worker keeps
/// every row's window in the same order of magnitude as the rest of the suite's
/// rows; the floor of 400 keeps the single-threaded row, which is the slowest,
/// from being the shortest as well.
///
/// Rows with different budgets stay comparable because the reported quantity is
/// a rate — the same reason `write_throughput_by_transport` gives cpp-httplib a
/// smaller budget than Beast.
auto concurrency_budget(std::size_t in_flight) -> std::size_t {
    return std::max<std::size_t>(400, 25 * in_flight);
}

/// @brief Throughput against in-flight concurrency, in both key distributions.
///
/// Two arms, because Requirement 8.7 asks for **single-group** throughput as a
/// function of concurrency and the uniform arm does not provide it: uniform
/// keys over a four-shard tiling spread the load across four `node<Types>`
/// instances, so its curve is the cluster's, and H7 is a claim about one
/// group's mutex. The Zipfian arm at theta 0.99 puts almost every key in the
/// lowest shard, which is as close to a single-group curve as this fixture
/// gets without changing the cluster shape every other row shares.
///
/// Read together the two arms say more than either alone: the gap between them
/// at a given concurrency is what per-group serialization costs, measured
/// rather than asserted.
BOOST_AUTO_TEST_CASE(write_throughput_by_concurrency,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "write throughput by client concurrency, 128B values, JSON on the wire "
        "(H7: the concurrency at which single-group throughput stops rising; "
        "H1/H2: batching and RPC cost as a function of it):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    for (auto distribution : {key_distribution::uniform, key_distribution::zipfian}) {
        for (std::size_t in_flight : {std::size_t{1}, std::size_t{8}, std::size_t{64}}) {
            // The floor is per-row rather than shared. One in-flight operation
            // is one round trip at a time by definition, and the Zipfian arm is
            // expected to be the slower of the two — holding either to the
            // sixteen-way uniform row's floor would fail it for being what it
            // is, which is the opposite of what a sanity floor is for.
            const double floor = in_flight == 1 ? 1.0 : 3.0;
            std::ignore = throughput_row<kythira::testing::beast_http_transport<json>>(
                concurrency_budget(in_flight), in_flight, 128, floor, distribution);
        }
    }
#else
    BOOST_TEST_MESSAGE("  concurrency sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the key-distribution axis ────────────────────────────────────────────────

/// @brief Uniform against Zipfian at the concurrency every other row uses.
///
/// The concurrency case above already runs both distributions, so this row is
/// not the only place the comparison appears. It exists because its uniform arm
/// is bit-for-bit the configuration of the transport, serializer and value-size
/// rows — 600 operations, 16 in flight, 128 B, JSON — which is what lets the
/// Zipfian number be quoted against those tables rather than only against
/// itself. The arms run back to back in one process on one cluster shape, so
/// the control is contemporaneous with the treatment.
BOOST_AUTO_TEST_CASE(write_throughput_by_key_distribution,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "write throughput by key distribution, 128B values, 16 in flight, JSON on the wire:");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::ignore = throughput_row<kythira::testing::beast_http_transport<json>>(
        600, 16, 128, 5.0, key_distribution::uniform);
    std::ignore = throughput_row<kythira::testing::beast_http_transport<json>>(
        600, 16, 128, 2.0, key_distribution::zipfian);
#else
    BOOST_TEST_MESSAGE("  distribution sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the read taxonomy ────────────────────────────────────────────────────────

/// @brief One read repetition: a fresh cluster, a preloaded store, one measured
///        window of one read kind.
///
/// The preload is inside the repetition, not shared across the five, because a
/// repetition is a whole measurement (doctrine 43) and a store loaded once and
/// read five times would measure a progressively warmer cache against a
/// progressively less representative cluster.
///
/// `preloaded` is asserted rather than assumed. A read row over a store that is
/// nine-tenths loaded measures something other than what it claims, and the
/// only way to know is to count what committed.
template<typename Transport>
auto one_read_measurement(read_kind kind, std::uint64_t distinct_keys, std::uint64_t stride,
                          std::size_t value_bytes, std::size_t operations, std::size_t in_flight)
    -> benchmark_result {
    kv_cluster<Transport> cluster{standard_cluster_options()};
    BOOST_REQUIRE_MESSAGE(cluster.await_all_leaders(k_election_budget),
                          Transport::name() << ": no leader on every shard within budget");

    operation_tally preload_tally;
    const auto preloaded = preload_keys(cluster, distinct_keys, stride, value_bytes,
                                        k_operation_timeout, preload_tally);
    BOOST_REQUIRE_MESSAGE(preloaded == distinct_keys,
                          Transport::name()
                              << ": preloaded only " << preloaded << " of " << distinct_keys
                              << " keys; this row would measure a miss path");

    workload_options workload;
    workload._in_flight = in_flight;
    workload._operations = operations;
    workload._value_bytes = value_bytes;
    workload._key_count = distinct_keys;
    workload._key_stride = stride;
    workload._distribution = key_distribution::uniform;
    workload._op_timeout = k_operation_timeout;
    workload._scenario = std::string{to_string(kind)};

    workload_options warmup = workload;
    warmup._operations = warmup_operations(operations, in_flight);
    std::ignore = run_read_workload(cluster, warmup, kind);

    auto row = run_read_workload(cluster, workload, kind);

    BOOST_CHECK_MESSAGE(row._tally._completed > 0, Transport::name()
                                                       << " / " << to_string(kind)
                                                       << ": no read completed at all");
    return row;
}

/// @brief `k_required_repetitions` read measurements, reported as one row.
template<typename Transport>
auto read_row(read_kind kind, std::uint64_t distinct_keys, std::uint64_t stride,
              std::size_t value_bytes, std::size_t operations, std::size_t in_flight)
    -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(operations, in_flight);
    row._measured_operations = operations;
    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = one_read_measurement<Transport>(kind, distinct_keys, stride, value_bytes,
                                                   operations, in_flight);
        report(repetition, run);
        row.record(std::move(run));
    }
    report(row);
    return row;
}

/// @brief How many operations a read row of each kind offers.
///
/// Per-kind for the same reason `write_throughput_by_transport` gives
/// cpp-httplib a smaller budget than Beast: throughput is a rate, so unequal
/// budgets stay comparable, and an equal budget does not. A local read costs
/// about a microsecond, so 400 of them complete in roughly a millisecond — a
/// window in which starting and joining eight threads is most of what is being
/// timed. Measured that way the local row came back at 44% spread, which said
/// nothing about the system and everything about the budget.
auto read_budget(read_kind kind) -> std::size_t {
    switch (kind) {
        case read_kind::local_stale:
            // ~1 us per read, so this is roughly half a second of work.
            return 200000;
        case read_kind::read_state:
        case read_kind::log_get:
            return 400;
    }
    return 400;
}

/// @brief The three read kinds, measured separately and never aggregated.
///
/// No sanity floor here, and that is deliberate: the three kinds differ by
/// orders of magnitude by construction — a local read touches one map, a `GET`
/// costs a log entry and a replication round, and `read_state` serializes the
/// whole store — so a floor low enough for the slowest would say nothing about
/// the fastest. What the row asserts instead is that reads *completed*, and
/// what it reports is the kind, the consistency and the bytes, which is what
/// Requirement 2.2 asks a read result to state.
BOOST_AUTO_TEST_CASE(read_taxonomy,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "read taxonomy, 1000 preloaded 128B keys spread over all four shards, 8 in flight "
        "(Requirement 2.1: three kinds, reported separately):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    // 1000 keys at stride 100 covers the 100000-key space evenly, so every
    // shard holds 250 of them and a uniform sampler hits all four.
    for (auto kind : {read_kind::read_state, read_kind::log_get, read_kind::local_stale}) {
        std::ignore = read_row<kythira::testing::beast_http_transport<json>>(kind, 1000, 100, 128,
                                                                             read_budget(kind), 8);
    }
#else
    BOOST_TEST_MESSAGE("  read taxonomy: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

/// @brief `read_state` against shard size — H5's curve rather than H5's point.
///
/// Stride 1, so the preloaded keys are contiguous and land in **one** shard.
/// That is the configuration H5 is about: `read_state` returns the whole
/// serialized store for the shard it is asked about, so its cost should track
/// that shard's size and not the cluster's.
///
/// Requirement 2.4 wants ops/sec **and** bytes/sec, and this is the sweep that
/// shows why: if H5 holds, ops/sec falls as the shard grows while bytes/sec
/// stays roughly flat — the machine doing the same work per second and less of
/// it per operation. A row reported only in ops/sec would look like a
/// regression instead of a transfer.
BOOST_AUTO_TEST_CASE(read_state_by_shard_size,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "read_state against shard size, 128B values, 8 in flight, one shard "
        "(H5: read cost scales with shard size):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    for (std::uint64_t keys : {std::uint64_t{100}, std::uint64_t{1000}, std::uint64_t{5000}}) {
        // Fewer operations at the larger shards: each one serializes and
        // transfers the entire store, so a fixed budget would make the 5000-key
        // row dominate the case's wall-clock without telling us anything the
        // rate does not.
        const std::size_t operations = keys >= 5000 ? 80 : 200;
        std::ignore = read_row<kythira::testing::beast_http_transport<json>>(
            read_kind::read_state, keys, 1, 128, operations, 8);
    }
#else
    BOOST_TEST_MESSAGE(
        "  read_state shard-size curve: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

BOOST_AUTO_TEST_SUITE_END()
