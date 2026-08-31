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
/// It **was** roughly two orders of magnitude slower than the other two, for a
/// cause this project documented for a year and then worked around four times
/// rather than fixing once: cpp-httplib's vendored header defaults
/// `CPPHTTPLIB_TCP_NODELAY` to `false`, so every small RPC body paid the
/// classic ~80ms Nagle/delayed-ACK round trip
/// (`doc/http_transport_performance_comparison.md`, which measured 12 ops/sec
/// against Beast's 3,527 on a bare ping-pong). Beast and Proxygen sit on
/// Asio/Folly `AsyncSocket`, which set `TCP_NODELAY` themselves.
///
/// **`cpp_httplib_client_config::tcp_nodelay` now defaults to `true`.** The old
/// text called it "a configuration default, not a verdict on the transport",
/// which was exactly right and stopped one step short: it was *this project's*
/// configuration of that transport, and one line in `http_transport_impl.hpp`
/// at each of three call sites.
///
/// **Both arms are reproducible from this binary**, which is the point of the
/// environment override rather than a rebuild:
///
///     ./multi_raft_http_benchmark_test \
///         --run_test=multi_raft_http_benchmark/write_throughput_by_transport
///     KYTHIRA_BENCH_TCP_NODELAY=0 ./multi_raft_http_benchmark_test \
///         --run_test=multi_raft_http_benchmark/write_throughput_by_transport
///
/// Five repetitions each, on the development machine:
///
///     KYTHIRA_BENCH_TCP_NODELAY=0    11.5 ops/sec, p50 250.1 ms, spread 34.5%,
///                                    verdict UNSTABLE
///     default                       369.4 ops/sec, p50   9.8 ms, spread  4.4%,
///                                    verdict stable
///
/// **The row became quotable for the first time**, which matters more than the
/// 32x: Requirement 6.3 keeps an unstable row out of every comparison table,
/// and this one had never cleared that bar.
///
/// **Nagle was not only slowing this transport, it was breaking leadership**,
/// and a throughput number alone would have hidden that. A 40 ms stall on every
/// heartbeat deposes a leader that is merely mid-heartbeat. That is why the
/// design raised this suite's election timeout to 2000-4000 ms, why this row's
/// operation budget is 24 rather than 592, and why the serializer sweep skips
/// cpp-httplib entirely — three workarounds whose combined cost exceeded the
/// fix.
///
/// Those three are now **revisitable and deliberately not revisited here**:
/// re-tuning budgets and timeouts moves every other row in this file, and a fix
/// and a re-tune landing together leave nobody able to say which produced the
/// difference.
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

#include "multi_raft_benchmark_rows.hpp"
#include "multi_raft_row_report.hpp"

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
#include <array>
#include <chrono>
#include <cstdlib>
#include <cmath>
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
using kythira::testing::deployment_tier;
using kythira::testing::describe_machine;
using kythira::testing::describe_row;
using kythira::testing::fabric_transport;
using kythira::testing::k_election_budget;
using kythira::testing::k_operation_timeout;
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
using kythira::testing::publishable_as_like_for_like;
using kythira::testing::read_kind;
using kythira::testing::read_row_spec;
using kythira::testing::repeated_result;
using kythira::testing::routing_mode;
using kythira::testing::standard_cluster_options;
using kythira::testing::to_string;
using kythira::testing::us;
using kythira::testing::write_row_spec;

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

