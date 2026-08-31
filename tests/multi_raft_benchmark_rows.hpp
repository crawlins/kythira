// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file multi_raft_benchmark_rows.hpp
/// @brief One repetition, and one row of `k_required_repetitions` of them —
///        shared by the CTest-registered suite and the report binary.
///
/// ### Why this is a header and not two copies
///
/// `.kiro/specs/multi-raft-performance/` asks for two consumers of the same
/// measurement. Requirement 12 wants a CI-registered regression subset, which
/// is `tests/multi_raft_http_benchmark_test.cpp`; Requirement 11.6 wants a
/// report generator that can run a subset by tier, scenario or axis, because
/// the full matrix will not fit in one sitting. Those are different programs
/// with different budgets and different failure behaviour, and they must not be
/// different *measurements*. A row taken by the report binary has to be the
/// same row the regression suite would have taken, or the artifact describes
/// something the CI gate never checked.
///
/// So the cluster shape, the budgets, the warm-up rule, the two structural
/// checks and the repetition loop live here, once. What differs between the two
/// consumers is only where a message goes and what a failed precondition does,
/// and that is `row_observer`.
///
/// ### `row_observer`
///
/// Two callbacks rather than a dependency on Boost.Test. `_message` is where
/// progress and per-repetition detail go — `BOOST_TEST_MESSAGE` in the suite,
/// `std::cout` in the report binary. `_require` is a precondition that must
/// hold for the measurement to mean anything (a cluster that never elected, a
/// preload that half-failed); the suite turns it into `BOOST_REQUIRE`, the
/// report binary into a thrown exception that abandons that row and keeps the
/// rest of the matrix. `_check` is a condition worth reporting that does not
/// invalidate the row — a term that moved, a node-internal serializer that
/// drifted.
///
/// Defaulted to a silent observer whose `_require` throws, so a caller that
/// forgets to install one gets no output and a hard stop, never a silently
/// skipped precondition.

#include "multi_raft_transport_harness.hpp"
#include "test_timeout_scale.hpp"

#include <chrono>
#include <functional>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace kythira::testing {

