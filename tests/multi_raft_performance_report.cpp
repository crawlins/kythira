// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file multi_raft_performance_report.cpp
/// @brief The report generator for `.kiro/specs/multi-raft-performance/` —
///        Requirement 11.
///
/// ### What this is, and what `multi_raft_http_benchmark_test` is
///
/// They run the **same measurement** and serve different purposes.
///
/// `tests/multi_raft_http_benchmark_test.cpp` is the CI-registered regression
/// subset of Requirement 12: it asserts sanity floors, fails a build, and prints
/// for a human reading a test log. This binary is the publication path: it takes
/// a *subset* of the matrix chosen on the command line (Requirement 11.6,
/// because the full matrix will not fit in one sitting), writes a timestamped
/// CSV and JSON pair to `test_results/` (11.1), and abandons a row whose
/// preconditions failed rather than the whole run.
///
/// Neither of them owns the measurement. `tests/multi_raft_benchmark_rows.hpp`
/// does — the cluster shape, the budgets, the warm-up rule, the structural
/// checks and the repetition loop are there, once, and both programs install a
/// `row_observer` over them. If those two ever describe different work, the
/// artifact stops describing what CI checks, and that is the failure this
/// arrangement exists to make impossible.
///
/// ### Not registered as a full CTest run
///
/// A report-quality invocation elects a fresh cluster five times per row over
/// real sockets and takes minutes per axis. `tests/CMakeLists.txt` registers a
/// deliberately tiny `--axis smoke` invocation instead, which is a build and
/// runtime check of *this program* — argument parsing, the catalog, the
/// artifact writers — and not a measurement. Report runs are launched by a
/// developer.
///
/// ### Usage
///
/// ```
/// multi_raft_performance_report --list
/// multi_raft_performance_report --axis concurrency --axis tick-cadence
/// multi_raft_performance_report --tier A
/// multi_raft_performance_report --scenario 'beast/json/128B/16'
/// multi_raft_performance_report --axis open-loop --rate 400
/// ```
///
/// With no selector every runnable row in the catalog runs, which is the full
/// matrix and is measured in hours.

#include "multi_raft_benchmark_rows.hpp"
#include "multi_raft_report_artifacts.hpp"

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
#include <exception>
#include <filesystem>
#include <functional>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using kythira::testing::deployment_tier;
using kythira::testing::describe_machine;
using kythira::testing::dropped_row;
using kythira::testing::fabric_transport;
using kythira::testing::k_required_repetitions;
using kythira::testing::key_distribution;
using kythira::testing::load_mode;
using kythira::testing::machine_description;
using kythira::testing::make_timestamp;
using kythira::testing::read_kind;
using kythira::testing::read_row_spec;
using kythira::testing::repeated_result;
using kythira::testing::report_row;
using kythira::testing::row_observer;
using kythira::testing::standard_cluster_options;
using kythira::testing::to_string;
using kythira::testing::us;
using kythira::testing::write_csv;
using kythira::testing::write_json;
using kythira::testing::write_row_spec;

using json = kythira::json_serializer;
using cbor = kythira::cbor_serializer;

/// @brief One entry in the catalog: what to measure, and the two labels a
///        selector matches on.
///
/// `_run` is a thunk rather than a description the program interprets, because
/// the transport and the serializer are *template arguments* — a row is a
/// distinct instantiation, not a set of runtime fields, and pretending
/// otherwise would mean a switch that has to be updated in two places every
/// time a transport is added.
///
/// `_tier` is duplicated here rather than read back off the result, because a
/// `--tier` selector has to decide whether to run a row *before* running it.
struct catalog_entry {
    std::string _axis;
    std::string _scenario;
    deployment_tier _tier;
    std::function<repeated_result(const row_observer&)> _run;
};

/// @brief Where this program's rows send their output, and what a failed
///        precondition does here.
///
/// `_require` throws. The report binary runs a matrix, and a cluster that never
/// elected invalidates *that row* and nothing else — abandoning the whole run
/// would throw away every row already measured, which on a multi-hour matrix is
/// the worst available outcome. The catalog loop catches it, records the row as
/// not measured, and continues.
auto console_observer() -> row_observer {
    return row_observer{
        ._message = [](const std::string& text) { std::cout << text << '\n'; },
        ._require =
            [](bool ok, const std::string& why) {
                if (!ok) {
                    throw std::runtime_error(why);
                }
            },
        ._check =
            [](bool ok, const std::string& why) {
                if (!ok) {
                    std::cout << "    NOTE: " << why << '\n';
                }
            },
    };
}

