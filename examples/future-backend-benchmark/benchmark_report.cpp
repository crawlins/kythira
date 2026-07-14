/// @file benchmark_report.cpp
/// @brief Standalone comparison report generator for the Folly vs. stdexec
///     future backend benchmark harness (`.kiro/specs/
///     future-backend-performance-benchmark/`). Modeled on
///     `examples/performance_benchmark_report.cpp`'s `PerformanceBenchmark`
///     class shape. Not CTest-registered — run manually by a developer,
///     using report-quality iteration counts rather than the CTest suite's
///     CI-fast counts (`tests/future_backend_benchmark_test.cpp`).

#include "benchmark_harness.hpp"

#include <folly/init/Init.h>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace kythira::benchmark;

namespace {

auto make_timestamp() -> std::string {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf{};
    localtime_r(&time_t_now, &tm_buf);
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%Y%m%d_%H%M%S");
    return oss.str();
}

auto json_escape(const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

// One implementation of "run every scenario for a backend", instantiated
// once per Traits — the same reasoning as every individual scenario
// template: a hand-duplicated "run stdexec's scenarios" function risks
// silently drifting from "run folly's scenarios" (different scenario
// list, different iteration counts) until the two backends' reports are no
// longer describing the same set of operations.
template<future_backend_traits Traits>
auto run_all_scenarios_for(const BenchConfig& cfg, std::size_t latency_iterations)
    -> std::vector<BenchmarkResult> {
    std::vector<BenchmarkResult> results;

    results.push_back(bench_creation_resolution<Traits, int>(
        cfg, "int", [](std::size_t i) { return static_cast<int>(i); }));
    results.push_back(bench_creation_resolution<Traits, std::string>(
        cfg, "string", [](std::size_t i) { return "benchmark_string_" + std::to_string(i); }));
    results.push_back(bench_creation_resolution<Traits, benchmark_payload>(cfg, "struct",
                                                                           make_benchmark_payload));

    results.push_back(bench_same_thread_promise<Traits>(cfg));

    for (std::size_t depth : {1UL, 5UL, 20UL}) {
        results.push_back(bench_thenvalue_chain<Traits>(cfg, depth));
    }

    results.push_back(bench_thenerror<Traits>(cfg));
    results.push_back(bench_via_scheduler<Traits>(cfg));

    for (std::size_t n : {10UL, 100UL, 1000UL}) {
        results.push_back(bench_collect_all<Traits>(cfg, n));
    }

    // Thread-spawn/timer-bound scenarios: report-quality throughput
    // iteration counts (potentially 100000+) would take far too long once
    // each iteration incurs a real std::thread spawn or a cfg.delay wait,
    // so these use a much smaller, dedicated iteration count.
    BenchConfig latency_cfg = cfg;
    latency_cfg.measured_iterations = latency_iterations;
    latency_cfg.warmup_iterations = std::min(cfg.warmup_iterations, latency_iterations);

    results.push_back(bench_cross_thread_promise<Traits>(latency_cfg));
    results.push_back(bench_collect_any<Traits>(latency_cfg, cfg.fan_in_n));
    results.push_back(bench_delay_within<Traits>(latency_cfg, delay_or_within::delay));
    results.push_back(bench_delay_within<Traits>(latency_cfg, delay_or_within::within));

    return results;
}

}  // namespace

class FutureBackendComparisonReport {
public:
    explicit FutureBackendComparisonReport(BenchConfig cfg, std::size_t latency_iterations)
        : cfg_(cfg), latency_iterations_(latency_iterations) {}

    void run_all_scenarios() {
        std::cout << "Running folly backend scenarios...\n";
        folly_results_ = run_all_scenarios_for<folly_backend_traits>(cfg_, latency_iterations_);
#if defined(KYTHIRA_HAS_STDEXEC)
        std::cout << "Running stdexec backend scenarios...\n";
        stdexec_results_ = run_all_scenarios_for<stdexec_backend_traits>(cfg_, latency_iterations_);
#else
        std::cout << "stdexec backend not available in this build (stdexec_FOUND was false) — "
                     "reporting folly-only results.\n";
#endif
    }

    void print_comparison_table(std::ostream& os) const {
        os << "\n=== Future Backend Comparison Report ===\n";
        os << std::left << std::setw(30) << "Scenario" << std::setw(10) << "Backend"
           << std::setw(18) << "Ops/sec" << std::setw(12) << "P50(ns)" << std::setw(12) << "P95(ns)"
           << std::setw(12) << "P99(ns)" << "Notes\n";
        os << std::string(120, '-') << "\n";

        for (const auto& r : folly_results_) {
            print_row(os, r);
        }
#if defined(KYTHIRA_HAS_STDEXEC)
        for (const auto& r : stdexec_results_) {
            print_row(os, r);
        }
#endif
        os << std::string(120, '-') << "\n";

#if defined(KYTHIRA_HAS_STDEXEC)
        os << "\n=== Delta (stdexec vs. folly, ops/sec) ===\n";
        for (std::size_t i = 0; i < folly_results_.size() && i < stdexec_results_.size(); ++i) {
            const auto& f = folly_results_[i];
            const auto& s = stdexec_results_[i];
            double delta_pct =
                f.ops_per_second == 0.0
                    ? 0.0
                    : ((s.ops_per_second - f.ops_per_second) / f.ops_per_second) * 100.0;
            os << std::left << std::setw(30) << f.scenario_name << std::showpos << std::fixed
               << std::setprecision(1) << delta_pct << "%" << std::noshowpos << std::defaultfloat
               << "\n";
        }
#else
        // Requirement 5.3 / Property 6: no delta column, no placeholder
        // rows — a reader should not mistake "not measured" for "measured
        // as zero".
        os << "\n(stdexec backend not available in this build — comparison column omitted)\n";
#endif
    }