/// One row, printed in the shape the comparison document wants.
///
/// The formatting moved to `multi_raft_row_report.hpp` when
/// `.kiro/specs/multi-raft-host-binary/` added a third consumer that produces
/// rows: its Requirement 3.4 asks the out-of-process driver to emit the same
/// fields a Tier B row carries, and two printers would satisfy that on the day
/// they were written and drift the week after.
auto report(const repeated_result& row) -> void {
    BOOST_TEST_MESSAGE(describe_row(row, machine()._quiet_at_start));
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

/// @brief Where this suite's rows send their output, and what a failed
///        precondition does here.
///
/// `_require` is `BOOST_REQUIRE` because a cluster that never elected has not
/// measured anything and the case should stop; `_check` is `BOOST_CHECK`
/// because a term that moved or a floor that was missed is a fact worth
/// recording without abandoning the rest of the matrix. The report binary
/// installs the same three callbacks over `std::cout` and an exception, and
/// that is the only thing that differs between the two consumers.
auto suite_observer() -> kythira::testing::row_observer {
    return kythira::testing::row_observer{
        ._message = [](const std::string& text) { BOOST_TEST_MESSAGE(text); },
        ._require = [](bool ok, const std::string& why) { BOOST_REQUIRE_MESSAGE(ok, why); },
        ._check = [](bool ok, const std::string& why) { BOOST_CHECK_MESSAGE(ok, why); },
    };
}

/// @brief One row, measured and then printed in the shape the comparison
///        document wants.
///
/// The measurement itself is `kythira::testing::throughput_row`, shared with
/// the report binary so the two cannot describe different work; what this adds
/// is Boost.Test's reporting and the printed row.
template<typename Transport> auto measure(const write_row_spec& spec) -> repeated_result {
    auto row = kythira::testing::throughput_row<Transport>(spec, suite_observer());
    report(row);
    return row;
}

template<typename Transport> auto measure(const read_row_spec& spec) -> repeated_result {
    auto row = kythira::testing::read_row<Transport>(spec, suite_observer());
    report(row);
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

    // cpp-httplib now runs the **same budget and the same concurrency** as the
    // other two, and that is the whole point of this row.
    //
    // It ran 24 operations at four in flight for as long as its round trip was
    // ~83 ms — a full-sized budget took tens of seconds. That round trip was
    // Nagle, and `tcp_nodelay` removed it. Until this change the axis was
    // therefore not a comparison at all: Requirement 11 asks that everything
    // but the swept axis be held identical, and three rows at two different
    // concurrencies hold nothing identical. The axis compares transports now.
    //
    // The floor moves with it, from 0.2 to 20. 0.2 was chosen when the row was
    // expected to manage single digits; leaving it there would mean a row that
    // had regressed by a factor of a thousand still passed. 20 is an order of
    // magnitude below the 369.4 measured here, which is what a floor is for —
    // it catches a cluster that elected and then committed almost nothing, and
    // nothing else.
    std::ignore = measure<cpp_httplib_transport<json>>({._floor_ops_per_second = 20.0});

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::ignore =
        measure<kythira::testing::beast_http_transport<json>>({._floor_ops_per_second = 5.0});
#endif
#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
    std::ignore =
        measure<kythira::testing::proxygen_http_transport<json>>({._floor_ops_per_second = 5.0});
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

    // **cpp-httplib carries this axis now, and did not before.**
    //
    // The sweep used to run on Beast alone, with a comment explaining that
    // cpp-httplib's ~83 ms round trip dwarfed every encoding difference — the
    // rows would have measured Nagle rather than the serializer. That was
    // correct, and `tcp_nodelay` removed the reason: the same transport
    // measures a 9.8 ms p50, and an encoding difference is now a visible
    // fraction of a round trip rather than noise on top of a tenth of a second.
    //
    // Running it on **two transports** is the point rather than a bonus.
    // Hypothesis H3 is about the encoding, and an encoding effect that appears
    // on one transport and not on another is a transport effect wearing an
    // encoding's name. Until now this axis could not tell those apart, because
    // it only ever had one transport to look at.
    std::ignore = measure<cpp_httplib_transport<json>>({._floor_ops_per_second = 20.0});
    std::ignore = measure<cpp_httplib_transport<cbor>>({._floor_ops_per_second = 20.0});
#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
    // Nothing about protobuf is transport-specific — the fixture is templated
    // on the serializer exactly as Beast's is, and the harness gates protobuf
    // nowhere. Carrying it on both transports is what lets the JSON/CBOR
    // spread below be read as an encoding effect rather than a binary-versus-
    // text one: protobuf is the second binary encoding, and if the effect were
    // about binary framing it would show here too.
    std::ignore = measure<cpp_httplib_transport<kythira::protobuf_serializer>>(
        {._floor_ops_per_second = 20.0});
#else
    BOOST_TEST_MESSAGE(
        "  cpp-httplib protobuf row: NOT RUN (KYTHIRA_BENCH_HAS_PROTOBUF undefined)");
#endif

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::ignore =
        measure<kythira::testing::beast_http_transport<json>>({._floor_ops_per_second = 5.0});
    std::ignore =
        measure<kythira::testing::beast_http_transport<cbor>>({._floor_ops_per_second = 5.0});
#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
    std::ignore = measure<kythira::testing::beast_http_transport<kythira::protobuf_serializer>>(
        {._floor_ops_per_second = 5.0});
#else
    BOOST_TEST_MESSAGE("  protobuf row: NOT RUN (KYTHIRA_BENCH_HAS_PROTOBUF undefined)");
#endif
#if defined(KYTHIRA_BENCH_HAS_ION)
    std::ignore =
        measure<kythira::testing::beast_http_transport<ion>>({._floor_ops_per_second = 5.0});
#else
    // Not the same "absent dependency" as protobuf's: ion-c is installed here,
    // but CONFIG_ION_SERIALIZER is unset in every checked-in defconfig, so this
    // row needs a build configured for it rather than a different vcpkg tree.
    BOOST_TEST_MESSAGE(
        "  ion row: NOT RUN (KYTHIRA_BENCH_HAS_ION undefined -- needs "
        "CONFIG_ION_SERIALIZER)");
#endif
#else
    // A build without Beast is no longer a build without this axis. It used to
    // report the whole sweep as unmeasured, which is Requirement 13.4's failure
    // one level up — a shorter, entirely plausible table with an axis missing
    // rather than a row.
    BOOST_TEST_MESSAGE(
        "  beast rows: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined). The "
        "cpp-httplib rows above carry the axis.");
#endif
}

// ── the payload axis ─────────────────────────────────────────────────────────

/// @brief The value-size sweep read across rows, in the quantities that
///        distinguish "the payload got bigger" from "the round got slower" from
///        "the same entry went out more times".
///
/// The sweep itself has been in this suite since task 6 and produced a finding
/// nobody could explain: throughput falls **fourteen-fold** between 1 KiB and
/// 4 KiB, where the payload grows four-fold, and the 4 KiB row was the only
/// unstable one of the four. A four-fold payload buying a fourteen-fold
/// throughput loss is not "bigger messages cost more".
///
/// Task 11 supplied the hypothesis this print was built to test — that the round
/// is paced by the response, so a larger value inflates the round *trip* rather
/// than the round *count*. The columns below refute it: at 4 KiB the round
/// interval rises 2.3x and the entry-bearing rounds per commit rise 4.4x, and
/// the product is the whole cliff.
///
/// ### What each column is, and what it is not
///
/// `AE/commit` counts **entry-bearing** AppendEntries only, not heartbeats.
/// That separation is the first thing this print had to get right: a commit
/// that takes six times longer accumulates six times the background heartbeats,
/// which would look like a replication regression and be nothing but a
/// consequence of the slowness. On the row that motivated this, empty
/// AppendEntries were 292 of 6828 — 4.3%, and not the explanation.
///
/// `entry-sends/commit` is `entries / commits`, where `entries` counts an entry
/// once **per send**, so an entry going to two followers counts twice. Its floor
/// is therefore `nodes - 1`: two, for the three-node cluster every row here
/// uses. `amplification` is the ratio to that floor — how many times the average
/// entry crossed the wire beyond the once-per-follower a commit requires.
///
/// `payload/round` is `value_bytes x entries / AppendEntries`: the entry bytes
/// one AppendEntries carried. It is a **lower bound on wire bytes**, not the
/// wire bytes — it counts the value and not the key, the command framing, the
/// log-entry envelope, JSON's expansion, or HTTP headers. Stating what it
/// excludes is cheaper than a byte counter on the send path that Requirement 8.2
/// would not want anyway.
///
/// ### One column is an identity, and says so
///
/// `ops/sec` is `1000 x streams / (round interval x AE/commit-including-empty)`
/// by construction, so the two ratios multiplying out to the throughput ratio is
/// arithmetic rather than evidence. What the decomposition buys is
/// **attribution** — which of the two factors moved, and by how much — and that
/// is not arithmetic.
auto report_value_size_sweep(const std::vector<repeated_result>& rows,
                             std::string_view what = "the value-size sweep") -> void {
    if (rows.empty()) {
        return;
    }
    const auto& base = rows.front().median_run();
    const auto ratio = [](double value, double reference) -> std::string {
        std::ostringstream r;
        r << std::fixed << std::setprecision(2);
        if (reference <= 0.0) {
            r << "n/a";
        } else {
            r << (value / reference) << "x";
        }
        return r.str();
    };
    const auto streams = [](const benchmark_result& r) -> std::size_t {
        return r._groups * (r._nodes > 0 ? r._nodes - 1 : 0);
    };
    const auto inter_round_ms = [&streams](const benchmark_result& r) -> double {
        const auto seconds = std::chrono::duration<double>(r._duration).count();
        const auto s = static_cast<double>(streams(r));
        if (seconds <= 0.0 || s <= 0.0 || r._rpc._append_entries == 0) {
            return 0.0;
        }
        return 1000.0 * seconds * s / static_cast<double>(r._rpc._append_entries);
    };
    /// Entry-bearing rounds per committed entry — heartbeats excluded.
    const auto carrying_per_commit = [](const benchmark_result& r) -> double {
        return r._tally._completed == 0 ? 0.0
                                        : static_cast<double>(r._rpc.carrying()) /
                                              static_cast<double>(r._tally._completed);
    };
    const auto sends_per_commit = [](const benchmark_result& r) -> double {
        return r._tally._completed == 0 ? 0.0
                                        : static_cast<double>(r._rpc._entries) /
                                              static_cast<double>(r._tally._completed);
    };
    /// Sends beyond the once-per-follower a commit cannot avoid.
    const auto amplification = [&sends_per_commit](const benchmark_result& r) -> double {
        const auto floor = static_cast<double>(r._nodes > 1 ? r._nodes - 1 : 1);
        return sends_per_commit(r) / floor;
    };
    const auto payload_per_round = [](const benchmark_result& r) -> double {
        return r._rpc._append_entries == 0
                   ? 0.0
                   : static_cast<double>(r._value_bytes) * static_cast<double>(r._rpc._entries) /
                         static_cast<double>(r._rpc._append_entries);
    };

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << what
        << " read across rows — every figure is the median run's, and "
           "the ratio is against the "
        << base._value_bytes << " B row:"
        << "\n      value      ops/sec        AE/commit      round interval    ent/AE   "
           "entry-sends/commit   amplification   payload/round";
    for (const auto& row : rows) {
        const auto& m = row.median_run();
        out << "\n      " << std::setw(5) << m._value_bytes << "B  " << std::setw(8)
            << std::setprecision(1) << m._ops_per_second << " ("
            << ratio(m._ops_per_second, base._ops_per_second) << ")  " << std::setprecision(2)
            << std::setw(6) << carrying_per_commit(m) << " ("
            << ratio(carrying_per_commit(m), carrying_per_commit(base)) << ")  " << std::setw(6)
            << inter_round_ms(m) << " ms (" << ratio(inter_round_ms(m), inter_round_ms(base))
            << ")  " << std::setw(5) << m._rpc.entries_per_append_entries().value_or(0.0) << "  "
            << std::setw(8) << sends_per_commit(m) << "  " << std::setw(8) << amplification(m)
            << "x over the " << (m._nodes > 1 ? m._nodes - 1 : 1) << "-per-follower floor  "
            << std::setw(9) << payload_per_round(m) << " B";
        if (!row.comparable()) {
            out << "  [" << to_string(row.verdict()) << "]";
        }
    }
    out << "\n      AE/commit counts ENTRY-BEARING rounds only. A commit that takes six times "
           "longer accumulates six times the heartbeats, which would read as a replication "
           "regression and be nothing but a consequence of the slowness."
        << "\n      Task 11's hypothesis — a larger value inflates the round TRIP, not the round "
           "COUNT — is refuted by any row whose AE/commit is not flat."
        << "\n      `payload/round` counts entry VALUES only: not keys, command framing, the "
           "log-entry envelope, JSON's expansion, or HTTP headers. A lower bound on wire bytes, "
           "here to show which way the per-round payload moves rather than how large it is.";
    BOOST_TEST_MESSAGE(out.str());
}

BOOST_AUTO_TEST_CASE(write_throughput_by_value_size,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE("write throughput by value size, JSON on the wire:");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::vector<repeated_result> rows;
    // Requirement 1.4 names 16 B, 128 B, 1 KiB and 4 KiB as the minimum. 2 KiB
    // is here for a reason the first four rows produced: write amplification is
    // flat at 8.8-10.9 entry-sends per follower from 16 B through 1 KiB and then
    // jumps to 37-38 at 4 KiB, so *something* changes between those two points
    // and the sweep as specified brackets it only to within a factor of four.
    for (std::size_t bytes : {std::size_t{16}, std::size_t{128}, std::size_t{1024},
                              std::size_t{2048}, std::size_t{4096}}) {
        rows.push_back(measure<kythira::testing::beast_http_transport<json>>(
            {._operations = 400, ._value_bytes = bytes, ._floor_ops_per_second = 2.0}));
    }
    report_value_size_sweep(rows, "the value-size sweep (JSON on the wire)");
#else
    BOOST_TEST_MESSAGE("  value-size sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

/// @brief The two encodings' amplification set side by side, at each value size.
///
/// One table rather than asking a reader to difference the two above. The
/// column that answers the question is `CBOR/JSON amplification`: at or near
/// 1.00x the knee is not about encoded size, and materially below 1.00x it is.
///
/// The throughput ratio is printed beside it and is **not** the answer to the
/// same question — CBOR encodes and decodes faster than JSON at every size
/// `doc/protobuf_serializer_performance_comparison.md` measured, so it would be
/// expected to win on throughput even if the knee were entirely about something
/// else. Amplification is a count of sends and does not care how fast each one
/// was.
auto report_encoding_comparison(const std::vector<repeated_result>& json_rows,
                                const std::vector<repeated_result>& cbor_rows) -> void {
    if (json_rows.size() != cbor_rows.size() || json_rows.empty()) {
        return;
    }
    const auto amplification = [](const benchmark_result& r) -> double {
        if (r._tally._completed == 0) {
            return 0.0;
        }
        const auto floor = static_cast<double>(r._nodes > 1 ? r._nodes - 1 : 1);
        return static_cast<double>(r._rpc._entries) / static_cast<double>(r._tally._completed) /
               floor;
    };

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "does the retransmission knee move with the ENCODED size? JSON against CBOR:"
        << "\n      value     JSON ampl   CBOR ampl   CBOR/JSON   JSON ops/sec   CBOR ops/sec   "
           "CBOR/JSON";
    for (std::size_t i = 0; i < json_rows.size(); ++i) {
        const auto& j = json_rows[i].median_run();
        const auto& c = cbor_rows[i].median_run();
        const auto ja = amplification(j);
        const auto ca = amplification(c);
        out << "\n      " << std::setw(5) << j._value_bytes << "B   " << std::setw(8) << ja
            << "x   " << std::setw(8) << ca << "x   " << std::setw(8) << (ja > 0.0 ? ca / ja : 0.0)
            << "x   " << std::setw(11) << std::setprecision(1) << j._ops_per_second << "   "
            << std::setw(11) << c._ops_per_second << "   " << std::setw(8) << std::setprecision(2)
            << (j._ops_per_second > 0.0 ? c._ops_per_second / j._ops_per_second : 0.0) << "x";
        if (!json_rows[i].comparable() || !cbor_rows[i].comparable()) {
            out << "  [one or both UNSTABLE]";
        }
    }
    out << "\n      `CBOR/JSON amplification` at or near 1.00x says the knee is NOT about "
           "encoded size; materially below 1.00x says it is."
        << "\n      The throughput ratio does not answer the same question: CBOR encodes and "
           "decodes faster than JSON at every size the serializer comparison measured, so it "
           "would win on throughput even if the knee were about something else entirely."
        << "\n      This case cannot separate `encoded size` from anything else that differs "
           "between the two serializers, so a CBOR improvement is evidence for the size "
           "hypothesis rather than proof of it.";
    BOOST_TEST_MESSAGE(out.str());
}

// ── the encoding's share of the amplification knee ───────────────────────────

/// @brief Is the 2 KiB → 4 KiB retransmission knee a function of the *encoded*
///        size? JSON against CBOR, on the same value sizes.
///
/// Task 11a left exactly one thing open. Write amplification is flat from 16 B
/// to 1 KiB (8.8–10.7 entry-sends per follower per commit), 14–15 at 2 KiB and
/// 62–76 at 4 KiB: one doubling costs a 4.4–5.4x rise in retransmission that no
/// other doubling on the axis does. *Why that doubling* was not measured, and
/// the obvious instrument — a byte counter on the send path — is exactly what
/// Requirement 8.2 keeps out of production code.
///
/// This case asks the question without one, by changing the encoded size while
/// holding the value size fixed. `json_rpc_serializer` base64-expands a byte
/// array by 4/3 and then quotes it, so a 4 KiB value is roughly 5.5 KiB on the
/// wire before framing; `cbor_serializer` writes byte strings natively, so the
/// same value is roughly 4 KiB. `doc/protobuf_serializer_performance_comparison.md`
/// measured the same effect from the other side: a 256-byte command is 527
/// bytes of JSON against 282 of protobuf.
///
/// **The prediction, stated before the run.** If the knee is a threshold in
/// *encoded* bytes, CBOR's smaller payload should push it out — the CBOR 4 KiB
/// row should behave like JSON's does somewhere below 4 KiB, and its
/// amplification should be materially lower. If the knee is in the *value* size
/// — a per-entry cost, a copy, an allocator — the two encodings should knee at
/// the same place and CBOR should buy little.
///
/// Either answer is worth having, and neither needs a counter in production
/// code. What this case cannot do is separate "encoded size" from "anything else
/// that differs between the two serializers", so a CBOR improvement is evidence
/// for the size hypothesis rather than proof of it — Requirement 8.4's own
/// caveat, applied here.
///
/// Three value sizes rather than five: 1 KiB is below the knee in both arms and
/// anchors them, and 2 KiB and 4 KiB are the doubling in question.
BOOST_AUTO_TEST_CASE(write_amplification_by_encoding,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "write amplification by wire encoding, 16 in flight (does the 2 KiB -> 4 KiB "
        "retransmission knee move with the ENCODED size?):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    constexpr std::array<std::size_t, 3> k_sizes{1024, 2048, 4096};

    std::vector<repeated_result> json_rows;
    for (auto bytes : k_sizes) {
        json_rows.push_back(measure<kythira::testing::beast_http_transport<json>>(
            {._operations = 400, ._value_bytes = bytes, ._floor_ops_per_second = 2.0}));
    }
    report_value_size_sweep(json_rows, "JSON on the wire");

    std::vector<repeated_result> cbor_rows;
    for (auto bytes : k_sizes) {
        cbor_rows.push_back(measure<kythira::testing::beast_http_transport<cbor>>(
            {._operations = 400, ._value_bytes = bytes, ._floor_ops_per_second = 2.0}));
    }
    report_value_size_sweep(cbor_rows, "CBOR on the wire");

    report_encoding_comparison(json_rows, cbor_rows);
#else
    BOOST_TEST_MESSAGE(
        "  encoding/amplification cross: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── durability ───────────────────────────────────────────────────────────────

/// @brief The three persistence configurations side by side, with the two
///        numbers Requirement 3.4 asks a durable row to carry.
///
/// `fsync/sec/host` and `entries/fsync` are the pair, and they have to be read
/// together: a system that fsyncs rarely because it batches well and one that
/// fsyncs rarely because it is doing nothing look identical in the first column
/// and nothing alike in the second.
///
/// The `barriered` column is the honest one and says so in the table rather
/// than only in a doc comment: it is the fraction of appended entries a
/// completed barrier covered before the node advertised them, and **only 100%
/// is a durable row**. It read 19.9% and 24.5% when this axis was first
/// measured, with a `tick_batch_controller` supplied exactly as
/// `multi_raft.hpp` then documented; `.kiro/specs/durable-append-barrier/`
/// moved the barrier to the boundary where `node` advertises an append and it
/// reads 100% now. The column stays because a durability claim that is not a
/// number is the thing that let the first one go unnoticed.
auto report_durability_comparison(const std::vector<repeated_result>& rows) -> void {
    if (rows.empty()) {
        return;
    }
    const auto seconds = [](const benchmark_result& r) -> double {
        return std::chrono::duration<double>(r._duration).count();
    };
    const auto fsyncs_per_second_per_host = [&seconds](const benchmark_result& r) -> double {
        const auto t = seconds(r);
        if (t <= 0.0 || r._nodes == 0) {
            return 0.0;
        }
        return static_cast<double>(r._durability_counts._barriers) / t /
               static_cast<double>(r._nodes);
    };

    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "durability (Requirements 3.4, 3.5) — every figure is the median run's, at a "
        << rows.front().median_run()._tick_interval.count() << " ms tick:"
        << "\n      mode                          ops/sec     p50        fsync/sec/host   "
           "entries/fsync   barriered   barriers   empty batches   entries";
    for (const auto& row : rows) {
        const auto& m = row.median_run();
        out << "\n      " << std::left << std::setw(28) << to_string(m._durability) << std::right
            << std::setw(9) << std::setprecision(1) << m._ops_per_second << "  " << std::setw(9)
            << std::chrono::duration_cast<std::chrono::microseconds>(m._p50).count() << "us  "
            << std::setw(14) << std::setprecision(2) << fsyncs_per_second_per_host(m) << "  "
            << std::setw(14) << m._durability_counts.entries_per_barrier() << "  " << std::setw(9)
            << std::setprecision(1) << 100.0 * m._durability_counts.barriered_fraction() << "%  "
            << std::setprecision(2) << std::setw(9) << m._durability_counts._barriers << "  "
            << std::setw(14) << m._durability_counts._empty_batches << "  " << std::setw(9)
            << m._durability_counts._entries;
        if (!row.comparable()) {
            out << "  [" << to_string(row.verdict()) << "]";
        }
    }
    out << "\n      `barriered` is the fraction of appended entries a completed barrier covered "
           "before the node advertised them, and it is the column that decides whether a row is "
           "durable at all. **Only 100% is a durable row.** It read 19.9% and 24.5% before "
           "`.kiro/specs/durable-append-barrier/`, when the barrier was taken around "
           "`multi_raft::tick()`'s persist phase — the tick runs on the host's driver thread and "
           "the appends run on the client's and the transport's, so a barrier there covered "
           "whatever happened to race into its window."
        << "\n      `entries/fsync` counts ONLY the covered entries, so it cannot be inflated by "
           "entries no barrier reached. That is not a hypothetical: this axis reported 8.45 "
           "entries per fsync on its first draft, over a configuration where a barrier had "
           "touched a fifth of them."
        << "\n      `file/buffered` issues NO barrier at all and its zeroes are not a "
           "measurement error — the engine is constructed with barriers disabled, so the log "
           "file is written and the stream flushed and no fsync is ever requested. It survives a "
           "process death and loses everything to a power cut. Requirement 3.5 requires that row "
           "be read as NOT DURABLE, and `file_persistence_engine::durability()` answers "
           "`buffered` for it rather than leaving a reader to remember."
        << "\n      `empty batches` is the group-commit yield: barrier requests that issued no "
           "syscall because a concurrent barrier had already covered the caller's sequence. A "
           "large number here against a small `barriers` is coalescing working."
        << "\n      The memory row is the control and the ONLY one of the three whose durability "
           "barrier is genuinely zero. It is here to price the other two, not to be quoted.";
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief What durability costs, now that it is real.
///
/// **This is NOT Tier D**, and the distinction is the first thing the row has
/// to carry. Requirement 3.1 defines Tier D as *Tier C plus* `file_persistence`
/// in `barrier` mode — Tier C being one host process per node — and every row
/// in this suite is Tier B, with all three hosts inside one process. What this
/// case delivers is Tier B with a real durability barrier: the fsync is real,
/// the volume is real, and the process separation is not. Requirement 3.3 still
/// forbids a like-for-like external comparison from it, and Requirement 3.7
/// asks that the missing tier be named rather than the claim quietly narrowed.
///
/// Three arms. The middle one, `file_buffered`, is the file-backed path with
/// barriers switched off: it pays the JSON encode and the write and none of the
/// fsync, which is what makes the two halves of the cost separable. It is not
/// durable and every row that carries it says so.
///
/// **This axis is where the durability defect was found**, and the history is
/// worth keeping beside the numbers. Its first run reported `barriered` at
/// 19.9% and 24.5% over two runs — a `tick_batch_controller` supplied exactly
/// as `multi_raft.hpp` then documented, covering a fifth of the appends
/// because the tick is not where appends happen. That produced
/// `.kiro/specs/durable-append-barrier/`, which moved the barrier to the
/// boundary where `node` advertises an append. This axis now reads **100%**,
/// and the `barriered` column stays because a durability claim that is not a
/// number is exactly what let the first one stand.
///
/// The prediction, written before the run: `file/buffered` should cost little
/// against `memory` — an ofstream append and a flush to page cache — and
/// `file/barrier` should cost a great deal, because every advertised append now
/// waits on a real barrier and group commit only coalesces the appends that are
/// concurrently outstanding. If `file/barrier` is *not* much slower, the barrier
/// is not reaching the device and the row is wrong rather than good.
BOOST_AUTO_TEST_CASE(write_throughput_by_durability,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "write throughput by durability mode, 128B values, 16 in flight (Requirement 3.4):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::vector<repeated_result> rows;
    for (auto mode : {kythira::testing::durability_mode::memory,
                      kythira::testing::durability_mode::file_buffered,
                      kythira::testing::durability_mode::file_barrier}) {
        auto cluster = kythira::testing::standard_cluster_options();
        cluster._durability = mode;
        // Empty means a temporary directory this cluster owns and removes.
        // A cloud row sets KYTHIRA_BENCH_DATA_DIR to the provisioned volume
        // whose class and IOPS the provenance records, which is the whole
        // point of running this on an instance rather than here.
        if (const char* dir = std::getenv("KYTHIRA_BENCH_DATA_DIR"); dir != nullptr) {
            cluster._data_dir = dir;
        }
        rows.push_back(measure<kythira::testing::beast_http_transport<json>>(
            {._operations = 400,
             ._cluster = cluster,
             // No floor. The barrier arm is bounded by a real device and this
             // suite has never measured it, so a floor here would be a guess
             // asserted as a requirement — which is the failure doctrine 98 is
             // about, from the other direction.
             ._floor_ops_per_second = 0.0}));
    }
    report_durability_comparison(rows);
    // The claim the middle arm exists to make, asserted rather than left to a
    // reader comparing two columns. If a future change makes the buffered arm
    // fsync after all, this is the check that notices — and its label would be
    // wrong, which is worse than its number being wrong.
    BOOST_CHECK_MESSAGE(
        rows[1].median_run()._durability_counts._barriers == 0,
        "file/buffered issued a durability barrier — the arm that exists to price the write "
        "without the fsync is paying for one, and its NOT DURABLE label is now wrong");
    BOOST_CHECK_MESSAGE(rows[2].median_run()._durability_counts._barriers > 0,
                        "file/barrier issued NO durability barrier — the barrier is not reaching "
                        "the engines, and the row is not durable");

    // **Asserted, not merely recorded**, which is the change
    // `.kiro/specs/durable-append-barrier/` earned. Requirement 2.2 of that
    // spec: a coverage fraction other than 1.0 in a configuration claiming
    // durability is a failure, not a slow row. This used to be a printed note
    // precisely because the fraction was 19.9–24.5% and a bound would have
    // been a threshold nobody measured.
    //
    // The bound is 0.99 rather than 1.0 for one reason, and it is about the
    // instrument rather than the system: both counters are differenced around
    // the measured window while the hosts keep ticking, so an append that
    // lands inside the window and whose barrier settles just after the closing
    // snapshot is counted in the denominator and not yet in the numerator. At
    // ~1200 entries that is worth at most a few tenths of a percent. Anything
    // materially below 1.0 is the defect, not the sampling.
    {
        const auto covered = rows[2].median_run()._durability_counts.barriered_fraction();
        std::ostringstream note;
        note << "  file/barrier: a barrier covered " << 100.0 * covered << "% of appended entries ("
             << rows[2].median_run()._durability_counts._entries_batched << " of "
             << rows[2].median_run()._durability_counts._entries << ").";
        BOOST_TEST_MESSAGE(note.str());
        BOOST_CHECK_MESSAGE(covered >= 0.99,
                            "file/barrier covered only "
                                << 100.0 * covered
                                << "% of appended entries. The rest reached the page cache and "
                                   "stopped, so this configuration is PARTIALLY barriered and is "
                                   "NOT durable (Requirement 2.2 of "
                                   ".kiro/specs/durable-append-barrier/)");
    }
#else
    BOOST_TEST_MESSAGE("  durability sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
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
            std::ignore = measure<kythira::testing::beast_http_transport<json>>(
                {._operations = concurrency_budget(in_flight),
                 ._in_flight = in_flight,
                 ._distribution = distribution,
                 ._floor_ops_per_second = floor});
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
    std::ignore = measure<kythira::testing::beast_http_transport<json>>(
        {._distribution = key_distribution::uniform, ._floor_ops_per_second = 5.0});
    std::ignore = measure<kythira::testing::beast_http_transport<json>>(
        {._distribution = key_distribution::zipfian, ._floor_ops_per_second = 2.0});
#else
    BOOST_TEST_MESSAGE("  distribution sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the read taxonomy ────────────────────────────────────────────────────────

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
        std::ignore = measure<kythira::testing::beast_http_transport<json>>(
            read_row_spec{._kind = kind, ._operations = read_budget(kind)});
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
        std::ignore = measure<kythira::testing::beast_http_transport<json>>(
            read_row_spec{._kind = read_kind::read_state,
                          ._distinct_keys = keys,
                          ._stride = 1,
                          ._operations = operations,
                          ._in_flight = 8});
    }
#else
    BOOST_TEST_MESSAGE(
        "  read_state shard-size curve: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the tick cadence axis ────────────────────────────────────────────────────

/// @brief The tick sweep read the way Requirement 8.6 asks for it: each
///        percentile against the cadence, and each one's movement relative to
///        the fastest tick measured.
///
/// The per-row report already prints every one of these numbers. What it cannot
/// do is answer 8.6's actual question — *which* percentiles move with the
/// cadence and which do not — because that is a statement about the rows
/// together. A percentile that scales with the tick is on a path that waits for
/// one; a percentile that does not is on a path that does not, and H6 is the
/// claim that the first set is non-empty.
///
/// The ratio column is against the fastest cadence in the sweep rather than
/// against the tick period, so a reader can compare it with the tick's own
/// ratio directly: a path gated purely by the clock would move 20x between a
/// 1 ms and a 20 ms tick.
///
/// `p99` is `std::optional` and is printed as `n/a` when the row did not have
/// the samples for one, rather than as a number that would be the slowest of a
/// few hundred.
auto report_tick_sweep(const std::vector<repeated_result>& rows) -> void {
    if (rows.empty()) {
        return;
    }
    const auto& base = rows.front().median_run();
    const auto ratio = [](double value, double reference) -> std::string {
        std::ostringstream r;
        r << std::fixed << std::setprecision(2);
        if (reference <= 0.0) {
            r << "n/a";
        } else {
            r << (value / reference) << "x";
        }
        return r.str();
    };

    // One leader replicates to `nodes - 1` followers for each of `groups`
    // shards, so this many AppendEntries streams are live at once. Dividing by
    // it turns a cluster-wide count into the quantity the mechanism is about:
    // how often ONE stream issues a round.
    const auto streams = [](const benchmark_result& r) -> std::size_t {
        return r._groups * (r._nodes > 0 ? r._nodes - 1 : 0);
    };
    const auto inter_round_ms = [&streams](const benchmark_result& r) -> double {
        const auto seconds = std::chrono::duration<double>(r._duration).count();
        const auto s = static_cast<double>(streams(r));
        if (seconds <= 0.0 || s <= 0.0 || r._rpc._append_entries == 0) {
            return 0.0;
        }
        return 1000.0 * seconds * s / static_cast<double>(r._rpc._append_entries);
    };

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "H6, the tick sweep read across rows (Requirement 8.6) — every figure is the "
           "median run's, and the ratio is against the "
        << base._tick_interval.count() << " ms row:"
        << "\n      tick    ops/sec        p50            p95            p99"
           "         AE/commit  round interval";
    for (const auto& row : rows) {
        const auto& median = row.median_run();
        out << "\n      " << std::setw(4) << median._tick_interval.count() << "ms " << std::setw(8)
            << median._ops_per_second << " (" << ratio(median._ops_per_second, base._ops_per_second)
            << ")  " << std::setw(8) << us(median._p50) << "us ("
            << ratio(us(median._p50), us(base._p50)) << ")  " << std::setw(8) << us(median._p95)
            << "us (" << ratio(us(median._p95), us(base._p95)) << ")  ";
        if (median._p99.has_value() && base._p99.has_value()) {
            out << std::setw(8) << us(*median._p99) << "us ("
                << ratio(us(*median._p99), us(*base._p99)) << ")";
        } else {
            out << "     n/a       ";
        }
        out << std::setprecision(2) << "  " << std::setw(6);
        if (const auto per_commit = median.rpcs_per_committed_entry()) {
            out << *per_commit;
        } else {
            out << "n/a";
        }
        out << "     " << std::setw(6) << inter_round_ms(median) << " ms" << std::setprecision(1);
        if (!row.comparable()) {
            out << "  [" << to_string(row.verdict()) << "]";
        }
    }
    out << "\n      A percentile whose ratio tracks the tick ratio is gated by the clock; one "
           "that stays near 1.00x is not."
        << "\n      `round interval` is the wall-clock gap between AppendEntries on ONE of the "
        << streams(base)
        << " replication streams (groups x followers). If the round were driven by the tick it "
           "would equal the tick; if it were driven by the proposal it would fall as "
           "throughput "
           "rises. Read it beside `AE/commit`, which is Hypothesis H2's ratio: task 8 moved "
           "concurrency 64-fold and could not move it, so whatever moves it here is not "
           "offered "
           "load.";
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief Throughput and every reported percentile against the host driver's
///        tick period — Hypothesis H6.
///
/// `multi_raft` has no timer thread. The tick is the only clock this library
/// has, so anything that waits for a heartbeat waits for the caller's next
/// `tick()`, and H6 says that sets a floor on those paths regardless of how
/// fast consensus is. Requirement 8.6 asks the measurement to show **which
/// percentiles move with the cadence and which do not**, which is why this row
/// prints the whole distribution rather than a headline.
///
/// Task 8's finding makes a sharper prediction than H6 as written. The
/// replication round turned out to be tick-driven rather than proposal-driven:
/// the AppendEntries rate was flat at 4,659–5,499/sec while offered load rose
/// sixteen-fold, and entries accumulated between rounds. If that is right then
/// the batching factor here is arrival rate × inter-round interval, so
/// **entries per AppendEntries should rise roughly in proportion to the tick
/// period while the AppendEntries rate falls in inverse proportion** — and
/// `rpc_counters` already reports both, per repetition. A tick sweep that moved
/// the latency percentiles but left those two alone would refute the mechanism,
/// not just the hypothesis.
///
/// Everything else is held at the shape the transport, serializer and
/// value-size rows share — Beast, JSON, 128 B, 16 in flight — so a cadence row
/// can be quoted against those tables. The election window is *not* rescaled
/// with the tick: it is 2000–4000 ms at every cadence, which at 20 ms is still
/// a hundred ticks, so a slower clock costs elections nothing here and the
/// sweep is not secretly a sweep of election behaviour.
BOOST_AUTO_TEST_CASE(write_throughput_by_tick_cadence,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "write throughput and latency by host tick cadence, 128B values, 16 in flight, "
        "JSON on the wire (H6: the tick sets a floor):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    std::vector<repeated_result> rows;
    for (auto tick : {std::chrono::milliseconds{1}, std::chrono::milliseconds{2},
                      std::chrono::milliseconds{5}, std::chrono::milliseconds{20}}) {
        auto options = standard_cluster_options();
        options._tick_interval = tick;
        BOOST_TEST_MESSAGE("  tick = " << tick.count() << " ms:");
        // No floor above 1.0. The whole point of the sweep is that a slower
        // clock is expected to cost throughput, and a floor set from the fast
        // arm would fail the slow one for being what it is.
        rows.push_back(measure<kythira::testing::beast_http_transport<json>>(
            {._cluster = options, ._floor_ops_per_second = 1.0}));
    }
    report_tick_sweep(rows);
#else
    BOOST_TEST_MESSAGE("  tick cadence sweep: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── cost attribution ─────────────────────────────────────────────────────────

/// @brief One component of the decomposition, with the band its inputs' spread
///        puts around it.
///
/// A component is a difference of two measured p50s, and a difference is only a
/// measurement when it is larger than the spread of the things differenced.
/// `_band` is that spread propagated through the subtraction; `resolved()` is
/// the question a reader would otherwise have to ask by hand.
struct cost_component {
    std::string _name;
    double _microseconds{0.0};
    double _band{0.0};
    std::string _note;

    [[nodiscard]] auto resolved() const -> bool { return std::abs(_microseconds) > _band; }
};

auto quoted_p50_us(const repeated_result& row) -> double {
    return us(row.median_p50());
}

/// @brief The half-width, in microseconds, that a row's repetitions put around
///        the p50 it is quoted at.
auto p50_band_us(const repeated_result& row) -> double {
    return quoted_p50_us(row) * row.p50_spread();
}

/// @brief Requirement 8.1's decomposition of one committed operation, built
///        entirely from tier and addressing deltas, with a stated residual.
///
/// ### What is subtracted from what
///
/// Four cells, identical in every respect but two: the tier (A, the in-process
/// fabric; B, Beast over loopback) and the addressing (`submit_command(key,…)`
/// against `submit_command(group, epoch,…)`).
///
/// - **routing** = B(key) − B(group). The two arms differ only in which
///   overload runs — the harness finds the leader the same way in both — so the
///   difference is the shard-map lookup by key against the lookup by group id
///   plus the epoch check. Requirement 8.3.
/// - **transport + wire serialization** = B(key) − A(key). Tier A has no socket
///   and no wire encoding; Tier B has both. Requirement 8.2 asks for exactly
///   this — the component derived from a tier delta rather than from a counter
///   inserted into a production path. The two cannot be split further here:
///   there is no tier with an encoder and no socket, and inventing one would
///   mean instrumenting the code, which is what 8.2 forbids. What the encoding's
///   own share looks like is measured on the serializer axis instead
///   (`write_throughput_by_rpc_serializer`, Requirement 8.4).
/// - **durability barrier** = 0, exactly, and stated rather than omitted: every
///   tier here uses `memory_persistence_engine`, so no operation waits on a
///   disk. A component that is zero because of the configuration is a fact
///   about the number; a component silently missing is a hole.
/// - **consensus core, host and client** = A(group). Everything left inside
///   Tier A once routing is addressed away: log append and encode, the
///   replication round over the fabric, follower apply, commit-waiter
///   fulfilment, future settlement, and this harness's own per-operation cost.
///   Requirement 8.1 lists those separately and they are **not** separated
///   here, because separating them needs timestamps inside `raft.hpp` and
///   `multi_raft_impl.hpp` — production instrumentation, which 8.2 rules out.
///   Named as one term rather than presented as several.
///
/// ### The residual is not a rounding error
///
/// residual = total − routing − (transport + serialization) − barrier − core,
/// which reduces to **(routing at Tier A) − (routing at Tier B)**. It is the
/// interaction term: whether the routing lookup costs the same over a socket as
/// it does over the fabric. A decomposition into independently-measured
/// components has one whether or not it is printed, and Requirement 8.1 asks
/// for it to be stated.
///
/// ### Cited rather than re-derived (Requirement 8.8)
///
/// The transport component's own prior measurement is
/// `doc/http_transport_performance_comparison.md`: on this same host, Beast at
/// 3,527 ops/sec and 219.4 µs p50 against cpp-httplib's 12 and 83,156.0 µs, on
/// a bare RPC ping-pong of 200 warm-up plus 2,000 measured iterations, July 28,
/// 2026. **Its conditions differ from this suite's** in the way that matters:
/// it measures one client against one server with no consensus behind it, so it
/// prices a round trip, not a replicated commit. The serializer component's is
/// `doc/protobuf_serializer_performance_comparison.md`: JSON at 2.275 µs to
/// serialize a one-entry, 16-byte-command AppendEntries against protobuf's
/// 0.526 µs, and 1.949 against 0.612 to parse it — encode/decode in isolation,
/// no transport at all. Future-settlement overhead is
/// `doc/future_backend_performance_comparison.md`, measured on bare promise and
/// continuation chains rather than on this workload. None of the three is
/// subtracted from anything below; they are the independent evidence that the
/// components which came out of the tier deltas are the right order of
/// magnitude.
auto report_decomposition(const repeated_result& tier_b_key, const repeated_result& tier_b_group,
                          const repeated_result& tier_a_key, const repeated_result& tier_a_group)
    -> void {
    const auto b_key = quoted_p50_us(tier_b_key);
    const auto b_group = quoted_p50_us(tier_b_group);
    const auto a_key = quoted_p50_us(tier_a_key);
    const auto a_group = quoted_p50_us(tier_a_group);

    std::vector<cost_component> components;
    components.push_back(
        cost_component{._name = "routing (shard-map lookup, epoch validation)",
                       ._microseconds = b_key - b_group,
                       ._band = p50_band_us(tier_b_key) + p50_band_us(tier_b_group),
                       ._note = "Tier B by key minus Tier B by group+epoch"});
    components.push_back(cost_component{._name = "transport + wire serialization",
                                        ._microseconds = b_key - a_key,
                                        ._band = p50_band_us(tier_b_key) + p50_band_us(tier_a_key),
                                        ._note = "Tier B minus Tier A, both addressed by key"});
    components.push_back(cost_component{
        ._name = "durability barrier",
        ._microseconds = 0.0,
        ._band = 0.0,
        ._note = "exactly zero: memory_persistence_engine at every tier here, nothing fsyncs"});
    components.push_back(
        cost_component{._name = "consensus core, host and client (not decomposed further)",
                       ._microseconds = a_group,
                       ._band = p50_band_us(tier_a_group),
                       ._note = "log append and encode, replication round, follower apply, "
                                "commit-waiter fulfilment, future settlement, harness"});

    double attributed = 0.0;
    for (const auto& c : components) {
        attributed += c._microseconds;
    }
    const auto residual = b_key - attributed;
    const auto residual_band = p50_band_us(tier_a_key) + p50_band_us(tier_a_group) +
                               p50_band_us(tier_b_key) + p50_band_us(tier_b_group);

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "cost decomposition of one committed operation (Requirement 8.1), "
           "128B value, 16 in flight, uniform keys"
        << "\n      total (Tier B, beast/JSON, addressed by key): " << b_key << " us p50";
    for (const auto& c : components) {
        const auto share = b_key > 0.0 ? 100.0 * c._microseconds / b_key : 0.0;
        out << "\n        " << std::setw(58) << std::left << c._name << std::right << std::setw(9)
            << c._microseconds << " us  " << std::setw(6) << share << "%"
            << "  +/- " << c._band << " us";
        out << "\n          (" << c._note << ")";
        if (!c.resolved() && c._band > 0.0) {
            // A component the machine cannot resolve is still a result: what it
            // is not is a *value*. Reported as the bound it actually
            // establishes rather than as the point estimate, which on these
            // spreads would be a number the next run contradicts.
            out << "\n          NOT RESOLVED — below its inputs' combined spread. This row "
                   "establishes an upper bound of "
                << c._band << " us (" << (b_key > 0.0 ? 100.0 * c._band / b_key : 0.0)
                << "% of the operation), not a value.";
        }
    }
    out << "\n        " << std::setw(58) << std::left
        << "residual (routing at Tier A minus routing at Tier B)" << std::right << std::setw(9)
        << residual << " us  " << std::setw(6) << (b_key > 0.0 ? 100.0 * residual / b_key : 0.0)
        << "%"
        << "  +/- " << residual_band << " us";
    if (std::abs(residual) <= residual_band) {
        out << "\n          (within the combined spread of the four rows — the components "
               "account "
               "for the total to the precision this machine supports)";
    } else {
        out << "\n          (larger than the combined spread of the four rows — routing does "
               "NOT "
               "cost the same at the two tiers, and that difference is a finding, not slack)";
    }
    out << "\n      inputs, each the p50 of its own median run:"
        << "\n        Tier B by key   " << b_key << " us (+/-" << 100.0 * tier_b_key.p50_spread()
        << "%)   Tier B by group " << b_group << " us (+/-" << 100.0 * tier_b_group.p50_spread()
        << "%)"
        << "\n        Tier A by key   " << a_key << " us (+/-" << 100.0 * tier_a_key.p50_spread()
        << "%)   Tier A by group " << a_group << " us (+/-" << 100.0 * tier_a_group.p50_spread()
        << "%)"
        << "\n      Tier A is NEVER comparable to an external number (Requirement 3.1); it is "
           "here only as the subtrahend.";
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief Requirement 8.3's comparison, taken where it can say the most: one
///        operation in flight.
///
/// The decomposition below runs at sixteen in flight because that is the shape
/// every other table in this suite shares. It is the wrong place to price
/// routing. At sixteen in flight a p50 is the latency of an operation that
/// queued behind fifteen others, so it inherits the whole run's throughput
/// noise — the four rows' p50 spreads were 27%, 20%, 10% and 4%, and a
/// sub-millisecond component differenced out of two 16 ms numbers with a 27%
/// spread cannot survive. At one in flight a p50 is one operation, start to
/// finish, and the spread is the spread of *an operation* rather than of a
/// queue.
///
/// The two arms differ in nothing but the overload called: same cluster shape,
/// same value size, same distribution, same leader discovery from a cached
/// group id. What the difference prices is `_shard_map.lookup(key)` against
/// `_shard_map.find(group)` plus the caller's epoch check — everything else in
/// `route_and_run` (the local-replica lookup, the partitioner's cross-shard
/// admission check, the merge-freeze check, `note_activity`, the leadership
/// check) runs identically in both.
auto report_routing_bound(const repeated_result& by_key, const repeated_result& by_group) -> void {
    const auto key_us = quoted_p50_us(by_key);
    const auto group_us = quoted_p50_us(by_group);
    const auto delta = key_us - group_us;
    const auto band = p50_band_us(by_key) + p50_band_us(by_group);

    std::ostringstream out;
    out << std::fixed << std::setprecision(1);
    out << "routing cost (Requirement 8.3), one operation in flight — the concurrency at "
           "which a p50 is one operation:"
        // The "+/-" is the p50's own spread and the verdict is the throughput
        // row's, and they are labelled apart because they disagree: a row can
        // be UNSTABLE on ops/sec while its p50 repeats, and the arithmetic
        // below uses the p50.
        << "\n      submit_command(key, ...)          " << key_us << " us p50 (p50 +/-"
        << (100.0 * by_key.p50_spread()) << "%; throughput verdict " << to_string(by_key.verdict())
        << ")"
        << "\n      submit_command(group, epoch, ...) " << group_us << " us p50 (p50 +/-"
        << (100.0 * by_group.p50_spread()) << "%; throughput verdict "
        << to_string(by_group.verdict()) << ")"
        << "\n      difference                        " << delta << " us  +/- " << band << " us";
    if (std::abs(delta) > band) {
        out << "\n      RESOLVED: routing by key costs " << delta << " us more than routing by "
            << "group and epoch, " << (key_us > 0.0 ? 100.0 * delta / key_us : 0.0)
            << "% of one operation.";
    } else {
        out << "\n      NOT RESOLVED, and the bound is the result: the routing lookup is under "
            << band << " us, at most " << (key_us > 0.0 ? 100.0 * band / key_us : 0.0)
            << "% of one committed operation. Requirement 8.1 asks where an operation's cost "
               "goes; this says where it does not.";
    }
    BOOST_TEST_MESSAGE(out.str());
}

/// @brief The four cells the decomposition is built from, and the decomposition.
///
/// Run back to back in one process on one machine, which is what makes the
/// subtraction legitimate at all (Requirement 6.6): a component computed by
/// differencing two numbers taken in different sessions would be measuring the
/// sessions.
///
/// The Tier A arms are the reason this case exists. Every other row in this
/// suite is Tier B, where transport and serialization are inseparable from
/// everything else; Tier A removes both and nothing else, so the difference has
/// an address. Requirement 8.2 asks for the component to come from that delta
/// rather than from instrumentation, and no counter is added to production code
/// anywhere in this task.
BOOST_AUTO_TEST_CASE(cost_attribution_by_tier,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(5400))) {
    BOOST_TEST_MESSAGE(
        "cost attribution: Tier A beside Tier B, addressed by key and by group+epoch "
        "(Requirements 8.1, 8.2, 8.3):");

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    // 600 operations, 16 in flight, 128 B, uniform — bit for bit the shape the
    // transport, serializer and value-size rows use, so the total this
    // decomposition is taken against is a number those tables already carry.
    constexpr std::size_t k_operations = 600;
    constexpr std::size_t k_in_flight = 16;
    constexpr std::size_t k_value_bytes = 128;

    BOOST_TEST_MESSAGE("  Tier B (beast/JSON over loopback), addressed by key:");
    auto b_key = measure<kythira::testing::beast_http_transport<json>>(
        {._operations = k_operations,
         ._in_flight = k_in_flight,
         ._value_bytes = k_value_bytes,
         ._routing = routing_mode::attributed_key});
    BOOST_TEST_MESSAGE("  Tier B (beast/JSON over loopback), addressed by group and epoch:");
    auto b_group = measure<kythira::testing::beast_http_transport<json>>(
        {._operations = k_operations,
         ._in_flight = k_in_flight,
         ._value_bytes = k_value_bytes,
         ._routing = routing_mode::attributed_group});
    BOOST_TEST_MESSAGE("  Tier A (in-process fabric), addressed by key:");
    auto a_key = measure<fabric_transport>({._operations = k_operations,
                                            ._in_flight = k_in_flight,
                                            ._value_bytes = k_value_bytes,
                                            ._routing = routing_mode::attributed_key});
    BOOST_TEST_MESSAGE("  Tier A (in-process fabric), addressed by group and epoch:");
    auto a_group = measure<fabric_transport>({._operations = k_operations,
                                              ._in_flight = k_in_flight,
                                              ._value_bytes = k_value_bytes,
                                              ._routing = routing_mode::attributed_group});
    // An epoch mismatch here would mean the descriptor cache went stale under a
    // window, which cannot happen with automatic split and merge off — and if
    // it ever did, every operation in the treatment arms would have failed and
    // the "routing" component would be the cost of an exception. Checked rather
    // than assumed, because the decomposition is worthless if it is not.
    for (const auto* row : {&b_group, &a_group}) {
        BOOST_CHECK_MESSAGE(row->median_run()._tally._epoch_mismatch == 0,
                            "group-addressed arm saw "
                                << row->median_run()._tally._epoch_mismatch
                                << " epoch mismatches; the cached descriptor went stale and this "
                                   "arm measured the failure path");
    }

    report_decomposition(b_key, b_group, a_key, a_group);

    // Requirement 8.3's own comparison, at one in flight rather than sixteen.
    // Two more rows rather than reusing the pair above, because the pair above
    // is at the concurrency the decomposition needs and this is at the
    // concurrency the routing question needs — and one measurement cannot be
    // both.
    BOOST_TEST_MESSAGE("  Tier B (beast/JSON over loopback), 1 in flight, addressed by key:");
    auto routing_key = measure<kythira::testing::beast_http_transport<json>>(
        {._operations = k_operations,
         ._in_flight = 1,
         ._value_bytes = k_value_bytes,
         ._routing = routing_mode::attributed_key});
    BOOST_TEST_MESSAGE(
        "  Tier B (beast/JSON over loopback), 1 in flight, addressed by group and epoch:");
    auto routing_group = measure<kythira::testing::beast_http_transport<json>>(
        {._operations = k_operations,
         ._in_flight = 1,
         ._value_bytes = k_value_bytes,
         ._routing = routing_mode::attributed_group});
    BOOST_CHECK_MESSAGE(routing_group.median_run()._tally._epoch_mismatch == 0,
                        "the one-in-flight group-addressed arm saw "
                            << routing_group.median_run()._tally._epoch_mismatch
                            << " epoch mismatches; its cached descriptor went stale");
    report_routing_bound(routing_key, routing_group);
#else
    BOOST_TEST_MESSAGE("  cost attribution: NOT RUN (KYTHIRA_BENCH_HAS_BEAST undefined)");
#endif
}

// ── the CI regression tier ───────────────────────────────────────────────────

/// @brief The bounds the CI-registered row asserts, each with the reasoning
///        Requirement 12.5 asks to be recorded beside the constant.
///
/// Every one of these is either a **ratio** or a floor chosen to be
/// hardware-independent, which is Requirement 12.1's rule and
/// `multi_raft_scale_test`'s before it: a wall-clock threshold is a statement
/// about the machine, and a ratio is a statement about the implementation. A
/// CI runner's speed varies by more than any regression this suite could
/// detect, so a throughput bound tight enough to catch one would fail on load
/// instead.
namespace regression_bounds {

/// Every operation offered must complete. Tier A has no socket to lose, no
/// disk to block on, and automatic split/merge is off, so there is no
/// legitimate source of a failed operation — `not_leader` after the election
/// budget, a timeout, or an epoch mismatch each mean something structural
/// broke. A rate rather than a count, so the budget can change without
/// touching the bound.
inline constexpr double k_min_completion_rate = 1.0;

/// Entries per entry-bearing AppendEntries cannot be below one by
/// construction: the denominator counts only calls that carried something. A
/// value under one therefore means the counters disagree with each other, not
/// that batching got worse — which is why this is an *exactness* check on the
/// instrument rather than a performance bound.
inline constexpr double k_min_entries_per_append_entries = 1.0;

/// RPCs per committed entry, bracketing every configuration this suite has
/// ever measured with room on both sides. Observed: 2.2 (20 ms tick, 16 in
/// flight) to 11.0 (300 ops/sec open loop) across tasks 8, 11 and 12, and
/// 2.65-3.20 on this exact Tier A row across three future backends. The floor
/// is below one AppendEntries per follower, which a three-node group cannot
/// commit without; the ceiling is five times the highest ever seen. What it
/// catches is a replication round that stopped coalescing entirely, or one
/// that started retrying without bound — both structural, neither
/// machine-dependent.
inline constexpr double k_min_rpcs_per_commit = 1.5;
inline constexpr double k_max_rpcs_per_commit = 60.0;

/// A "did anything happen at all" floor, and deliberately not a performance
/// bound. Tier A over the in-process fabric measured 1568-2168 ops/sec on this
/// row across three future backends on a machine that was not quiet; 20 is two
/// orders of magnitude below the slowest of those. It catches a cluster that
/// elected and then committed almost nothing — a deadlock, a lost executor, a
/// tick that stopped — and nothing else. Requirement 12.1 permits it as a
/// sanity floor precisely because no plausible runner fails it.
inline constexpr double k_min_ops_per_second = 20.0;

/// The row's own size. Small on purpose: Requirement 12.4 asks the CI variant
/// to complete within a stated budget, and this one is **under 30 seconds** in
/// Release on four cores (measured 15.2 s for a comparable Tier A row),
/// dominated by five elections rather than by the workload. The operation count
/// only has to be large enough that the replication ratios have samples behind
/// them.
inline constexpr std::size_t k_operations = 200;
inline constexpr std::size_t k_in_flight = 8;

}  // namespace regression_bounds

/// @brief Assert a lower bound, in the words Requirement 12.6 asks for: the
///        metric, the floor, the measured value, and the tier.
///
/// A free function rather than a bare `BOOST_CHECK` at each site because 12.6
/// is a property of *every* failure message here, and four hand-written
/// messages are four chances for one of them to omit the tier — which is the
/// field that tells a reader whether the bound was even chosen for the
/// configuration that failed it.
auto check_at_least(std::string_view metric, double measured, double floor, deployment_tier tier)
    -> void {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << "REGRESSION: " << metric << " measured "
        << measured << ", floor " << floor << ", at " << to_string(tier);
    BOOST_CHECK_MESSAGE(measured >= floor, out.str());
}

/// @brief The same, for a metric bounded on both sides.
auto check_within(std::string_view metric, double measured, double low, double high,
                  deployment_tier tier) -> void {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << "REGRESSION: " << metric << " measured "
        << measured << ", band [" << low << ", " << high << "], at " << to_string(tier);
    BOOST_CHECK_MESSAGE(measured >= low && measured <= high, out.str());
}

/// @brief The subset CI runs — Requirement 12.
///
/// ### Why this case exists when the rest of the suite is registered already
///
/// It is registered *twice*: once as part of `multi_raft_http_benchmark_test`,
/// which carries the `performance;benchmark` labels and is therefore excluded
/// from every CI invocation (`ci.yml` filters
/// `^(slow|performance|verbose|benchmark|docker)$`), and once as
/// `multi_raft_regression_tier`, which runs this case alone under labels CI does
/// not exclude. Before this, nothing in this suite ran in CI at all — the full
/// matrix is three hours and correctly excluded, and "correctly excluded" had
/// quietly become "never checked".
///
/// ### Tier A, and why
///
/// Requirement 12.4 expects it: a CI runner's socket and disk behaviour is not
/// a stable measurement substrate, and every quantity asserted below is a
/// property of the consensus implementation rather than of the wire. Tier B
/// would add a loopback socket whose latency on a shared runner varies by more
/// than any regression these bounds could detect.
///
/// ### What it asserts, and what it refuses to
///
/// Ratios and one absurdly low floor (Requirement 12.1). **No comparison to any
/// external implementation appears here or can** (12.2) — there is no external
/// number in this translation unit, and Tier A is the tier Requirement 3.1
/// labels never comparable to one.
///
/// The per-repetition ratios are checked on **every** run rather than on the
/// median, because a structural regression that appears in one repetition of
/// five is still a structural regression; it is the *throughput* that gets the
/// median treatment, for the reason `repeated_result` exists.
///
/// An election inside a measured window is **reported and not asserted**. On a
/// loaded CI runner an election is a fact about the runner, and failing on it
/// would make this the flaky test the whole design is written to avoid.
BOOST_AUTO_TEST_CASE(ci_regression_tier,
                     *boost::unit_test::timeout(kythira::testing::scaled_timeout(600))) {
    using namespace regression_bounds;
    constexpr auto tier = deployment_tier::a_fabric;

    BOOST_TEST_MESSAGE("CI regression tier (Requirement 12): Tier A, "
                       << k_operations << " operations, " << k_in_flight
                       << " in flight, ratios and one floor:");

    auto row = kythira::testing::throughput_row<fabric_transport>(
        {._operations = k_operations, ._in_flight = k_in_flight}, suite_observer());
    report(row);

    BOOST_REQUIRE_MESSAGE(row.runs() == k_required_repetitions, "REGRESSION: repetitions measured "
                                                                    << row.runs() << ", floor "
                                                                    << k_required_repetitions
                                                                    << ", at " << to_string(tier));

    // Ratios, per repetition. A regression that appears once in five is still
    // a regression.
    for (std::size_t i = 0; i < row._runs.size(); ++i) {
        const auto& run = row._runs[i];
        const std::string where = " (run " + std::to_string(i + 1) + ")";

        const auto completion = run._tally._offered == 0
                                    ? 0.0
                                    : static_cast<double>(run._tally._completed) /
                                          static_cast<double>(run._tally._offered);
        check_at_least("completion rate" + where, completion, k_min_completion_rate, tier);

        if (const auto batching = run._rpc.entries_per_append_entries()) {
            check_at_least("entries per AppendEntries" + where, *batching,
                           k_min_entries_per_append_entries, tier);
        } else {
            BOOST_CHECK_MESSAGE(false,
                                "REGRESSION: entries per AppendEntries" + where +
                                        " has no value — no AppendEntries carried an entry, at "
                                    << to_string(tier));
        }

        if (const auto cost = run.rpcs_per_committed_entry()) {
            check_within("RPCs per committed entry" + where, *cost, k_min_rpcs_per_commit,
                         k_max_rpcs_per_commit, tier);
        } else {
            BOOST_CHECK_MESSAGE(false, "REGRESSION: RPCs per committed entry" + where +
                                               " has no value — nothing committed, at "
                                           << to_string(tier));
        }
    }

    // The one wall-clock bound, on the median run, and low enough that no
    // plausible runner fails it.
    if (const auto headline = row.headline_ops_per_second()) {
        check_at_least("throughput", *headline, k_min_ops_per_second, tier);
    }
}

BOOST_AUTO_TEST_SUITE_END()
