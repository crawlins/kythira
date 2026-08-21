// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

// A real, measured throughput/latency comparison of this project's RPC
// transports -- cpp-httplib (include/raft/http_transport.hpp),
// Boost.Beast (include/raft/beast_http_transport.hpp), Proxygen
// (include/raft/proxygen_http_transport.hpp), and, when built with it,
// gRPC (include/raft/grpc_transport.hpp, .kiro/specs/grpc-transport/
// Task 13.4's performance sanity pass) -- running the *same* RequestVote
// RPC workload against each in turn, all in one process. See
// doc/http_transport_performance_comparison.md for the methodology write-up
// and the measured numbers this program produced.
//
// gRPC is not an HTTP-transport sibling in the code (it owns its framing
// and codegen rather than pairing a serializer_type with an HTTP client),
// but it answers to the same question this program exists for -- "what does
// one RequestVote round trip cost on each thing that can carry one?" -- so
// it is a fourth row here rather than a second program duplicating the
// harness. Guarded by KYTHIRA_BENCH_HAS_GRPC (set in
// examples/raft/CMakeLists.txt when raft_grpc_transport exists) because
// gRPC availability is independent of the two HTTP-transport flags that
// gate this binary.
//
// This is a comparison, not a sanity floor: it does not gate CI on any
// transport beating another by a specific margin (matching
// .kiro/specs/future-backend-performance-benchmark/'s own established
// distinction for the analogous Folly-vs-stdexec comparison) -- relative
// performance is expected to shift across hardware/compiler/library
// versions. Only built when both KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT and
// KYTHIRA_BUILD_PROXYGEN_TRANSPORT are enabled (examples/raft/CMakeLists.txt),
// since it needs all three HTTP transports available in the same binary.

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/executor_default.hpp>

#if defined(KYTHIRA_BENCH_HAS_GRPC)
#include <raft/grpc_transport.hpp>
#include <raft/grpc_transport_impl.hpp>

#include <folly/executors/CPUThreadPoolExecutor.h>
#endif

#include <folly/executors/IOThreadPoolExecutor.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::uint64_t node_id = 1;
// Not const: overridable via --iterations/--warmup so the CTest-registered
// entry (examples/raft/CMakeLists.txt) can run a fast, reduced-iteration
// smoke test (catches a build/runtime regression in this program itself,
// not a real comparison) while a manual invocation with no arguments still
// gets the full report-quality run -- mirroring
// future_backend_benchmark_report_test's own established precedent
// (examples/CMakeLists.txt) for the analogous Folly-vs-stdexec comparison.
int warmup_iterations = 200;
int measured_iterations = 2000;
constexpr std::chrono::milliseconds rpc_timeout{5000};

using http_types = kythira::http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
using beast_types =
    kythira::future_default_http_transport_types<kythira::json_serializer, kythira::noop_metrics,
                                                 kythira::executor_default>;
using proxygen_types = kythira::future_default_proxygen_transport_types<
    kythira::json_serializer, kythira::noop_metrics, kythira::executor_default>;

struct scenario_result {
    std::string transport_name;
    double ops_per_second{0.0};
    double p50_us{0.0};
    double p95_us{0.0};
    double p99_us{0.0};
};

auto percentile(std::vector<double>& sorted_samples_us, double pct) -> double {
    if (sorted_samples_us.empty()) {
        return 0.0;
    }
    auto idx = static_cast<std::size_t>(pct * static_cast<double>(sorted_samples_us.size() - 1));
    return sorted_samples_us[idx];
}

auto summarize(std::string_view name, std::vector<double> samples_us,
               std::chrono::steady_clock::duration total_elapsed) -> scenario_result {
    std::sort(samples_us.begin(), samples_us.end());
    scenario_result result;
    result.transport_name = std::string(name);
    auto total_seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(total_elapsed).count();
    result.ops_per_second = static_cast<double>(samples_us.size()) / total_seconds;
    result.p50_us = percentile(samples_us, 0.50);
    result.p95_us = percentile(samples_us, 0.95);
    result.p99_us = percentile(samples_us, 0.99);
    return result;
}