/// @brief The cluster shape every row shares.
///
/// Comparability requires that the only thing differing between rows is the
/// axis under test, so these are constants rather than per-transport tuning.
///
/// The election timings are deliberately far more generous than the fabric
/// suites use. Four stripes for four groups keeps each stripe to one group's
/// worth of blocking I/O per tick.
///
/// **The reason these numbers are what they are has changed, and the numbers
/// have not.** They were chosen because a cpp-httplib tick's send phase blocked
/// for one ~83 ms round trip per follower per stripe, so a 300 ms election
/// timeout — fine over an in-process fabric — would have had followers timing
/// out on a leader that was merely mid-heartbeat. That round trip was Nagle and
/// is gone: `tcp_nodelay` defaults to `true` and the same row measures a 9.8 ms
/// p50.
///
/// So the old justification was retired and the values were **measured** rather
/// than inherited. Lowering them to 600–1200 ms with a 150 ms heartbeat, and
/// re-running the transport axis:
///
/// | | 2000–4000 / 400 ms | 600–1200 / 150 ms |
/// |---|---:|---:|
/// | elections | 0 of 15 windows | 0 of 15 windows |
/// | H1 entries/AppendEntries (beast) | 4.64 | 4.64 |
/// | **H2 RPCs/committed entry (beast)** | **3.99** | **4.38** |
/// | H2 RPCs/committed entry (proxygen) | 3.99 | 4.19 |
///
/// The tighter timing is *safe* — nothing elected in fifteen windows — and it
/// is **not free**. A 2.7x faster heartbeat adds heartbeat AppendEntries the
/// commits did not need, and H2 moves about 10%. H2 is the quantity this
/// project has tracked across three machines and two cloud vendors and found
/// converging on 2.10; moving it by a configuration change would cost every
/// comparison against those rows. H1 does not move at all.
///
/// **And nothing here measures what a shorter election timeout buys.** No row
/// in this suite fails a leader, so faster failover is a property none of these
/// numbers would show. Paying a real cost in comparability for a benefit no row
/// measures is the wrong trade, so the values stay — now for a reason that is
/// true today rather than one that expired.
inline auto standard_cluster_options() -> kv_cluster_options {
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
/// constraint on a sanitizer build — 30 seconds is ample in Release and not
/// enough under ASan to elect four shards over a real socket at all, so the
/// case failed on its own deadline while measuring nothing.
inline const auto k_election_budget = scaled_deadline(30000);

/// @brief The per-operation deadline every row hands to `run_command`. Scaled
/// for the same reason, and by the same factor, as the election budget: an
/// operation that times out is counted as a failure rather than as a slow
/// build.
inline const auto k_operation_timeout = scaled_deadline(20000);

/// @brief Nanoseconds as microseconds, which is the unit every row prints in.
inline auto us(std::chrono::nanoseconds d) -> double {
    return std::chrono::duration<double, std::micro>(d).count();
}

/// @brief Where a row's output goes and what a failed precondition does.
///
/// See the file comment. The defaults are silent, and `_require` throws — a
/// caller that installs nothing gets a hard stop rather than a measurement
/// taken on a cluster that never elected.
struct row_observer {
    /// Progress and per-repetition detail.
    std::function<void(const std::string&)> _message = [](const std::string&) {};
    /// A precondition the measurement cannot proceed without.
    std::function<void(bool, const std::string&)> _require = [](bool ok, const std::string& what) {
        if (!ok) {
            throw std::runtime_error(what);
        }
    };
    /// Worth reporting; does not invalidate the row.
    std::function<void(bool, const std::string&)> _check = [](bool, const std::string&) {};
};

/// @brief How many operations a repetition throws away before it starts
///        measuring.
///
/// An eighth of the budget, but never fewer than one per client thread —
/// otherwise a highly-concurrent row warms up some workers and not others, and
/// the measured window pays the difference.
inline auto warmup_operations(std::size_t operations, std::size_t in_flight) -> std::size_t {
    return std::max<std::size_t>(in_flight, operations / 8);
}

/// @brief Everything that distinguishes one write row from another.
///
/// A struct rather than a parameter list because the list had reached seven and
/// two of them were `std::size_t`. A caller naming `._in_flight = 16` cannot
/// transpose it with `._value_bytes`, and a caller adding an axis does not
/// touch every existing call site.
struct write_row_spec {
    std::size_t _operations{600};
    std::size_t _in_flight{16};
    std::size_t _value_bytes{128};
    key_distribution _distribution{key_distribution::uniform};
    routing_mode _routing{routing_mode::by_key};
    load_mode _load{load_mode::closed_loop};
    double _offered_rate_per_second{0.0};
    kv_cluster_options _cluster{standard_cluster_options()};
    /// A sanity floor, not a target: low enough that a loaded runner passes,
    /// high enough that a structural regression (a replication round that
    /// stopped batching, a lost connection pool) does not. Checked against the
    /// median, never against every run.
    double _floor_ops_per_second{0.0};
};

/// @brief Everything that distinguishes one read row from another.
struct read_row_spec {
    read_kind _kind{read_kind::read_state};
    std::uint64_t _distinct_keys{1000};
    std::uint64_t _stride{100};
    std::size_t _value_bytes{128};
    std::size_t _operations{400};
    std::size_t _in_flight{8};
    kv_cluster_options _cluster{standard_cluster_options()};
};

/// @brief One repetition: a fresh cluster, a discarded warm-up, one measured
///        window.
///
/// `_operations` is per-row on purpose. Throughput is a rate, so rows with
/// different budgets remain comparable on ops/sec; what a smaller budget costs
/// is the tail — which is why `latency_sample_set` refuses to print a p99 it
/// does not have the samples for rather than printing a flattering one.
template<typename Transport>
auto one_measurement(const write_row_spec& spec, const row_observer& observer = {})
    -> benchmark_result {
    kv_cluster<Transport> cluster{spec._cluster};
    {
        std::ostringstream why;
        why << Transport::name() << ": no leader on every shard within budget";
        observer._require(cluster.await_all_leaders(k_election_budget), why.str());
    }

    const auto terms_before = cluster.term_sum();

    workload_options workload;
    workload._in_flight = spec._in_flight;
    workload._operations = spec._operations;
    workload._value_bytes = spec._value_bytes;
    workload._key_count = cluster.options()._key_count;
    workload._distribution = spec._distribution;
    workload._op_timeout = k_operation_timeout;
    workload._routing = spec._routing;
    workload._load = spec._load;
    workload._offered_rate_per_second = spec._offered_rate_per_second;
    // The scenario carries the distribution, because the two arms of the
    // distribution sweep are otherwise identical in every printed field and a
    // reader would have no way to tell the rows apart.
    workload._scenario =
        spec._distribution == key_distribution::zipfian ? "put/zipfian" : "put/uniform";

    // Warm-up: elections have settled but connections, allocators and the
    // serializer's own buffers have not. Discarded.
    workload_options warmup = workload;
    warmup._operations = warmup_operations(spec._operations, spec._in_flight);
    std::ignore = run_put_workload(cluster, warmup);

    auto row = run_put_workload(cluster, workload);

    const auto terms_after = cluster.term_sum();
    // A window that spanned an election measured election recovery, not steady
    // state. Reported rather than asserted: on a loaded machine an election is
    // a fact about the machine, and failing the build for it would make this
    // test flaky in exactly the way doctrine warns against. It is also one of
    // the things a wide run-to-run spread is usually explained by, which is why
    // it is reported per repetition rather than per row.
    {
        std::ostringstream note;
        note << "    " << Transport::name() << ": term sum " << terms_before << " -> "
             << terms_after
             << (terms_before == terms_after ? " (steady)" : " (AN ELECTION OCCURRED)");
        observer._message(note.str());
    }

    {
        std::ostringstream why;
        why << Transport::name() << ": no operation completed at all";
        observer._check(row._tally._completed > 0, why.str());
    }

    // The swept axis is the *wire* serializer; the node-internal one (log
    // entries, snapshots) is held at JSON so rows differing only in
    // `_serializer` differ only in what crossed the socket. `kv_host_types`
    // pins it by type alias, but a pin nothing reads is a pin that can be moved
    // without anyone noticing — so every repetition of every row checks the
    // media type the node bundle reports for itself (Requirement 8.4).
    {
        std::ostringstream why;
        why << Transport::name() << " / " << row._serializer
            << ": node-internal serializer drifted off JSON, it reports '" << row._node_serializer
            << "' — this row is not comparable with the others on the serializer axis";
        observer._check(row._node_serializer == "application/json", why.str());
    }
    return row;
}

/// @brief One repetition of a read row: a fresh cluster, a preloaded store, one
///        measured window of one read kind.
///
/// The preload is inside the repetition, not shared across the five, because a
/// repetition is a whole measurement and a store loaded once and read five
/// times would measure a progressively warmer cache against a progressively
/// less representative cluster.
///
/// `preloaded` is asserted rather than assumed. A read row over a store that is
/// nine-tenths loaded measures something other than what it claims, and the
/// only way to know is to count what committed.
template<typename Transport>
auto one_read_measurement(const read_row_spec& spec, const row_observer& observer = {})
    -> benchmark_result {
    kv_cluster<Transport> cluster{spec._cluster};
    {
        std::ostringstream why;
        why << Transport::name() << ": no leader on every shard within budget";
        observer._require(cluster.await_all_leaders(k_election_budget), why.str());
    }

    operation_tally preload_tally;
    const auto preloaded = preload_keys(cluster, spec._distinct_keys, spec._stride,
                                        spec._value_bytes, k_operation_timeout, preload_tally);
    {
        std::ostringstream why;
        why << Transport::name() << ": preloaded only " << preloaded << " of "
            << spec._distinct_keys << " keys; this row would measure a miss path";
        observer._require(preloaded == spec._distinct_keys, why.str());
    }

    workload_options workload;
    workload._in_flight = spec._in_flight;
    workload._operations = spec._operations;
    workload._value_bytes = spec._value_bytes;
    workload._key_count = spec._distinct_keys;
    workload._key_stride = spec._stride;
    workload._distribution = key_distribution::uniform;
    workload._op_timeout = k_operation_timeout;
    workload._scenario = std::string{to_string(spec._kind)};

    workload_options warmup = workload;
    warmup._operations = warmup_operations(spec._operations, spec._in_flight);
    std::ignore = run_read_workload(cluster, warmup, spec._kind);

    auto row = run_read_workload(cluster, workload, spec._kind);

    {
        std::ostringstream why;
        why << Transport::name() << " / " << to_string(spec._kind) << ": no read completed at all";
        observer._check(row._tally._completed > 0, why.str());
    }
    return row;
}

/// @brief One repetition, formatted the way every consumer of this header
///        prints it.
///
/// Shared for the same reason the measurement is: a per-repetition line that
/// says something different in the report binary than in the suite would make
/// two logs of the same row impossible to compare by eye.
inline auto format_repetition(std::size_t repetition, const benchmark_result& row) -> std::string {
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
    if (row._load == load_mode::open_loop) {
        // Requirement 4.2's correction, shown rather than assumed: a lag that
        // is a large fraction of the latency means the pool could not sustain
        // the schedule, so the row was not measured at the rate it asked for.
        out << " | lag=" << us(row._mean_schedule_lag) << "us";
    }
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
    return out.str();
}

/// @brief One row of the matrix: `k_required_repetitions` whole measurements,
///        collected into a `repeated_result`.
///
/// Each repetition builds its own cluster and elects again. That is more
/// expensive than re-running the workload against one long-lived cluster, and
/// it is the point: the ±21% Beast/JSON/128 B spread that made Requirement 6.2
/// necessary appeared *between* freshly-elected clusters, so a repetition that
/// reused one would have measured a narrower thing than the number is quoted as.
template<typename Transport>
auto throughput_row(const write_row_spec& spec, const row_observer& observer = {})
    -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(spec._operations, spec._in_flight);
    row._measured_operations = spec._operations;

    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = one_measurement<Transport>(spec, observer);
        observer._message(format_repetition(repetition, run));
        row.record(std::move(run));
    }

    if (const auto headline = row.headline_ops_per_second();
        headline.has_value() && spec._floor_ops_per_second > 0.0) {
        std::ostringstream why;
        why << Transport::name() << ": median " << *headline
            << " ops/sec is below the sanity floor of " << spec._floor_ops_per_second;
        observer._check(*headline >= spec._floor_ops_per_second, why.str());
    }
    return row;
}

/// @brief `k_required_repetitions` read measurements, collected into one row.
template<typename Transport>
auto read_row(const read_row_spec& spec, const row_observer& observer = {}) -> repeated_result {
    repeated_result row;
    row._warmup_operations = warmup_operations(spec._operations, spec._in_flight);
    row._measured_operations = spec._operations;
    for (std::size_t repetition = 0; repetition < k_required_repetitions; ++repetition) {
        auto run = one_read_measurement<Transport>(spec, observer);
        observer._message(format_repetition(repetition, run));
        row.record(std::move(run));
    }
    return row;
}

}  // namespace kythira::testing