    void write_csv(const std::filesystem::path& path) const {
        std::ofstream f(path);
        f << "scenario,backend,operations,total_duration_ns,ops_per_second,p50_ns,p95_ns,p99_ns,"
             "notes\n";
        auto write_row = [&](const BenchmarkResult& r) {
            f << r.scenario_name << ',' << r.backend_name << ',' << r.operations << ','
              << r.total_duration.count() << ',' << r.ops_per_second << ',' << r.p50.count() << ','
              << r.p95.count() << ',' << r.p99.count() << ",\"" << r.notes << "\"\n";
        };
        for (const auto& r : folly_results_) {
            write_row(r);
        }
#if defined(KYTHIRA_HAS_STDEXEC)
        for (const auto& r : stdexec_results_) {
            write_row(r);
        }
#endif
    }

    void write_json(const std::filesystem::path& path) const {
        std::ofstream f(path);
        f << "[\n";
        bool first = true;
        auto write_entry = [&](const BenchmarkResult& r) {
            if (!first) {
                f << ",\n";
            }
            first = false;
            f << "  {\n"
              << "    \"scenario\": \"" << json_escape(r.scenario_name) << "\",\n"
              << "    \"backend\": \"" << json_escape(r.backend_name) << "\",\n"
              << "    \"operations\": " << r.operations << ",\n"
              << "    \"total_duration_ns\": " << r.total_duration.count() << ",\n"
              << "    \"ops_per_second\": " << r.ops_per_second << ",\n"
              << "    \"p50_ns\": " << r.p50.count() << ",\n"
              << "    \"p95_ns\": " << r.p95.count() << ",\n"
              << "    \"p99_ns\": " << r.p99.count() << ",\n"
              << "    \"notes\": \"" << json_escape(r.notes) << "\"\n"
              << "  }";
        };
        for (const auto& r : folly_results_) {
            write_entry(r);
        }
#if defined(KYTHIRA_HAS_STDEXEC)
        for (const auto& r : stdexec_results_) {
            write_entry(r);
        }
#endif
        f << "\n]\n";
    }

private:
    static void print_row(std::ostream& os, const BenchmarkResult& r) {
        os << std::left << std::setw(30) << r.scenario_name << std::setw(10) << r.backend_name
           << std::setw(18) << static_cast<std::int64_t>(r.ops_per_second) << std::setw(12)
           << r.p50.count() << std::setw(12) << r.p95.count() << std::setw(12) << r.p99.count()
           << r.notes << "\n";
    }

    BenchConfig cfg_;
    std::size_t latency_iterations_;
    std::vector<BenchmarkResult> folly_results_;
    std::vector<BenchmarkResult> stdexec_results_;
};

auto main(int argc, char** argv) -> int {
    try {
        BenchConfig cfg;
        std::size_t latency_iterations = 200;

        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--iterations" && i + 1 < argc) {
                cfg.measured_iterations = std::stoul(argv[++i]);
            } else if (arg == "--warmup" && i + 1 < argc) {
                cfg.warmup_iterations = std::stoul(argv[++i]);
            } else {
                std::cerr << "Usage: " << argv[0] << " [--iterations N] [--warmup N]\n";
                return 1;
            }
        }

        // A synthetic argc/argv (rather than the real one) keeps folly's
        // gflags-based parsing from tripping over --iterations/--warmup,
        // which are this program's own flags, not folly's. Needed because
        // bench_delay_within/bench_collect_any use Future<T>::delay()/
        // within(), which require folly::Timekeeper's singleton finalized
        // before use (same reasoning as tests/future_backend_benchmark_test.cpp's
        // FollyInitFixture).
        int folly_argc = 1;
        char* folly_argv_data[] = {const_cast<char*>("future_backend_benchmark_report"), nullptr};
        char** folly_argv = folly_argv_data;
        folly::Init init(&folly_argc, &folly_argv);

        FutureBackendComparisonReport report(cfg, latency_iterations);
        report.run_all_scenarios();
        report.print_comparison_table(std::cout);

        std::filesystem::create_directories("test_results");
        auto timestamp = make_timestamp();
        auto csv_path = std::filesystem::path("test_results") /
                        ("future_backend_benchmark_" + timestamp + ".csv");
        auto json_path = std::filesystem::path("test_results") /
                         ("future_backend_benchmark_" + timestamp + ".json");
        report.write_csv(csv_path);
        report.write_json(json_path);

        std::cout << "\nWrote " << csv_path.string() << " and " << json_path.string() << "\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Benchmark report failed: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << "Benchmark report failed with unknown exception\n";
        return 1;
    }
}
