// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

/// @file main.cpp
/// @brief `multi_raft_bench` — the load driver, in its own process.
///        `.kiro/specs/multi-raft-host-binary/` Requirement 3.
///
/// **It hosts no Raft node** (Requirement 3.1), and that is the expensive
/// answer to Appendix B's question 2, chosen deliberately. A driver sharing a
/// process with a host makes the host's CPU and the client's indistinguishable
/// in every number: the key sampling, value construction, latency bookkeeping
/// and future settlement all run on the same cores as the tick, the transport
/// and consensus. At sixteen operations in flight on four cores that is not a
/// rounding error, and removing it is the entire point of Tiers C, D and E.
///
/// **Everything except the submit step is shared with the in-process
/// harness.** The sampler, the value construction, the command mix, the read
/// taxonomy, both load modes with the intended-start-time rule, and the
/// statistics — five repetitions, the spread rule, and a verdict that refuses a
/// headline to an unstable row — are `tests/multi_raft_kv_workload.hpp` and
/// `tests/multi_raft_transport_harness.hpp`, unchanged. What differs is
/// `data_path_target`, and nothing else may.

#include "data_path_target.hpp"
#include "kv_data_path.hpp"

#include "multi_raft_kv_workload.hpp"
#include "multi_raft_report_artifacts.hpp"
#include "multi_raft_row_report.hpp"
#include "multi_raft_transport_harness.hpp"

#include <folly/init/Init.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

using kythira::bench::data_path_target;
using kythira::bench::host_endpoint;
using kythira::bench::target_options;
using kythira::testing::benchmark_result;
using kythira::testing::deployment_tier;
using kythira::testing::key_distribution;
using kythira::testing::load_mode;
using kythira::testing::read_kind;
using kythira::testing::repeated_result;
using kythira::testing::report_row;
using kythira::testing::workload_options;

struct driver_options {
    target_options _target;
    workload_options _workload;
    /// Requirement 4.2: five repetitions, the spread rule, and a verdict. The
    /// default is the required count rather than a convenient one — a row with
    /// fewer gets `inconclusive` and no headline, by construction.
    std::size_t _repetitions{kythira::testing::k_required_repetitions};
    std::size_t _warmup{50};
    /// `write`, or one of the three read kinds.
    std::string _scenario{"write"};
    std::string _axis{"tier-c"};
    std::chrono::milliseconds _ready_timeout{60000};
    /// Requirement 3.5: a Tier E number without the driver's placement is not
    /// reproducible, so the placement is a required input rather than an
    /// inference. Free text, recorded verbatim on the row.
    std::string _placement{"not stated"};
    std::filesystem::path _out_dir{"test_results"};
    bool _write_artifacts{true};
};