auto bench_cpp_httplib(std::uint16_t port) -> scenario_result {
    kythira::cpp_httplib_server<http_types> server("127.0.0.1", port, {}, kythira::noop_metrics{});
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return {._term = req.term(), ._vote_granted = true};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::cpp_httplib_client<http_types> client(node_map, {}, kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_iterations; ++i) {
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize("cpp-httplib", std::move(samples_us), elapsed);
}

auto bench_beast(std::uint16_t port) -> scenario_result {
    boost::asio::io_context ioc;
    auto work_guard = boost::asio::make_work_guard(ioc);
    std::vector<std::thread> io_threads;
    for (int i = 0; i < 2; ++i) {
        io_threads.emplace_back([&ioc] { ioc.run(); });
    }

    kythira::boost_beast_server<beast_types> server(ioc, "127.0.0.1", port, {},
                                                    kythira::noop_metrics{});
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return {._term = req.term(), ._vote_granted = true};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::boost_beast_client<beast_types> client(ioc, node_map, {}, kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_iterations; ++i) {
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    work_guard.reset();
    ioc.stop();
    for (auto& t : io_threads) {
        t.join();
    }
    return summarize("Boost.Beast", std::move(samples_us), elapsed);
}

// Requirement 17.1 (.kiro/specs/proxygen-http-transport/): compares
// Proxygen's own generic bridge (Requirement 14) against its Folly fast
// path (Requirement 16, bench_proxygen above) for the *same* RPC-shaped
// operation, both under this program's KYTHIRA_DEFAULT_FUTURE_BACKEND=folly
// build -- Property 12's test-only escape hatch
// (proxygen_client::send_rpc_via_generic_bridge_for_test,
// proxygen_http_transport.hpp) is what makes this comparison possible
// without a second, non-Folly build entirely (there would be no way to
// reach the generic bridge under a Folly-backend Types bundle otherwise --
// send_rpc's own if-constexpr dispatch, Requirement 16.1, would always
// select the fast path).
auto bench_proxygen_generic_bridge(std::uint16_t port) -> scenario_result {
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(4);

    kythira::proxygen_server<proxygen_types> server("127.0.0.1", port, {}, kythira::noop_metrics{},
                                                    io_executor);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return {._term = req.term(), ._vote_granted = true};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<proxygen_types> client(*io_executor, node_map, {},
                                                    kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_iterations; ++i) {
        std::move(client.send_rpc_via_generic_bridge_for_test<kythira::request_vote_request<>,
                                                              kythira::request_vote_response<>>(
                      node_id, kythira::proxygen_detail::proxygen_endpoint_request_vote, req,
                      rpc_timeout))
            .get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_rpc_via_generic_bridge_for_test<kythira::request_vote_request<>,
                                                              kythira::request_vote_response<>>(
                      node_id, kythira::proxygen_detail::proxygen_endpoint_request_vote, req,
                      rpc_timeout))
            .get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize("Proxygen (generic bridge)", std::move(samples_us), elapsed);
}

// Requirement 17.3: the Introduction's zero-copy folly::IOBuf claim was, at
// authoring time, explicitly unmeasured (design.md's own Non-Goals /
// Post-Spike Addendum note the accumulate-into-std::string posture this
// first cut actually has -- proxygen_detail::http_response, this feature's
// own header comment). This scenario is what turns "architectural
// expectation" into "measured": a large (install_snapshot-sized) body,
// round-tripped through both Proxygen paths, so a reader can see whether
// the fast path's shorter translation chain (Requirement 16's own point)
// shows up more or less at this body size than at the small-RequestVote
// size the scenarios above use.
auto bench_proxygen_large_snapshot_body(std::uint16_t port, bool use_fast_path) -> scenario_result {
    constexpr std::size_t body_size = 1024 * 1024;  // 1 MiB -- install_snapshot-sized
                                                    // (Introduction's own framing: the one
                                                    // RPC this project's Raft implementation
                                                    // already treats as having a large body).
    constexpr int large_body_warmup_iterations = 20;
    constexpr int large_body_measured_iterations = 200;  // fewer than the small-body scenarios --
                                                         // 1 MiB x 2000 iterations would dominate
                                                         // this program's runtime for no added
                                                         // measurement value.

    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(4);
    kythira::proxygen_server<proxygen_types> server("127.0.0.1", port, {}, kythira::noop_metrics{},
                                                    io_executor);
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            return {._term = req.term()};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<proxygen_types> client(*io_executor, node_map, {},
                                                    kythira::noop_metrics{});

    kythira::install_snapshot_request<> req{};
    req._term = 1;
    req._leader_id = node_id;
    req._last_included_index = 1;
    req._last_included_term = 1;
    req._offset = 0;
    req._data.assign(body_size, std::byte{0x42});
    req._done = true;

    auto call_once = [&] {
        if (use_fast_path) {
            return std::move(client.send_install_snapshot(node_id, req, rpc_timeout)).get();
        }
        return std::move(
                   client.send_rpc_via_generic_bridge_for_test<
                       kythira::install_snapshot_request<>, kythira::install_snapshot_response<>>(
                       node_id, kythira::proxygen_detail::proxygen_endpoint_install_snapshot, req,
                       rpc_timeout))
            .get();
    };

    for (int i = 0; i < large_body_warmup_iterations; ++i) {
        call_once();
    }

    std::vector<double> samples_us;
    samples_us.reserve(large_body_measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < large_body_measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        call_once();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize(use_fast_path ? "Proxygen 1MiB snapshot (fast path)"
                                   : "Proxygen 1MiB snapshot (generic bridge)",
                     std::move(samples_us), elapsed);
}

auto bench_proxygen(std::uint16_t port) -> scenario_result {
    auto io_executor = std::make_shared<folly::IOThreadPoolExecutor>(4);

    kythira::proxygen_server<proxygen_types> server("127.0.0.1", port, {}, kythira::noop_metrics{},
                                                    io_executor);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return {._term = req.term(), ._vote_granted = true};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, std::string("http://127.0.0.1:") + std::to_string(port)}};
    kythira::proxygen_client<proxygen_types> client(*io_executor, node_map, {},
                                                    kythira::noop_metrics{});

    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_iterations; ++i) {
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize("Proxygen (Folly fast path)", std::move(samples_us), elapsed);
}

#if defined(KYTHIRA_BENCH_HAS_GRPC)
// Task 13.4 (.kiro/specs/grpc-transport/): the transport-level performance
// sanity pass -- doc/protobuf_serializer_performance_comparison.md measures
// the *serializer* and explicitly does not satisfy this. Same workload and
// measurement discipline as the three HTTP scenarios above; the differences
// are the transport's own: gRPC carries protobuf over its own HTTP/2
// framing (no serializer_type/JSON involved), the endpoint is a bare
// host:port, and the client wants a CPU executor for its completion hops.
//
// Port 0 rather than a fixed 2809x port: grpc_server supports bind-0 +
// bound_port() (the same SO_REUSEADDR silent-port-steal reasoning as
// examples/grpc_transport_example.cpp), and nothing here needs the port
// known in advance.
auto bench_grpc() -> scenario_result {
    using grpc_types = kythira::grpc_kythira_transport_types;
    folly::CPUThreadPoolExecutor exec(4);

    kythira::grpc_server<grpc_types> server("127.0.0.1", 0, {}, kythira::noop_metrics{}, exec);
    server.register_request_vote_handler(
        [](const kythira::request_vote_request<>& req) -> kythira::request_vote_response<> {
            return {._term = req.term(), ._vote_granted = true};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, "127.0.0.1:" + std::to_string(server.bound_port())}};
    kythira::grpc_client<grpc_types> client(node_map, {}, kythira::noop_metrics{}, exec);

    kythira::request_vote_request<> req{};
    req._term = 1;
    for (int i = 0; i < warmup_iterations; ++i) {
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_request_vote(node_id, req, rpc_timeout)).get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize("gRPC", std::move(samples_us), elapsed);
}

// The gRPC row for the 1 MiB install_snapshot table: same body size and
// iteration counts as bench_proxygen_large_snapshot_body, so the three rows
// of that table are directly comparable. Worth measuring separately from
// the small-RequestVote scenario because this is where the transports'
// body-handling actually diverges: both Proxygen paths JSON-encode the
// snapshot's byte vector (base64-ish inflation through boost::json), while
// gRPC carries it as a protobuf `bytes` field over its own framing.
auto bench_grpc_large_snapshot_body() -> scenario_result {
    constexpr std::size_t body_size = 1024 * 1024;
    constexpr int large_body_warmup_iterations = 20;
    constexpr int large_body_measured_iterations = 200;

    using grpc_types = kythira::grpc_kythira_transport_types;
    folly::CPUThreadPoolExecutor exec(4);

    kythira::grpc_server<grpc_types> server("127.0.0.1", 0, {}, kythira::noop_metrics{}, exec);
    server.register_install_snapshot_handler(
        [](const kythira::install_snapshot_request<>& req) -> kythira::install_snapshot_response<> {
            return {._term = req.term()};
        });
    server.start();

    std::unordered_map<std::uint64_t, std::string> node_map{
        {node_id, "127.0.0.1:" + std::to_string(server.bound_port())}};
    kythira::grpc_client<grpc_types> client(node_map, {}, kythira::noop_metrics{}, exec);

    kythira::install_snapshot_request<> req{};
    req._term = 1;
    req._leader_id = node_id;
    req._last_included_index = 1;
    req._last_included_term = 1;
    req._offset = 0;
    req._data.assign(body_size, std::byte{0x42});
    req._done = true;

    for (int i = 0; i < large_body_warmup_iterations; ++i) {
        std::move(client.send_install_snapshot(node_id, req, rpc_timeout)).get();
    }

    std::vector<double> samples_us;
    samples_us.reserve(large_body_measured_iterations);
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < large_body_measured_iterations; ++i) {
        auto call_start = std::chrono::steady_clock::now();
        std::move(client.send_install_snapshot(node_id, req, rpc_timeout)).get();
        auto call_end = std::chrono::steady_clock::now();
        samples_us.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(
                                 call_end - call_start)
                                 .count());
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    server.stop();
    return summarize("gRPC 1MiB snapshot", std::move(samples_us), elapsed);
}
#endif  // KYTHIRA_BENCH_HAS_GRPC

