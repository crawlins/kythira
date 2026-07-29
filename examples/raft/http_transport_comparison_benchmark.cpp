// A real, measured throughput/latency comparison of this project's three
// HTTP transports -- cpp-httplib (include/raft/http_transport.hpp),
// Boost.Beast (include/raft/beast_http_transport.hpp), and Proxygen
// (include/raft/proxygen_http_transport.hpp) -- running the *same*
// RequestVote RPC workload against each in turn, all in one process. See
// doc/http_transport_performance_comparison.md for the methodology write-up
// and the measured numbers this program produced.
//
// This is a comparison, not a sanity floor: it does not gate CI on any
// transport beating another by a specific margin (matching
// .kiro/specs/future-backend-performance-benchmark/'s own established
// distinction for the analogous Folly-vs-stdexec comparison) -- relative
// performance is expected to shift across hardware/compiler/library
// versions. Only built when both KYTHIRA_BUILD_BOOST_BEAST_TRANSPORT and
// KYTHIRA_BUILD_PROXYGEN_TRANSPORT are enabled (examples/raft/CMakeLists.txt),
// since it needs all three transports available in the same binary.

#include <raft/http_transport.hpp>
#include <raft/http_transport_impl.hpp>
#include <raft/beast_http_transport.hpp>
#include <raft/beast_http_transport_impl.hpp>
#include <raft/proxygen_http_transport.hpp>
#include <raft/proxygen_http_transport_impl.hpp>
#include <raft/json_serializer.hpp>
#include <raft/metrics.hpp>
#include <raft/executor_default.hpp>

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

    std::cout << "HTTP transport comparison benchmark: cpp-httplib vs. Boost.Beast vs. Proxygen\n";
    std::cout << "RequestVote RPC round trip, " << warmup_iterations << " warmup + "
              << measured_iterations
              << " measured iterations, single connection reused throughout.\n";

    std::vector<scenario_result> results;
    results.push_back(bench_cpp_httplib(28090));
    results.push_back(bench_beast(28091));
    results.push_back(bench_proxygen(28092));

    print_table(results);
    return 0;
}