[[nodiscard]] auto usage() -> std::string {
    return R"(multi_raft_bench — the load driver for multi_raft_node.

Hosts no Raft node. Offers load over the data path and measures latency from
the client's side of the wire, which is what makes a Tier C or Tier E number
the cluster's rather than the process's.

Usage: multi_raft_bench --host 1@127.0.0.1:9101:9201 [--host ...] [options]

Cluster
  --host ID@ADDR:DATA:CONTROL  A host's node id, address, data port and
                               control port. Repeatable, once per host.
  --groups N                   Shards the hosts were PRE-SPLIT into (default 4).
                               Must match the hosts, or the driver's idea of
                               which shard holds a key is not theirs.
  --key-space N                The key space those ranges tile (default 100000).
                               Must match the hosts' --key-count, or the
                               driver's idea of which shard holds a key is not
                               theirs.
  --sample-keys N              How many distinct keys the workload draws from
                               (default 100000). Separate from --key-space for
                               the same reason the in-process harness keeps
                               them separate: a read row draws from a small,
                               preloaded set while the ranges still tile the
                               whole space.
  --key-stride N               Multiplies every sampled key index (default 1).
                               At 1 the keys are contiguous and land in one
                               shard, which is what a shard-size curve wants;
                               at key-space/sample-keys they spread evenly.
  --tier NAME                  c | e. What the row says it is: the driver
                               cannot tell one machine from several and must be
                               told (Requirement 3.5).
  --placement TEXT             Where the hosts and this driver are, recorded
                               verbatim on the row. A Tier E number without it
                               is not reproducible.
  --transport NAME             What the hosts were configured with, for the
                               row's label (default cpp-httplib).
  --durability MODE            memory | file-buffered | file-barrier, for the
                               row's label. file-buffered is NOT DURABLE.
  --tick-interval MS           What the hosts were configured with (default 2).

Workload — identical in name and meaning to the in-process harness
  --operations N               Measured operations per repetition (default 400).
  --in-flight N                Concurrency, or open loop's worker count (8).
  --value-bytes N              (default 128)
  --distribution NAME          uniform | zipfian (default uniform)
  --zipf-theta F               (default 0.99)
  --load MODE                  closed | open (default closed)
  --rate F                     Open loop's offered operations per second.
  --scenario NAME              write | read-state | read-log | read-local
  --op-timeout MS              (default 3000)
  --repetitions N              (default 5; fewer yields no headline at all)
  --warmup N                   Operations discarded before each window (50).

Output
  --axis NAME                  The axis label on the row (default tier-c).
  --out-dir DIR                Where the CSV/JSON pair is written (test_results).
  --no-artifacts               Print the row and write no files.
  --ready-timeout MS           How long to wait for every group to have a
                               leader before offering load (default 60000).
  --help
)";
}

[[nodiscard]] auto to_u64(const std::string& s, const char* what) -> std::uint64_t {
    try {
        return static_cast<std::uint64_t>(std::stoull(s));
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("multi_raft_bench: ") + what + ": not a number: " + s);
    }
}

/// `ID@ADDR:DATA:CONTROL`
auto add_host(driver_options& out, const std::string& spec) -> void {
    const auto at = spec.find('@');
    if (at == std::string::npos) {
        throw std::runtime_error("multi_raft_bench: --host expects ID@ADDR:DATA:CONTROL: " + spec);
    }
    const auto rest = spec.substr(at + 1);
    const auto first = rest.rfind(':');
    if (first == std::string::npos) {
        throw std::runtime_error("multi_raft_bench: --host is missing the control port: " + spec);
    }
    const auto second = rest.rfind(':', first - 1);
    if (second == std::string::npos) {
        throw std::runtime_error("multi_raft_bench: --host is missing the data port: " + spec);
    }
    out._target._hosts.push_back(host_endpoint{
        ._node_id = to_u64(spec.substr(0, at), "--host id"),
        ._address = rest.substr(0, second),
        ._data_port = static_cast<std::uint16_t>(
            to_u64(rest.substr(second + 1, first - second - 1), "--host data port")),
        ._control_port =
            static_cast<std::uint16_t>(to_u64(rest.substr(first + 1), "--host control port"))});
}

