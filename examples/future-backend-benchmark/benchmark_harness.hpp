#pragma once

/// @file benchmark_harness.hpp
/// @brief Backend-generic performance benchmark scenarios comparing
///     `kythira::Future<T>` (Folly, `include/raft/future.hpp`) against
///     `kythira::stdexec_backend::Future<T>` (`include/raft/future_stdexec.hpp`).
///
/// See `.kiro/specs/future-backend-performance-benchmark/` for the full
/// design. Every scenario below is a single function template parameterized
/// on a `future_backend_traits`-satisfying `Traits` type; there is exactly
/// one implementation of each scenario, instantiated once per backend, so
/// the two backends' numbers can never silently compare two different
/// operations (see design.md's rationale).

#include <concepts/future.hpp>
#include <raft/future.hpp>
#include <raft/future_stdexec.hpp>

#if defined(KYTHIRA_HAS_STDEXEC)
#include <exec/single_thread_context.hpp>
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace kythira::benchmark {

// ── Do-not-optimize sink (Requirement 3.4, Property 7) ─────────────────────
//
// Prevents an optimizing compiler from proving a measured value is dead and
// eliminating the loop that produces it. Takes the value's address into an
// opaque inline-asm block rather than the value itself, so it works
// uniformly regardless of the value's size or triviality (int, std::string,
// a multi-field struct, a std::vector<Try<T>> fan-in result, ...).
template<typename T> inline auto do_not_optimize(const T& value) -> void {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(std::addressof(value)) : "memory");
#else
    static volatile const void* sink;
    sink = std::addressof(value);
#endif
}

// ── BenchConfig / BenchmarkResult (Requirement 1.3, 1.4) ────────────────────

struct BenchConfig {
    std::size_t warmup_iterations = 1000;
    std::size_t measured_iterations = 100000;
    std::size_t fan_in_n = 100;   // collectAll/collectAny width
    std::size_t chain_depth = 5;  // thenValue chain depth
    std::chrono::milliseconds delay{10};
};

struct BenchmarkResult {
    std::string scenario_name;
    std::string backend_name;  // "folly" or "stdexec"
    std::size_t operations = 0;
    std::chrono::nanoseconds total_duration{0};
    double ops_per_second = 0.0;
    // Populated only for latency-style scenarios (Requirement 3.3); left at
    // zero for pure-throughput scenarios (caller distinguishes the two by
    // scenario identity, not by inspecting these fields).
    std::chrono::nanoseconds p50{0};
    std::chrono::nanoseconds p95{0};
    std::chrono::nanoseconds p99{0};
    std::string notes;
};

// ── Result helpers (Requirement 3.2, 3.3) ───────────────────────────────────

inline auto make_throughput_result(std::string scenario_name, std::string backend_name,
                                   std::size_t operations, std::chrono::nanoseconds total_duration,
                                   std::string notes = {}) -> BenchmarkResult {
    BenchmarkResult r;
    r.scenario_name = std::move(scenario_name);
    r.backend_name = std::move(backend_name);
    r.operations = operations;
    r.total_duration = total_duration;
    r.ops_per_second =
        total_duration.count() == 0
            ? 0.0
            : (static_cast<double>(operations) * 1e9) / static_cast<double>(total_duration.count());
    r.notes = std::move(notes);
    return r;
}

// Nearest-rank percentile over sorted samples, matching the convention
// already used by doc/raft_performance_benchmarking.md's P50/P95/P99
// sections.
inline auto make_latency_result(std::string scenario_name, std::string backend_name,
                                std::vector<std::chrono::nanoseconds> samples,
                                std::string notes = {}) -> BenchmarkResult {
    std::sort(samples.begin(), samples.end());
    BenchmarkResult r;
    r.scenario_name = std::move(scenario_name);
    r.backend_name = std::move(backend_name);
    r.operations = samples.size();
    r.total_duration = std::accumulate(samples.begin(), samples.end(), std::chrono::nanoseconds{0});
    r.ops_per_second = (samples.empty() || r.total_duration.count() == 0)
                           ? 0.0
                           : (static_cast<double>(samples.size()) * 1e9) /
                                 static_cast<double>(r.total_duration.count());
    if (!samples.empty()) {
        auto pick = [&](double quantile) {
            auto idx = static_cast<std::size_t>(quantile * static_cast<double>(samples.size() - 1));
            return samples[idx];
        };
        r.p50 = pick(0.50);
        r.p95 = pick(0.95);
        r.p99 = pick(0.99);
    }
    r.notes = std::move(notes);
    return r;
}