auto tier_letter(deployment_tier tier) -> std::string_view {
    switch (tier) {
        case deployment_tier::a_fabric:
            return "A";
        case deployment_tier::b_loopback:
            return "B";
    }
    return "?";
}

/// @brief The catalog, and everything this build could not put in it.
struct catalog {
    std::vector<catalog_entry> _entries;
    std::vector<dropped_row> _dropped;
};

/// @brief What the command line asked for.
struct selection {
    std::vector<std::string> _axes;
    std::vector<std::string> _scenarios;
    std::vector<std::string> _tiers;
    /// The offered rate the `open-loop` axis uses, in operations per second.
    double _rate{400.0};
    /// Scales every row's operation budget. A report run wants the catalog's
    /// own budgets; the CTest smoke entry wants them small enough to finish in
    /// a minute, and does not want a *different catalog*, which would check a
    /// program nobody runs.
    double _budget_scale{1.0};
    std::filesystem::path _out_dir{"test_results"};
    bool _list{false};
};

/// @brief Case-insensitive substring match, so `--axis tick` selects
///        `tick-cadence` and `--tier a` selects Tier A.
///
/// Substring rather than exact because the scenario labels are long and a
/// selector nobody can type is a selector nobody uses. An empty selector list
/// matches everything, which is what makes "no arguments" mean "the whole
/// matrix".
auto matches(const std::vector<std::string>& wanted, std::string_view value) -> bool {
    if (wanted.empty()) {
        return true;
    }
    std::string haystack{value};
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const auto& w : wanted) {
        std::string needle = w;
        std::transform(needle.begin(), needle.end(), needle.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// @brief Scale an operation budget, never below one per client thread.
///
/// A scaled budget that rounds to zero would make `run_put_workload` measure a
/// window in which nothing was offered, which reports as a division by a
/// duration rather than as an error.
auto scaled(std::size_t operations, std::size_t in_flight, double scale) -> std::size_t {
    const auto scaled_value = static_cast<std::size_t>(static_cast<double>(operations) * scale);
    return std::max(in_flight, std::max<std::size_t>(1, scaled_value));
}

/// @brief The full matrix, as data.
///
/// Every row the suite measures, plus the two Requirement 4 asks for that a
/// CI-registered test cannot carry: an open-loop arm, whose rate has to be
/// chosen for the machine, and the 512-in-flight point of Requirement 4.3,
/// which is minutes of wall clock on this host.
auto build_catalog(const selection& sel) -> catalog {
    catalog out;
    const auto scale = sel._budget_scale;

    // Every `#else` below records what it could not build and why, so that
    // `--list` and every run print the same list the compiler decided.
    const auto drop = [&out](std::string axis, std::string scenario, std::string reason) {
        out._dropped.push_back(dropped_row{._axis = std::move(axis),
                                           ._scenario = std::move(scenario),
                                           ._reason = std::move(reason)});
    };

    const auto add_write = [&](std::string axis, std::string scenario, deployment_tier tier,
                               auto transport_tag, write_row_spec spec) {
        using transport_type = typename decltype(transport_tag)::type;
        spec._operations = scaled(spec._operations, spec._in_flight, scale);
        out._entries.push_back(catalog_entry{
            ._axis = std::move(axis),
            ._scenario = std::move(scenario),
            ._tier = tier,
            ._run = [spec](const row_observer& observer) {
                return kythira::testing::throughput_row<transport_type>(spec, observer);
            }});
    };
    const auto add_read = [&](std::string axis, std::string scenario, auto transport_tag,
                              read_row_spec spec) {
        using transport_type = typename decltype(transport_tag)::type;
        spec._operations = scaled(spec._operations, spec._in_flight, scale);
        out._entries.push_back(catalog_entry{._axis = std::move(axis),
                                             ._scenario = std::move(scenario),
                                             ._tier = deployment_tier::b_loopback,
                                             ._run = [spec](const row_observer& observer) {
                                                 return kythira::testing::read_row<transport_type>(
                                                     spec, observer);
                                             }});
    };

    // A tag type, because a lambda cannot take a template parameter and a
    // function template cannot be stored in the catalog. `std::type_identity`
    // carries the transport into the lambda's body where the call needs it.
    const auto tag = []<typename T>() { return std::type_identity<T>{}; };

    // `smoke` is not an axis of the matrix. It is the one row the CTest entry
    // runs: the cheapest possible instantiation of this whole program, so that
    // a broken catalog, selector or artifact writer fails a build rather than a
    // developer's report run two hours in.
    add_write("smoke", "fabric/none/128B/4", deployment_tier::a_fabric,
              tag.operator()<fabric_transport>(),
              write_row_spec{._operations = 40, ._in_flight = 4});

#if defined(KYTHIRA_BENCH_HAS_BEAST)
    using beast_json = kythira::testing::beast_http_transport<json>;
    using beast_cbor = kythira::testing::beast_http_transport<cbor>;

    add_write("transport", "cpp-httplib/json/128B/4", deployment_tier::b_loopback,
              tag.operator()<kythira::testing::cpp_httplib_transport<json>>(),
              write_row_spec{._operations = 24, ._in_flight = 4});
    add_write("transport", "beast/json/128B/16", deployment_tier::b_loopback,
              tag.operator()<beast_json>(), write_row_spec{});
#if defined(KYTHIRA_BENCH_HAS_PROXYGEN)
    add_write("transport", "proxygen/json/128B/16", deployment_tier::b_loopback,
              tag.operator()<kythira::testing::proxygen_http_transport<json>>(), write_row_spec{});
#else
    drop("transport", "proxygen/json/128B/16",
         "KYTHIRA_BENCH_HAS_PROXYGEN undefined (requires KYTHIRA_BUILD_PROXYGEN_TRANSPORT)");
#endif

    add_write("serializer", "beast/cbor/128B/16", deployment_tier::b_loopback,
              tag.operator()<beast_cbor>(), write_row_spec{});
#if defined(KYTHIRA_BENCH_HAS_PROTOBUF)
    add_write(
        "serializer", "beast/protobuf/128B/16", deployment_tier::b_loopback,
        tag.operator()<kythira::testing::beast_http_transport<kythira::protobuf_serializer>>(),
        write_row_spec{});
#else
    drop("serializer", "beast/protobuf/128B/16",
         "KYTHIRA_BENCH_HAS_PROTOBUF undefined (requires PROTOBUF_SERIALIZER_FOUND)");
#endif
#if defined(KYTHIRA_BENCH_HAS_ION)
    add_write("serializer", "beast/ion/128B/16", deployment_tier::b_loopback,
              tag.operator()<kythira::testing::beast_http_transport<kythira::ion_serializer>>(),
              write_row_spec{});
#else
    drop("serializer", "beast/ion/128B/16",
         "KYTHIRA_BENCH_HAS_ION undefined (requires CONFIG_ION_SERIALIZER and the ion-c vcpkg "
         "feature)");
#endif

    // Requirement 1.4's four value sizes.
    for (std::size_t bytes :
         {std::size_t{16}, std::size_t{128}, std::size_t{1024}, std::size_t{4096}}) {
        add_write("value-size", "beast/json/" + std::to_string(bytes) + "B/16",
                  deployment_tier::b_loopback, tag.operator()<beast_json>(),
                  write_row_spec{._operations = 400, ._value_bytes = bytes});
    }

    // Requirement 4.3's four in-flight points, both distributions. 512 is here
    // and not in the CTest suite: it is the point the requirement names and the
    // one whose wall clock a CI leg cannot absorb.
    for (auto distribution : {key_distribution::uniform, key_distribution::zipfian}) {
        const std::string arm = distribution == key_distribution::zipfian ? "zipfian" : "uniform";
        for (std::size_t in_flight :
             {std::size_t{1}, std::size_t{8}, std::size_t{64}, std::size_t{512}}) {
            add_write(
                "concurrency", "beast/json/128B/" + arm + "/" + std::to_string(in_flight),
                deployment_tier::b_loopback, tag.operator()<beast_json>(),
                write_row_spec{._operations = in_flight <= 8 ? std::size_t{200} : std::size_t{600},
                               ._in_flight = in_flight,
                               ._distribution = distribution});
        }
    }

    // Requirement 8.6's cadence sweep.
    for (auto tick : {std::chrono::milliseconds{1}, std::chrono::milliseconds{2},
                      std::chrono::milliseconds{5}, std::chrono::milliseconds{20}}) {
        auto cluster = standard_cluster_options();
        cluster._tick_interval = tick;
        add_write("tick-cadence", "beast/json/128B/16/tick" + std::to_string(tick.count()) + "ms",
                  deployment_tier::b_loopback, tag.operator()<beast_json>(),
                  write_row_spec{._cluster = cluster});
    }

    // Requirement 8.1–8.3's four cells.
    for (auto routing : {kythira::testing::routing_mode::attributed_key,
                         kythira::testing::routing_mode::attributed_group}) {
        const std::string how =
            routing == kythira::testing::routing_mode::attributed_group ? "by-group" : "by-key";
        add_write("cost-attribution", "beast/json/128B/16/" + how, deployment_tier::b_loopback,
                  tag.operator()<beast_json>(), write_row_spec{._routing = routing});
        add_write("cost-attribution", "fabric/none/128B/16/" + how, deployment_tier::a_fabric,
                  tag.operator()<fabric_transport>(), write_row_spec{._routing = routing});
        add_write("cost-attribution", "beast/json/128B/1/" + how, deployment_tier::b_loopback,
                  tag.operator()<beast_json>(),
                  write_row_spec{._in_flight = 1, ._routing = routing});
    }

    // Requirement 4.1 and 4.2's open loop. Not in the CTest suite because the
    // rate has to be one the machine can sustain, and a CI runner's cannot be
    // known in advance; here it is a command-line parameter and the row reports
    // its schedule lag so a rate the machine could not hold is visible.
    // The rate is formatted as an integer: `std::to_string(double)` produces
    // `400.000000`, and a scenario label is something a `--scenario` selector
    // has to be typed against.
    add_write("open-loop",
              "beast/json/128B/16/rate" + std::to_string(static_cast<long long>(sel._rate)),
              deployment_tier::b_loopback, tag.operator()<beast_json>(),
              write_row_spec{._load = load_mode::open_loop, ._offered_rate_per_second = sel._rate});

    // Requirement 2.1's three read kinds, and 2.4's shard-size curve.
    for (auto kind : {read_kind::read_state, read_kind::log_get, read_kind::local_stale}) {
        add_read("read-taxonomy", "beast/json/" + std::string{to_string(kind)},
                 tag.operator()<beast_json>(),
                 read_row_spec{._kind = kind,
                               ._operations = kind == read_kind::local_stale ? std::size_t{200000}
                                                                             : std::size_t{400}});
    }
    for (std::uint64_t keys : {std::uint64_t{100}, std::uint64_t{1000}, std::uint64_t{5000}}) {
        add_read("shard-size", "beast/json/read_state/" + std::to_string(keys) + "keys",
                 tag.operator()<beast_json>(),
                 read_row_spec{._kind = read_kind::read_state,
                               ._distinct_keys = keys,
                               ._stride = 1,
                               ._operations = keys >= 5000 ? std::size_t{80} : std::size_t{200}});
    }
#else
    // Beast is the transport every non-transport axis holds fixed, so without
    // it there is no axis left but `smoke` — and a run that printed one Tier A
    // row and nothing else would read as a complete matrix. Named, one entry
    // per axis, with the reason.
    for (const auto* axis : {"transport", "serializer", "value-size", "concurrency", "tick-cadence",
                             "cost-attribution", "open-loop", "read-taxonomy", "shard-size"}) {
        drop(axis, "every row",
             "KYTHIRA_BENCH_HAS_BEAST undefined (requires KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT); "
             "Beast is the transport every other axis holds fixed");
    }
#endif  // KYTHIRA_BENCH_HAS_BEAST

    return out;
}

auto print_usage(std::ostream& out, const char* program) -> void {
    out << "Usage: " << program << " [options]\n"
        << "  --list                 print the catalog and exit\n"
        << "  --axis NAME            run only rows whose axis contains NAME (repeatable)\n"
        << "  --scenario NAME        run only rows whose scenario contains NAME (repeatable)\n"
        << "  --tier A|B             run only rows at that tier (repeatable)\n"
        << "  --rate OPS             offered rate for the open-loop axis (default 400)\n"
        << "  --budget-scale F       multiply every row's operation budget by F\n"
        << "  --out-dir DIR          where the CSV and JSON go (default test_results)\n"
        << "  --help                 this text\n"
        << "\nWith no selector, every row in the catalog runs. That is the full\n"
        << "matrix and is measured in hours; --list first.\n";
}

auto parse(int argc, char** argv, selection& sel, bool& want_help) -> bool {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto value = [&](const char* what) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error(std::string{what} + " needs a value");
            }
            return argv[++i];
        };
        if (arg == "--list") {
            sel._list = true;
        } else if (arg == "--axis") {
            sel._axes.push_back(value("--axis"));
        } else if (arg == "--scenario") {
            sel._scenarios.push_back(value("--scenario"));
        } else if (arg == "--tier") {
            sel._tiers.push_back(value("--tier"));
        } else if (arg == "--rate") {
            sel._rate = std::stod(value("--rate"));
        } else if (arg == "--budget-scale") {
            sel._budget_scale = std::stod(value("--budget-scale"));
        } else if (arg == "--out-dir") {
            sel._out_dir = value("--out-dir");
        } else if (arg == "--help" || arg == "-h") {
            want_help = true;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            return false;
        }
    }
    return true;
}