[[nodiscard]] auto parse(int argc, char** argv) -> driver_options {
    driver_options out;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag == "--help" || flag == "-h") {
            throw std::invalid_argument("help");
        }
        if (flag == "--no-artifacts") {
            out._write_artifacts = false;
            continue;
        }
        if (i + 1 >= argc) {
            throw std::runtime_error("multi_raft_bench: " + flag + " needs a value");
        }
        const std::string value = argv[++i];
        if (flag == "--host") {
            add_host(out, value);
        } else if (flag == "--groups") {
            out._target._groups = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--key-space") {
            out._target._key_count = to_u64(value, flag.c_str());
        } else if (flag == "--sample-keys") {
            out._workload._key_count = to_u64(value, flag.c_str());
        } else if (flag == "--key-stride") {
            out._workload._key_stride = to_u64(value, flag.c_str());
        } else if (flag == "--tier") {
            out._target._tier =
                value == "e" ? deployment_tier::e_multi_machine : deployment_tier::c_process;
        } else if (flag == "--placement") {
            out._placement = value;
            out._target._placement = value;
        } else if (flag == "--transport") {
            out._target._transport_name = value;
        } else if (flag == "--durability") {
            out._target._durability =
                value == "file-barrier"    ? kythira::testing::durability_mode::file_barrier
                : value == "file-buffered" ? kythira::testing::durability_mode::file_buffered
                                           : kythira::testing::durability_mode::memory;
        } else if (flag == "--tick-interval") {
            out._target._tick_interval =
                std::chrono::milliseconds{static_cast<std::int64_t>(to_u64(value, flag.c_str()))};
        } else if (flag == "--operations") {
            out._workload._operations = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--in-flight") {
            out._workload._in_flight = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--value-bytes") {
            out._workload._value_bytes = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--distribution") {
            out._workload._distribution =
                value == "zipfian" ? key_distribution::zipfian : key_distribution::uniform;
        } else if (flag == "--zipf-theta") {
            out._workload._zipf_theta = std::stod(value);
        } else if (flag == "--load") {
            out._workload._load = value == "open" ? load_mode::open_loop : load_mode::closed_loop;
        } else if (flag == "--rate") {
            out._workload._offered_rate_per_second = std::stod(value);
        } else if (flag == "--scenario") {
            out._scenario = value;
            out._workload._scenario = value;
        } else if (flag == "--op-timeout") {
            out._workload._op_timeout =
                std::chrono::milliseconds{static_cast<std::int64_t>(to_u64(value, flag.c_str()))};
        } else if (flag == "--repetitions") {
            out._repetitions = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--warmup") {
            out._warmup = static_cast<std::size_t>(to_u64(value, flag.c_str()));
        } else if (flag == "--axis") {
            out._axis = value;
        } else if (flag == "--out-dir") {
            out._out_dir = value;
        } else if (flag == "--ready-timeout") {
            out._ready_timeout =
                std::chrono::milliseconds{static_cast<std::int64_t>(to_u64(value, flag.c_str()))};
        } else {
            throw std::runtime_error("multi_raft_bench: unknown option " + flag);
        }
    }
    if (out._target._hosts.empty()) {
        throw std::runtime_error("multi_raft_bench: at least one --host is required");
    }
    out._target._media_type = "application/json";
    return out;
}

[[nodiscard]] auto read_kind_of(const std::string& scenario) -> std::optional<read_kind> {
    if (scenario == "read-state") {
        return read_kind::read_state;
    }
    if (scenario == "read-log") {
        return read_kind::log_get;
    }
    if (scenario == "read-local") {
        return read_kind::local_stale;
    }
    return std::nullopt;
}

}  // namespace

