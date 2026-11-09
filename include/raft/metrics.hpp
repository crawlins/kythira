// Copyright (c) 2026 Clark Rawlins
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file metrics.hpp
/// @brief Metrics-collection concept and a zero-overhead no-op implementation.

#include <chrono>
#include <concepts>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace kythira {

/// @brief Concept for a metrics-collection back-end.
///
/// Implementations may forward data to CloudWatch, Prometheus, StatsD, etc.
/// All methods must be non-blocking; defer I/O to a background emitter.
///
/// @tparam M Concrete metrics type.
template<typename M>
concept metrics = requires(M metric, std::string_view name, std::string_view dimension_name,
                           std::string_view dimension_value, std::int64_t count,
                           std::chrono::nanoseconds duration, double value) {
    /// Set the metric name; called once before any recording method.
    { metric.set_metric_name(name) } -> std::same_as<void>;
    /// Attach a named dimension (tag) to this metric.
    { metric.add_dimension(dimension_name, dimension_value) } -> std::same_as<void>;

    /// Increment the counter by one.
    { metric.add_one() } -> std::same_as<void>;
    /// Increment the counter by `count`.
    { metric.add_count(count) } -> std::same_as<void>;
    /// Record a latency sample.
    { metric.add_duration(duration) } -> std::same_as<void>;
    /// Record an arbitrary floating-point gauge value.
    { metric.add_value(value) } -> std::same_as<void>;

    /// Flush the current metric to the back-end.
    { metric.emit() } -> std::same_as<void>;
};

/// @brief Zero-cost no-op metrics implementation for testing and production environments
///        where metrics are unwanted.
///
/// All methods are inlined and compile away to nothing.
class noop_metrics {
public:
    auto set_metric_name([[maybe_unused]] std::string_view name) -> void {}
    auto add_dimension([[maybe_unused]] std::string_view dimension_name,
                       [[maybe_unused]] std::string_view dimension_value) -> void {}
    auto add_one() -> void {}
    auto add_count([[maybe_unused]] std::int64_t count) -> void {}
    auto add_duration([[maybe_unused]] std::chrono::nanoseconds duration) -> void {}
    auto add_value([[maybe_unused]] double value) -> void {}
    auto emit() -> void {}
};

static_assert(metrics<noop_metrics>, "noop_metrics must satisfy metrics concept");

/// @brief Makes a metrics back-end safe to build and emit from many threads.
///
/// The concept builds one metric at a time on one object: `set_metric_name`,
/// then dimensions and values, then `emit`. A Raft node emits from RPC
/// handlers, timers and future continuations at once, and two threads
/// building at the same time interleave on the back-end's pending metric:
/// mixed dimensions at best, a corrupted container at worst (a SIGSEGV in a
/// test recorder was how this showed). This wrapper gives each thread its
/// own pending metric, recorded call by call, and `emit` replays it into the
/// back-end under a mutex. A thread that starts a metric and never emits it
/// leaves nothing behind but its own buffer, which the next
/// `set_metric_name` clears.
template<metrics M> class serialized_metrics {
public:
    serialized_metrics() = default;
    explicit serialized_metrics(M inner) : _inner(std::move(inner)) {}

    auto set_metric_name(std::string_view name) -> void {
        auto& p = pending();
        p.name.assign(name);
        p.ops.clear();
    }
    auto add_dimension(std::string_view dimension_name, std::string_view dimension_value) -> void {
        pending().ops.emplace_back(
            dimension{std::string(dimension_name), std::string(dimension_value)});
    }
    auto add_one() -> void { pending().ops.emplace_back(one{}); }
    auto add_count(std::int64_t count) -> void { pending().ops.emplace_back(count); }
    auto add_duration(std::chrono::nanoseconds duration) -> void {
        pending().ops.emplace_back(duration);
    }
    auto add_value(double value) -> void { pending().ops.emplace_back(value); }

    auto emit() -> void {
        auto& p = pending();
        {
            const std::lock_guard lock(*_mu);
            _inner.set_metric_name(p.name);
            for (const auto& op : p.ops) {
                std::visit([this](const auto& v) { replay(v); }, op);
            }
            _inner.emit();
        }
        p.name.clear();
        p.ops.clear();
    }

    /// The wrapped back-end. Only for reading its state (tests); calling its
    /// builder methods directly bypasses the serialization.
    [[nodiscard]] auto inner() -> M& { return _inner; }
    [[nodiscard]] auto inner() const -> const M& { return _inner; }

private:
    struct dimension {
        std::string name;
        std::string value;
    };
    struct one {};
    using op = std::variant<dimension, one, std::int64_t, std::chrono::nanoseconds, double>;
    struct pending_metric {
        std::string name;
        std::vector<op> ops;
    };

    auto replay(const dimension& d) -> void { _inner.add_dimension(d.name, d.value); }
    auto replay(const one& /*unused*/) -> void { _inner.add_one(); }
    auto replay(std::int64_t c) -> void { _inner.add_count(c); }
    auto replay(std::chrono::nanoseconds d) -> void { _inner.add_duration(d); }
    auto replay(double v) -> void { _inner.add_value(v); }

    // This thread's pending metric for this object. Keyed by address: an
    // object is not moved between set_metric_name and emit, and a stale
    // entry left by a destroyed object is cleared by the next
    // set_metric_name at that address.
    auto pending() -> pending_metric& {
        thread_local std::unordered_map<const void*, pending_metric> by_object;
        return by_object[this];
    }

    M _inner{};
    // Held by pointer so the wrapper stays movable like the back-end.
    std::unique_ptr<std::mutex> _mu = std::make_unique<std::mutex>();
};

/// What a node holds its metrics as: `noop_metrics` stays zero-cost, every
/// other back-end is serialized.
template<metrics M>
using node_metrics_t =
    std::conditional_t<std::is_same_v<M, noop_metrics>, noop_metrics, serialized_metrics<M>>;

static_assert(metrics<serialized_metrics<noop_metrics>>);

}  // namespace kythira