// ── Backend traits (Requirement 1.4) ────────────────────────────────────────

struct folly_backend_traits {
    template<typename T> using future_type = kythira::Future<T>;
    template<typename T> using promise_type = kythira::Promise<T>;
    using factory_type = kythira::FutureFactory;
    using collector_type = kythira::FutureCollector;
    static constexpr const char* name = "folly";

    // Owns a warmed, ready-to-use single worker thread for `via` scenarios;
    // constructed once per benchmark run, never inside a timed region
    // (Requirement 3.5).
    struct scheduler_handle {
        std::shared_ptr<folly::CPUThreadPoolExecutor> executor;
        [[nodiscard]] auto get() const -> folly::Executor* { return executor.get(); }
    };

    static auto make_scheduler() -> scheduler_handle {
        return scheduler_handle{std::make_shared<folly::CPUThreadPoolExecutor>(1)};
    }
};

#if defined(KYTHIRA_HAS_STDEXEC)

struct stdexec_backend_traits {
    template<typename T> using future_type = kythira::stdexec_backend::Future<T>;
    template<typename T> using promise_type = kythira::stdexec_backend::Promise<T>;
    using factory_type = kythira::stdexec_backend::FutureFactory;
    using collector_type = kythira::stdexec_backend::FutureCollector;
    static constexpr const char* name = "stdexec";

    // scheduler_handle here is the traits-level wrapper (owns the worker
    // thread context); Traits::scheduler_handle::get() returns the pointer
    // type each backend's Future<T>::via() actually expects — a raw
    // folly::Executor* above, a kythira::stdexec_backend::scheduler_handle*
    // here.
    struct scheduler_handle {
        std::shared_ptr<exec::single_thread_context> context;
        std::shared_ptr<kythira::stdexec_backend::scheduler_handle> handle;
        [[nodiscard]] auto get() const -> kythira::stdexec_backend::scheduler_handle* {
            return handle.get();
        }
    };

    static auto make_scheduler() -> scheduler_handle {
        auto context = std::make_shared<exec::single_thread_context>();
        auto handle =
            std::make_shared<kythira::stdexec_backend::scheduler_handle>(context->get_scheduler());
        return scheduler_handle{std::move(context), std::move(handle)};
    }
};

#endif  // KYTHIRA_HAS_STDEXEC

// ── future_backend_traits concept (Requirement 1.1) ─────────────────────────
//
// Constrains every scenario template to a Traits type whose future/promise/
// factory/collector types actually satisfy this project's backend-neutral
// concepts (include/concepts/future.hpp) — the same concepts each real
// backend already statically asserts itself against.
template<typename Traits>
concept future_backend_traits =
    requires {
        typename Traits::template future_type<int>;
        typename Traits::template promise_type<int>;
        typename Traits::factory_type;
        typename Traits::collector_type;
        { Traits::name } -> std::convertible_to<const char*>;
        Traits::make_scheduler();
    } && kythira::future<typename Traits::template future_type<int>, int> &&
    kythira::promise<typename Traits::template promise_type<int>, int> &&
    kythira::future_factory<typename Traits::factory_type> &&
    kythira::future_collector<typename Traits::collector_type,
                              typename Traits::template future_type<int>>;

// ── Multi-field payload (Requirement 2.1's "struct" sub-scenario) ──────────

struct benchmark_payload {
    int id = 0;
    double value = 0.0;
    std::string label;

    auto operator==(const benchmark_payload&) const -> bool = default;
};

inline auto make_benchmark_payload(std::size_t i) -> benchmark_payload {
    return benchmark_payload{static_cast<int>(i), static_cast<double>(i) * 0.5,
                             "payload_" + std::to_string(i)};
}