auto main(int argc, char** argv) -> int {
    // **Parsed before `folly::Init`, and folly is then given an argv holding
    // only the program name.** `folly::Init` hands the real one to gflags,
    // which rejects every flag it does not recognise — and every flag here is
    // one it does not recognise. `chaos_node` sidesteps this by taking its
    // configuration from the environment; this binary has a command line
    // because Requirement 1.2 asks for one, so it keeps gflags away from it.
    driver_options opt;
    try {
        opt = parse(argc, argv);
    } catch (const std::invalid_argument&) {
        std::cout << usage();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n\n" << usage();
        return 2;
    }

    int folly_argc = 1;
    char* folly_argv_storage[] = {argv[0], nullptr};
    char** folly_argv = folly_argv_storage;
    folly::Init init(&folly_argc, &folly_argv);

    data_path_target target{opt._target};
    if (!target.await_ready(opt._ready_timeout)) {
        std::cerr << "multi_raft_bench: the cluster did not report every group led within "
                  << opt._ready_timeout.count()
                  << " ms. Refusing to offer load: a window containing an election is not a "
                     "window measuring a workload.\n";
        return 1;
    }

    const auto kind = read_kind_of(opt._scenario);

    repeated_result row;
    row._warmup_operations = opt._warmup;
    row._measured_operations = opt._workload._operations;

    // A read row needs a loaded store. Preloading is setup, not a measurement,
    // and it is single-threaded and sequential for the same reason the harness's
    // is: a concurrent preloader only adds ways for it to half-fail.
    if (kind.has_value()) {
        const auto loaded = kythira::testing::preload_keys_on(
            target, opt._workload._key_count, opt._workload._key_stride, opt._workload._value_bytes,
            opt._workload._op_timeout);
        if (loaded < opt._workload._key_count) {
            std::cerr << "multi_raft_bench: preloaded only " << loaded << " of "
                      << opt._workload._key_count
                      << " keys; a read row over a partly loaded store measures a different "
                         "thing from the one it claims\n";
            return 1;
        }
    }

    std::size_t windows_with_an_election = 0;
    for (std::size_t r = 0; r < opt._repetitions; ++r) {
        // Out of band and before the window: leadership moves between
        // repetitions, and a driver addressing last repetition's leader would
        // spend this one's first request per group learning that. The
        // in-process harness takes its descriptor cache the same way, once,
        // before the clock.
        target.refresh_leaders();
        // Warm-up, discarded. Requirement 6.1 of the performance spec, and the
        // same count the in-process rows use so the two are comparable.
        if (opt._warmup > 0) {
            auto warm = opt._workload;
            warm._operations = opt._warmup;
            if (kind.has_value()) {
                (void)kythira::testing::run_read_workload_on(target, warm, *kind);
            } else {
                (void)kythira::testing::run_put_workload_on(target, warm);
            }
        }
        const auto terms_before = target.term_sum();
        auto run = kind.has_value()
                       ? kythira::testing::run_read_workload_on(target, opt._workload, *kind)
                       : kythira::testing::run_put_workload_on(target, opt._workload);
        const auto terms_after = target.term_sum();
        const bool elected = terms_after != terms_before;
        windows_with_an_election += elected ? 1 : 0;

        row.record(std::move(run));
        std::cerr << "    run " << (r + 1) << '/' << opt._repetitions << ": " << std::fixed
                  << std::setprecision(1) << row._runs.back()._ops_per_second << " ops/sec"
                  << (elected ? "  [AN ELECTION HAPPENED IN THIS WINDOW]" : "") << '\n';
    }

    // The same reporting the in-process rows get, **from the same function**,
    // so the two are readable side by side (Requirement 3.4). Two printers
    // would satisfy that on the day they were written and drift the week after.
    auto machine = kythira::testing::describe_machine();
    std::cout << kythira::testing::describe_row(row, machine._quiet_at_start) << '\n';
    std::cout << "      placement: " << opt._placement << '\n';
    std::cout << "      elections: " << windows_with_an_election << " of " << opt._repetitions
              << " measured windows contained one. Reported, never asserted — a window that "
                 "contained an election is not a window measuring a workload, and a row that "
                 "cannot say whether it did cannot be read at all.\n";

    if (opt._write_artifacts) {
        std::vector<report_row> rows{
            report_row{._axis = opt._axis, ._scenario = opt._scenario, ._result = row}};
        const auto stamp = kythira::testing::make_timestamp();
        std::filesystem::create_directories(opt._out_dir);
        kythira::testing::write_csv(opt._out_dir / ("multi_raft_bench_" + stamp + ".csv"), rows,
                                    machine);
        kythira::testing::write_json(opt._out_dir / ("multi_raft_bench_" + stamp + ".json"), rows,
                                     machine, {});
        std::cout << "      artifacts: " << (opt._out_dir / ("multi_raft_bench_" + stamp)).string()
                  << ".{csv,json}\n";
    }

    return row.comparable() ? 0 : 0;
}