/// @brief The machine, printed the way the suite prints it, so two logs of the
///        same host are comparable by eye.
auto report_machine(const machine_description& m) -> void {
    std::cout << std::fixed << std::setprecision(2) << "machine (Requirement 6.4):"
              << "\n    cpu:      " << m._cpu_model << " x" << m._logical_cpus << " logical"
              << "\n    memory:   " << static_cast<double>(m._memory_bytes) / (1024.0 * 1024 * 1024)
              << " GiB"
              << "\n    kernel:   " << m._kernel << "\n    storage:  " << m._described_path
              << " on " << m._filesystem << ", device " << m._device << " (" << m._device_kind
              << ")"
              << "\n    build:    " << m._compiler << ", " << m._build_type << ", sanitizer "
              << m._sanitizer << ", future backend " << m._future_backend
              << "\n    flags:    " << m._cxx_flags << "\n    load:     " << m._load_average_1m
              << " (1m) — "
              << (m._quiet_at_start ? "machine was quiet at start"
                                    : "OTHER LOAD WAS PRESENT AT START; these numbers are not "
                                      "publication-grade (Requirement 6.5)")
              << "\n";
}

/// @brief Name every row this build cannot run, with the reason (Requirement
///        13.4).
///
/// Printed even when the list is empty, and *saying so*, because "no rows were
/// dropped" and "the program does not track dropped rows" look identical
/// otherwise, and only one of them means the matrix is complete.
auto report_dropped(const std::vector<dropped_row>& dropped) -> void {
    if (dropped.empty()) {
        std::cout << "optional dependencies: none missing — the catalog below is the whole "
                     "matrix this build defines (Requirement 13.4)\n";
        return;
    }
    std::cout << "rows this build CANNOT run (" << dropped.size()
              << "), named rather than omitted (Requirement 13.4):\n";
    for (const auto& d : dropped) {
        std::cout << "  " << std::left << std::setw(18) << d._axis << "  " << d._scenario << "\n"
                  << "      " << d._reason << "\n";
    }
}