// ── Scenario: creation + resolution (Requirement 2.1) ───────────────────────
//
// One function template, instantiated for int/std::string/benchmark_payload
// via three call sites (Task 3) rather than three near-duplicate functions.
template<future_backend_traits Traits, typename ValueType, typename MakeValue>
auto bench_creation_resolution(const BenchConfig& cfg, const std::string& label,
                               MakeValue&& make_value) -> BenchmarkResult {
    auto run_once = [&](std::size_t i) {
        auto f = Traits::factory_type::makeFuture(make_value(i));
        auto result = std::move(f).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once(i);
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once(i);
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("creation_resolution_" + label, Traits::name,
                                  cfg.measured_iterations, end - start);
}

// ── Scenario: same-thread promise round trip (Requirement 2.2) ─────────────
template<future_backend_traits Traits>
auto bench_same_thread_promise(const BenchConfig& cfg) -> BenchmarkResult {
    using promise_t = typename Traits::template promise_type<int>;

    auto run_once = [](std::size_t i) {
        promise_t promise;
        auto future = promise.getFuture();
        promise.setValue(static_cast<int>(i));
        auto result = std::move(future).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once(i);
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once(i);
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("same_thread_promise", Traits::name, cfg.measured_iterations,
                                  end - start);
}

// ── Scenario: cross-thread promise fulfillment (Requirement 2.3) ───────────
//
// Spawns a real std::thread per iteration deliberately: this scenario
// exists specifically to measure the cross-thread wake-up path, isolating
// the single_shot_channel's mutex (stdexec backend) from folly::Promise's
// internal core (Folly backend). Thread-spawn cost is common to both
// backends' measured samples and cancels out of the *relative* comparison
// even though it inflates both backends' absolute per-iteration latency
// (documented in doc/future_backend_performance_comparison.md, Requirement
// 6.3) — a reader should not mistake "absolute latency includes a
// std::thread spawn" for a backend-specific cost.
template<future_backend_traits Traits>
auto bench_cross_thread_promise(const BenchConfig& cfg) -> BenchmarkResult {
    using promise_t = typename Traits::template promise_type<int>;

    auto run_once = [] {
        promise_t promise;
        auto future = promise.getFuture();
        auto start = std::chrono::steady_clock::now();
        std::thread fulfiller([&promise] { promise.setValue(42); });
        auto result = std::move(future).get();
        auto end = std::chrono::steady_clock::now();
        fulfiller.join();
        do_not_optimize(result);
        return end - start;
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    std::vector<std::chrono::nanoseconds> latencies;
    latencies.reserve(cfg.measured_iterations);
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        latencies.push_back(run_once());
    }

    return make_latency_result(
        "cross_thread_promise", Traits::name, std::move(latencies),
        "includes per-iteration std::thread spawn cost, common to both backends");
}

// ── Scenario: thenValue chain at configurable depth (Requirement 2.4) ──────
template<future_backend_traits Traits>
auto bench_thenvalue_chain(const BenchConfig& cfg, std::size_t depth) -> BenchmarkResult {
    auto run_once = [&] {
        auto f = Traits::factory_type::makeFuture(0);
        for (std::size_t d = 0; d < depth; ++d) {
            f = std::move(f).thenValue([](int v) { return v + 1; });
        }
        auto result = std::move(f).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once();
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("thenvalue_chain_depth" + std::to_string(depth), Traits::name,
                                  cfg.measured_iterations, end - start);
}

// ── Scenario: thenError (Requirement 2.5) ───────────────────────────────────
template<future_backend_traits Traits>
auto bench_thenerror(const BenchConfig& cfg) -> BenchmarkResult {
    auto run_once = [] {
        auto ex = std::make_exception_ptr(std::runtime_error("benchmark error"));
        auto f = Traits::factory_type::template makeExceptionalFuture<int>(ex);
        auto handled = std::move(f).thenError([](std::exception_ptr) { return -1; });
        auto result = std::move(handled).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once();
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("thenerror", Traits::name, cfg.measured_iterations, end - start);
}

// ── Scenario: via(scheduler) (Requirement 2.6) ──────────────────────────────
template<future_backend_traits Traits>
auto bench_via_scheduler(const BenchConfig& cfg) -> BenchmarkResult {
    // Constructed once, outside the timed region (Requirement 3.5).
    auto sched = Traits::make_scheduler();

    auto run_once = [&] {
        auto f = Traits::factory_type::makeFuture(0);
        auto viaed = std::move(f).via(sched.get());
        auto result = std::move(viaed).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once();
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("via_scheduler", Traits::name, cfg.measured_iterations,
                                  end - start);
}

// ── Scenario: collectAll fan-in (Requirement 2.7) ───────────────────────────
template<future_backend_traits Traits>
auto bench_collect_all(const BenchConfig& cfg, std::size_t n) -> BenchmarkResult {
    using future_t = typename Traits::template future_type<int>;

    auto run_once = [&] {
        std::vector<future_t> futures;
        futures.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            futures.push_back(Traits::factory_type::makeFuture(static_cast<int>(i)));
        }
        auto collected = Traits::collector_type::collectAll(std::move(futures));
        auto result = std::move(collected).get();
        do_not_optimize(result);
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        run_once();
    }
    auto end = std::chrono::steady_clock::now();

    return make_throughput_result("collect_all_n" + std::to_string(n), Traits::name,
                                  cfg.measured_iterations, end - start);
}

// ── Scenario: collectAny (Requirement 2.8) ──────────────────────────────────
//
// Index 0 is arranged to resolve first (already-ready future); the rest
// are delayed by cfg.delay so collectAny has an actual race to settle
// rather than N simultaneously-ready futures whose "winner" would be an
// implementation-defined tie-break.
template<future_backend_traits Traits>
auto bench_collect_any(const BenchConfig& cfg, std::size_t n) -> BenchmarkResult {
    using future_t = typename Traits::template future_type<int>;

    auto run_once = [&] {
        std::vector<future_t> futures;
        futures.reserve(n);
        futures.push_back(Traits::factory_type::makeFuture(0));
        for (std::size_t i = 1; i < n; ++i) {
            futures.push_back(
                std::move(Traits::factory_type::makeFuture(static_cast<int>(i))).delay(cfg.delay));
        }
        auto start = std::chrono::steady_clock::now();
        auto collected = Traits::collector_type::collectAny(std::move(futures));
        auto result = std::move(collected).get();
        auto end = std::chrono::steady_clock::now();
        do_not_optimize(result);
        return end - start;
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    std::vector<std::chrono::nanoseconds> latencies;
    latencies.reserve(cfg.measured_iterations);
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        latencies.push_back(run_once());
    }

    return make_latency_result("collect_any_n" + std::to_string(n), Traits::name,
                               std::move(latencies));
}

// ── Scenario: delay/within scheduling overhead (Requirement 2.9) ───────────
//
// Reports elapsed time minus the nominal delay itself (the scheduling
// overhead), not the delay duration — a reader should not mistake "this
// backend's delay() takes cfg.delay" (true of both, uninteresting) for
// the actual measured quantity (each backend's overhead atop that floor).
enum class delay_or_within {
    delay,
    within
};

template<future_backend_traits Traits>
auto bench_delay_within(const BenchConfig& cfg, delay_or_within which) -> BenchmarkResult {
    auto run_once = [&] {
        auto start = std::chrono::steady_clock::now();
        auto f = Traits::factory_type::makeFuture(0);
        auto delayed = (which == delay_or_within::delay) ? std::move(f).delay(cfg.delay)
                                                         : std::move(f).within(cfg.delay);
        auto result = std::move(delayed).get();
        auto end = std::chrono::steady_clock::now();
        do_not_optimize(result);
        auto elapsed = end - start;
        return elapsed > cfg.delay ? elapsed - cfg.delay : std::chrono::nanoseconds{0};
    };

    for (std::size_t i = 0; i < cfg.warmup_iterations; ++i) {
        run_once();
    }

    std::vector<std::chrono::nanoseconds> overhead_samples;
    overhead_samples.reserve(cfg.measured_iterations);
    for (std::size_t i = 0; i < cfg.measured_iterations; ++i) {
        overhead_samples.push_back(run_once());
    }

    const char* scenario_name =
        which == delay_or_within::delay ? "delay_overhead" : "within_overhead";
    return make_latency_result(scenario_name, Traits::name, std::move(overhead_samples),
                               "value is scheduling overhead atop the nominal delay, "
                               "not the delay duration itself");
}

}  // namespace kythira::benchmark