auto print_table(const std::vector<scenario_result>& results) -> void {
    std::cout << "\n";
    std::cout << std::left << std::setw(30) << "Transport" << std::right << std::setw(14)
              << "ops/sec" << std::setw(12) << "p50 (us)" << std::setw(12) << "p95 (us)"
              << std::setw(12) << "p99 (us)" << "\n";
    std::cout << std::string(80, '-') << "\n";
    for (const auto& r : results) {
        std::cout << std::left << std::setw(30) << r.transport_name << std::right << std::setw(14)
                  << std::fixed << std::setprecision(0) << r.ops_per_second << std::setw(12)
                  << std::setprecision(1) << r.p50_us << std::setw(12) << r.p95_us << std::setw(12)
                  << r.p99_us << "\n";
    }
    std::cout << "\n";
}

}  // namespace

auto main(int argc, char** argv) -> int {
    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--iterations" && i + 1 < argc) {
            measured_iterations = std::atoi(argv[++i]);
        } else if (arg == "--warmup" && i + 1 < argc) {
            warmup_iterations = std::atoi(argv[++i]);
        }
    }

    std::cout << "Transport comparison benchmark: cpp-httplib vs. Boost.Beast vs. Proxygen"
#if defined(KYTHIRA_BENCH_HAS_GRPC)
                 " vs. gRPC"