auto report_row_summary(const report_row& row) -> void {
    const auto& r = row._result;
    if (r.runs() == 0) {
        std::cout << "  " << row._axis << " / " << row._scenario << ": NOT MEASURED\n";
        return;
    }
    const auto& m = r.median_run();
    std::cout << std::fixed << std::setprecision(1) << "  " << row._axis << " / " << row._scenario
              << "\n      tier:     " << to_string(m._tier)
              << (kythira::testing::publishable_as_like_for_like(m._tier)
                      ? ""
                      : " — NOT a like-for-like comparison with any external number "
                        "(Requirement 3.3)")
              << "\n      load:     " << to_string(m._load);
    if (m._load == load_mode::open_loop) {
        std::cout << " at " << m._offered_rate_per_second << " ops/sec, mean schedule lag "
                  << us(m._mean_schedule_lag) << "us";
    }
    std::cout << "\n      headline: ";
    if (const auto headline = r.headline_ops_per_second()) {
        std::cout << *headline << " ops/sec (median of " << r.runs() << " runs)";
    } else {
        std::cout << "NONE — " << r.runs() << " run(s), " << k_required_repetitions << " required";
    }
    // Both spreads, always, with the governing one named. In open loop the
    // throughput spread is near zero because the schedule pinned it, and a row
    // printing only that would read as the most stable measurement in the
    // suite while its p50 ranged over two orders of magnitude.
    std::cout << " | throughput spread " << (r.spread() * 100.0) << "%, p50 spread "
              << (r.p50_spread() * 100.0) << "%"
              << "\n      verdict:  " << to_string(r.verdict())
              << (m._load == load_mode::open_loop
                      ? " (judged on the p50 spread; in open loop the offered rate is an input, "
                        "so its spread is a tautology)"
                      : " (judged on the throughput spread)")
              << (r.comparable() ? "" : " — MUST NOT ENTER A COMPARISON TABLE (Requirement 6.3)")
              << "\n      p50=" << us(m._p50) << "us p95=" << us(m._p95) << "us\n";
}

}  // namespace

auto main(int argc, char** argv) -> int {
    selection sel;
    bool want_help = false;
    try {
        if (!parse(argc, argv, sel, want_help)) {
            print_usage(std::cerr, argv[0]);
            return 2;
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
    if (want_help) {
        print_usage(std::cout, argv[0]);
        return 0;
    }

#if !defined(KYTHIRA_FUTURE_BACKEND_STDEXEC) && !defined(KYTHIRA_FUTURE_BACKEND_BOOST)
    // A synthetic argv, so folly's gflags parsing does not trip over this
    // program's own arguments — the same reason
    // `future_backend_benchmark_report` builds one.
    int folly_argc = 1;
    char* folly_argv_data[] = {const_cast<char*>("multi_raft_performance_report"), nullptr};
    char** folly_argv = folly_argv_data;
    folly::Init folly_init(&folly_argc, &folly_argv);
#endif

    const auto catalog = build_catalog(sel);

    // Requirement 13.4, printed before anything else and on every invocation,
    // not only `--list`: what this build could not measure is part of what a
    // reader needs in order to know the matrix is smaller than it looks.
    report_dropped(catalog._dropped);

    if (sel._list) {
        std::cout << "catalog (" << catalog._entries.size() << " rows, " << k_required_repetitions
                  << " repetitions each):\n";
        for (const auto& entry : catalog._entries) {
            std::cout << "  tier " << tier_letter(entry._tier) << "  " << std::left << std::setw(18)
                      << entry._axis << "  " << entry._scenario << "\n";
        }
        return 0;
    }

    // Captured before the first row runs, for the reason the suite captures it
    // in a global fixture: the load average is the point, and by the second row
    // it is measuring this program.
    const auto machine = describe_machine(".");
    report_machine(machine);

    std::vector<report_row> rows;
    std::size_t abandoned = 0;
    const auto observer = console_observer();
    for (const auto& entry : catalog._entries) {
        if (!matches(sel._axes, entry._axis) || !matches(sel._scenarios, entry._scenario) ||
            !matches(sel._tiers, tier_letter(entry._tier))) {
            continue;
        }
        std::cout << "\n=== " << entry._axis << " / " << entry._scenario << " (tier "
                  << tier_letter(entry._tier) << ") ===\n";
        report_row row{._axis = entry._axis, ._scenario = entry._scenario, ._result = {}};
        try {
            row._result = entry._run(observer);
        } catch (const std::exception& e) {
            // One row's precondition failed. Recorded as not measured and the
            // matrix continues — see `console_observer`. The row is still
            // emitted, with zero repetitions and therefore no headline, so the
            // artifact says "not measured" rather than omitting it silently.
            std::cout << "    ROW ABANDONED: " << e.what() << "\n";
            ++abandoned;
        }
        report_row_summary(row);
        rows.push_back(std::move(row));
    }

    if (rows.empty()) {
        std::cerr << "no row matched the selection; try --list\n";
        return 1;
    }

    std::filesystem::create_directories(sel._out_dir);
    const auto timestamp = make_timestamp();
    const auto csv_path = sel._out_dir / ("multi_raft_performance_" + timestamp + ".csv");
    const auto json_path = sel._out_dir / ("multi_raft_performance_" + timestamp + ".json");
    write_csv(csv_path, rows, machine);
    write_json(json_path, rows, machine, timestamp, catalog._dropped);

    std::cout << "\n"
              << rows.size() << " row(s) measured, " << abandoned << " abandoned\n"
              << "  " << csv_path.string() << "\n"
              << "  " << json_path.string() << "\n";
    if (!machine._quiet_at_start) {
        std::cout << "  NOT PUBLICATION-GRADE: other load was present at start (Requirement 6.5)\n";
    }
    // A run in which every row was abandoned produced artifacts describing
    // nothing, and exiting zero would let a scheduled invocation report success
    // for it.
    return abandoned == rows.size() ? 1 : 0;
}