#endif
                 "\n";
    std::cout << "RequestVote RPC round trip, " << warmup_iterations << " warmup + "
              << measured_iterations
              << " measured iterations, single connection reused throughout.\n";

    std::vector<scenario_result> results;
    results.push_back(bench_cpp_httplib(28090));
    results.push_back(bench_beast(28091));
    results.push_back(bench_proxygen(28092));
#if defined(KYTHIRA_BENCH_HAS_GRPC)
    results.push_back(bench_grpc());
#endif
    print_table(results);

    // Requirement 17.1: generic bridge vs. Folly fast path, same RPC shape,
    // same build -- a separate table since this is a within-Proxygen
    // comparison, not a cross-transport one like the results above.
    std::cout << "Proxygen: generic bridge vs. Folly fast path (Requirement 17.1)\n";
    std::vector<scenario_result> path_results;
    path_results.push_back(bench_proxygen_generic_bridge(28093));
    path_results.push_back(bench_proxygen(28094));
    print_table(path_results);

    // Requirement 17.3: the same generic-bridge-vs-fast-path comparison,
    // but with a large (1 MiB, install_snapshot-sized) body -- turns the
    // Introduction's zero-copy folly::IOBuf claim into a measured result
    // rather than an unmeasured architectural expectation.
    std::cout << "Proxygen: 1 MiB install_snapshot body, generic bridge vs. fast path "
                 "(Requirement 17.3)\n";
    std::vector<scenario_result> large_body_results;
    large_body_results.push_back(
        bench_proxygen_large_snapshot_body(28095, /*use_fast_path=*/false));
    large_body_results.push_back(bench_proxygen_large_snapshot_body(28096, /*use_fast_path=*/true));
#if defined(KYTHIRA_BENCH_HAS_GRPC)
    // Task 13.4's large-body row, in this table rather than a fourth one:
    // the question a reader has is "against the best HTTP path", and this
    // is the table holding that comparison at the same body size.
    large_body_results.push_back(bench_grpc_large_snapshot_body());
#endif
    print_table(large_body_results);

    return 0;
}
